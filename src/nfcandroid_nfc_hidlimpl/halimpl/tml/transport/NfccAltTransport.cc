/******************************************************************************
 *  Copyright 2021 NXP
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  The original Work has been changed by PIONIX GmbH in 11-2024
 *
 ******************************************************************************/

#include <errno.h>
#include <fcntl.h>
#ifdef ANDROID
#include <hardware/nfc.h>
#endif
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <NfccI2cTransport.h>
#include <NfccAltTransport.h>
#include <phNfcStatus.h>
#include <phNxpConfig.h>
#include <phNxpLog.h>
#include <string.h>
#include "phNxpNciHal_utils.h"
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

#define CRC_LEN 2
#define NORMAL_MODE_HEADER_LEN 3
#define FW_DNLD_HEADER_LEN 2
#define FW_DNLD_LEN_OFFSET 1
#define NORMAL_MODE_LEN_OFFSET 2
#define FRAGMENTSIZE_MAX PHNFC_I2C_FRAGMENT_SIZE
extern phTmlNfc_i2cfragmentation_t fragmentation_enabled;
extern phTmlNfc_Context_t* gpphTmlNfc_Context;

NfccAltTransport::NfccAltTransport() {
}

/*******************************************************************************
**
** Function         Reset
**
** Description      Reset NFCC device, using VEN pin
**
** Parameters       pDevHandle     - valid device handle
**                  eType          - reset level
**
** Returns           0   - reset operation success
**                  -1   - reset operation failure
**
*******************************************************************************/
int NfccAltTransport::NfccReset(void* pDevHandle, NfccResetType eType) {
  int ret = -1;
  NXPLOG_TML_D("%s, VEN eType %ld", __func__, eType);

  if (NULL == pDevHandle) {
    return -1;
  }
  switch (eType) {
    case MODE_POWER_OFF:
      gpio_set_fwdl(0);
      gpio_set_ven(0);
      break;
    case MODE_POWER_ON:
      gpio_set_fwdl(0);
      gpio_set_ven(1);
      break;
    case MODE_FW_DWNLD_WITH_VEN:
      gpio_set_fwdl(1);
      gpio_set_ven(0);
      gpio_set_ven(1);
      break;
    case MODE_FW_DWND_HIGH:
      gpio_set_fwdl(1);
      break;
    case MODE_POWER_RESET:
      gpio_set_ven(0);
      gpio_set_ven(1);
      break;
    case MODE_FW_GPIO_LOW:
      gpio_set_fwdl(0);
      break;
    default:
      NXPLOG_TML_E("%s, VEN eType %ld", __func__, eType);
      return -1;
  }
  if ((eType != MODE_FW_DWNLD_WITH_VEN) && (eType != MODE_FW_DWND_HIGH)) {
    EnableFwDnldMode(false);
  }
  if ((eType == MODE_FW_DWNLD_WITH_VEN) || (eType == MODE_FW_DWND_HIGH)) {
    EnableFwDnldMode(true);
  }

  return ret;
}

/*******************************************************************************
**
** Function         EnableFwDnldMode
**
** Description      updates the state to Download mode
**
** Parameters       True/False
**
** Returns          None
*******************************************************************************/
void NfccAltTransport::EnableFwDnldMode(bool mode) { bFwDnldFlag = mode; }

/*******************************************************************************
**
** Function         IsFwDnldModeEnabled
**
** Description      Returns the current mode
**
** Parameters       none
**
** Returns           Current mode download/NCI
*******************************************************************************/
bool_t NfccAltTransport::IsFwDnldModeEnabled(void) { return bFwDnldFlag; }

/*******************************************************************************
**
** Function         SemPost
**
** Description      sem_post 2c_read / write
**
** Parameters       none
**
** Returns          none
*******************************************************************************/
void NfccAltTransport::SemPost() {
  int sem_val = 0;
  sem_getvalue(&mTxRxSemaphore, &sem_val);
  if (sem_val == 0) {
    sem_post(&mTxRxSemaphore);
  }
}

/*******************************************************************************
**
** Function         SemTimedWait
**
** Description      Timed sem_wait for avoiding i2c_read & write overlap
**
** Parameters       none
**
** Returns          Sem_wait return status
*******************************************************************************/
int NfccAltTransport::SemTimedWait() {
  NFCSTATUS status = NFCSTATUS_FAILED;
  long sem_timedout = 500 * 1000 * 1000;
  int s = 0;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += 0;
  ts.tv_nsec += sem_timedout;
  while ((s = sem_timedwait(&mTxRxSemaphore, &ts)) == -1 && errno == EINTR) {
    continue; /* Restart if interrupted by handler */
  }
  if (s != -1) {
    status = NFCSTATUS_SUCCESS;
  } else if (errno == ETIMEDOUT && s == -1) {
    NXPLOG_TML_E("%s :timed out errno = 0x%x", __func__, errno);
  }
  return status;
}

/*******************************************************************************
**
** Function         GetIrqState
**
** Description      Get state of IRQ GPIO
**
** Parameters       pDevHandle - valid device handle
**
** Returns          The state of IRQ line i.e. +ve if read is pending else Zer0.
**                  In the case of IOCTL error, it returns -ve value.
**
*******************************************************************************/
int NfccAltTransport::GetIrqState(void* pDevHandle) {
  (void)pDevHandle;

#ifdef USE_LIBGPIOD
  if (m_GpioDInUse) {
    return GetIrqStateLibGpioD();
  }
#endif

  return GetIrqStateSysFS();
}

int NfccAltTransport::GetIrqStateSysFS() {
  int ret = -1;

  NXPLOG_TML_D("%s Enter", __func__);
  int len;
  char buf[2];

  if (iInterruptFd < 0) {
    NXPLOG_TML_E("Error with interrupt-detect pin (%d)", iInterruptFd);
    return (-1);
  }

  // Seek to the start of the file
  lseek(iInterruptFd, SEEK_SET, 0);

  // Read the field_detect line
  len = read(iInterruptFd, buf, 2);

  if (len != 2) {
    NXPLOG_TML_E("Error with interrupt-detect pin (%s)", strerror(errno));
    return (0);
  }

  NXPLOG_TML_D("%s exit: state = %d", __func__, (buf[0] != '0'));
  return (buf[0] != '0');
}

int NfccAltTransport::verifyPin(int pin, int isoutput, int edge) {
  char buf[40];
  // Check if gpio pin has already been created
  int hasGpio = 0;
  NXPLOG_TML_D("%s Enter", __func__);
  sprintf(buf, "/sys/class/gpio/gpio%d", pin);
  NXPLOG_TML_D("Pin %s\n", buf);
  int fd = open(buf, O_RDONLY);
  if (fd < 0) {
    // Pin not exported yet
    NXPLOG_TML_D("Create pin %s\n", buf);
    if ((fd = open("/sys/class/gpio/export", O_WRONLY)) >= 0) {
      sprintf(buf, "%d", pin);
      if (write(fd, buf, strlen(buf)) == strlen(buf)) {
        hasGpio = 1;
        usleep(100 * 1000);
      }
    } else {
      NXPLOG_TML_E("open failed for /sys/class/gpio/export\n");
      return -1;
    }
  } else {
    NXPLOG_TML_E("System already has pin %s\n", buf);
    hasGpio = 1;
  }
  close(fd);

  if (hasGpio) {
    // Make sure it is an output
    sprintf(buf, "/sys/class/gpio/gpio%d/direction", pin);
    NXPLOG_TML_D("Direction %s\n", buf);
    fd = open(buf, O_WRONLY);
    if (fd < 0) {
      NXPLOG_TML_E("Could not open direction port '%s' (%s)", buf,
                   strerror(errno));
      return -1;
    } else {
      if (isoutput) {
        if (write(fd, "out", 3) == 3) {
          NXPLOG_TML_D("Pin %d now an output\n", pin);
        }
        close(fd);

        // Open pin and make sure it is off
        sprintf(buf, "/sys/class/gpio/gpio%d/value", pin);
        fd = open(buf, O_RDWR);
        if (fd < 0) {
        }
        close(fd);

        // Open pin and make sure it is off
        sprintf(buf, "/sys/class/gpio/gpio%d/value", pin);
        fd = open(buf, O_RDWR);
        if (fd < 0) {
          NXPLOG_TML_E("Could not open value port '%s' (%s)", buf,
                       strerror(errno));
          return -1;
        } else {
          if (write(fd, "0", 1) == 1) {
            NXPLOG_TML_D("Pin %d now off\n", pin);
          }
          return (fd);  // Success
        }
      } else {
        if (write(fd, "in", 2) == 2) {
          NXPLOG_TML_D("Pin %d now an input\n", pin);
        }
        close(fd);

        if (edge != EDGE_NONE) {
          // Open pin edge control
          sprintf(buf, "/sys/class/gpio/gpio%d/edge", pin);
          NXPLOG_TML_D("Edge %s\n", buf);
          fd = open(buf, O_RDWR);
          if (fd < 0) {
            NXPLOG_TML_E("Could not open edge port '%s' (%s)", buf,
                         strerror(errno));
            return -1;
          } else {
            char* edge_str = "none";
            switch (edge) {
              case EDGE_RISING:
                edge_str = "rising";
                break;
              case EDGE_FALLING:
                edge_str = "falling";
                break;
              case EDGE_BOTH:
                edge_str = "both";
                break;
            }
            int l = strlen(edge_str);
            NXPLOG_TML_D("Edge-string %s - %d\n", edge_str, l);
            if (write(fd, edge_str, l) == l) {
              NXPLOG_TML_D("Pin %d trigger on %s\n", pin, edge_str);
            }
            close(fd);
          }
        }

        // Open pin
        sprintf(buf, "/sys/class/gpio/gpio%d/value", pin);
        NXPLOG_TML_D("Value %s\n", buf);
        fd = open(buf, O_RDONLY);
        if (fd < 0) {
          NXPLOG_TML_E("Could not open value port '%s' (%s)", buf,
                       strerror(errno));
          return -1;
        } else {
          return (fd);  // Success
        }
      }
    }
  }
  return (0);
}

void NfccAltTransport::gpio_set_ven(int value) {
#ifdef USE_LIBGPIOD
  if (m_GpioDInUse) {
      SetGpioDPin(*mEnableLineRequest, "Enable", value);
      usleep(10 * 1000);
      return;
  }
#endif

  if (iEnableFd >= 0) {
    if (value == 0) {
      write(iEnableFd, "0", 1);
    } else {
      write(iEnableFd, "1", 1);
    }
    usleep(10 * 1000);
  }
}

void NfccAltTransport::gpio_set_fwdl(int value) {
#ifdef USE_LIBGPIOD
  if (m_GpioDInUse) {
      SetGpioDPin(*mFWDownloadLineRequest, "FW DL", value);
      usleep(10 * 1000);
      return;
  }
#endif

  if (iFwDnldFd >= 0) {
    if (value == 0) {
      write(iFwDnldFd, "0", 1);
    } else {
      write(iFwDnldFd, "1", 1);
    }
    usleep(10 * 1000);
  }
}

void NfccAltTransport::wait4interrupt(void) {
#ifdef USE_LIBGPIOD
  if (m_GpioDInUse) {
    gpiod::edge_event_buffer event_buffer(1);

    while (mIRQLineRequest->get_value(mIRQLineRequest->offsets()[0]) != gpiod::line::value::ACTIVE) {
      // negative timeout -> sleep until an event is ready
      mIRQLineRequest->wait_edge_events(std::chrono::nanoseconds{-1});
      mIRQLineRequest->read_edge_events(event_buffer, 1);
    }

    return;
  }
#endif

  /* Open STREAMS device. */
  struct pollfd fds[1];
  fds[0].fd = iInterruptFd;
  fds[0].events = POLLPRI;
  int timeout_msecs = -1;  // 100000;
  int ret;
  // usleep(500000);
  while (!GetIrqState(NULL)) {
    // Wait for an edge on the GPIO pin to get woken up
    ret = poll(fds, 1, timeout_msecs);
    if (ret != 1) {
      NXPLOG_TML_D("wait4interrupt() %d - %s, ", ret, strerror(errno));
    }
  }
}

/*****************************************************************************
   **
   ** Function         ConfigurePin
   **
   ** Description      Configure Pins such as IRQ, VEN, Firmware Download
   **
   ** Parameters       none
   **
   ** Returns           NFCSTATUS_SUCCESS - on Success/ -1 on Failure
   ****************************************************************************/
int NfccAltTransport::ConfigurePin()
{
#ifdef USE_LIBGPIOD
  NXPLOG_TML_D("ConfigurePin: compiled w/ libgpiod support");
#else
  NXPLOG_TML_D("ConfigurePin: compiled w/o libgpiod support");
#endif
#ifdef USE_LIBGPIOD
  char enable_linename[64];
  char fwdl_linename[64];
  char irq_linename[64];

  // check whether all three config options are strings and only then try to
  // acquire all via libgpiod, otherwise just fallback and let the old code
  // handle errors
  if (GetNxpStrValue(NAME_EXT_PIN_INT, irq_linename, sizeof(irq_linename)) &&
      GetNxpStrValue(NAME_EXT_PIN_ENABLE, enable_linename, sizeof(enable_linename)) &&
      GetNxpStrValue(NAME_EXT_PIN_FWDNLD, fwdl_linename, sizeof(fwdl_linename))) {

    try {
      gpiod::line_settings line_settings;
      line_settings.set_direction(gpiod::line::direction::OUTPUT);
      line_settings.set_output_value(gpiod::line::value::INACTIVE);

      mEnableLineRequest =
        std::make_unique<gpiod::line_request>(GetGpioDByName(enable_linename, "Enable", line_settings));
    }
    catch (const std::runtime_error& e) {
      NXPLOG_TML_E("ConfigurePin(Enable): %s", e.what());
      return NFCSTATUS_INVALID_DEVICE;
    }

    try {
      gpiod::line_settings line_settings;
      line_settings.set_direction(gpiod::line::direction::OUTPUT);
      line_settings.set_output_value(gpiod::line::value::INACTIVE);

      mFWDownloadLineRequest =
        std::make_unique<gpiod::line_request>(GetGpioDByName(fwdl_linename, "FWDLReq", line_settings));
    }
    catch (const std::runtime_error& e) {
      NXPLOG_TML_E("ConfigurePin(FW DL Req): %s", e.what());
      mEnableLineRequest->release();
      return NFCSTATUS_INVALID_DEVICE;
    }

    try {
      gpiod::line_settings line_settings;
      line_settings.set_direction(gpiod::line::direction::INPUT);
      line_settings.set_edge_detection(gpiod::line::edge::RISING);

      mIRQLineRequest =
        std::make_unique<gpiod::line_request>(GetGpioDByName(irq_linename, "IRQ", line_settings));
    }
    catch (const std::runtime_error& e) {
      NXPLOG_TML_E("ConfigurePin(IRQ): %s", e.what());
      mFWDownloadLineRequest->release();
      mEnableLineRequest->release();
      return NFCSTATUS_INVALID_DEVICE;
    }

    m_GpioDInUse = true;
    return NFCSTATUS_SUCCESS;
  }
#endif

  int pin_int = loadIntValueOrDefault(NAME_EXT_PIN_INT, DEFAULT_PIN_INT);
  int pin_ena = loadIntValueOrDefault(NAME_EXT_PIN_ENABLE, DEFAULT_PIN_ENABLE);
  int pin_fwd = loadIntValueOrDefault(NAME_EXT_PIN_FWDNLD, DEFAULT_PIN_FWDNLD);

  // Assign IO pins
  iInterruptFd = verifyPin(pin_int, 0, EDGE_RISING);
  if (iInterruptFd < 0) return (NFCSTATUS_INVALID_DEVICE);
  iEnableFd = verifyPin(pin_ena, 1, EDGE_NONE);
  if (iEnableFd < 0) return (NFCSTATUS_INVALID_DEVICE);
  iFwDnldFd = verifyPin(pin_fwd, 1, EDGE_NONE);
  if (iFwDnldFd < 0) return (NFCSTATUS_INVALID_DEVICE);
  return NFCSTATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         Close
**
** Description      Closes NFCC device
**
** Parameters       pDevHandle - device handle
**
** Returns          None
**
*******************************************************************************/
void NfccAltTransport::Close(void* pDevHandle) {
  NXPLOG_TML_D("%s Enter", __func__);
  if (NULL != pDevHandle) {
    close((intptr_t)pDevHandle);
  }
  if (iEnableFd >= 0) {
      close(iEnableFd);
      iEnableFd = -1;
  }
  if (iInterruptFd >= 0) {
      close(iInterruptFd);
      iInterruptFd = -1;
  }
  if (iFwDnldFd >= 0) {
      close(iFwDnldFd);
      iFwDnldFd = -1;
  }
#ifdef USE_LIBGPIOD
  if (m_GpioDInUse) {
      mEnableLineRequest->release();
      mFWDownloadLineRequest->release();
      mIRQLineRequest->release();
  }
#endif
  NXPLOG_TML_D("%s exit", __func__);
}

#ifdef USE_LIBGPIOD

gpiod::line_request NfccAltTransport::GetGpioLineByName(const std::string& name,
                                                        const std::string& consumer,
                                                        const gpiod::line_settings& settings) {

  for (const auto& entry : std::filesystem::directory_iterator("/dev/")) {
    if (gpiod::is_gpiochip_device(entry.path())) {
      gpiod::chip chip(entry.path());

      auto offset = chip.get_line_offset_from_name(name);
      if (offset >= 0) {
        return chip
                .prepare_request()
                .set_consumer("libnfc-nci: " + consumer)
                .add_line_settings(offset, settings)
                .do_request();
      }
    }
  }

  throw std::runtime_error("No GPIO line with name '" + name + "' found.");
}

gpiod::line_request NfccAltTransport::GetGpioDByName(const std::string& name,
                                                     const std::string& consumer,
                                                     gpiod::line_settings& line_settings) {
  std::string line_name = name;
  bool active_low = false;

  if (!line_name.empty() && line_name.front() == '!') {
    active_low = true;
    line_name.erase(0, 1);
  }

  line_settings.set_active_low(active_low);

  return GetGpioLineByName(line_name, consumer, line_settings);
}

int NfccAltTransport::GetIrqStateLibGpioD() {
  return mIRQLineRequest->get_value(mIRQLineRequest->offsets()[0]) == gpiod::line::value::ACTIVE;
}

void NfccAltTransport::SetGpioDPin(gpiod::line_request& lq, const char* line_descr, int value) {
    try {
      lq.set_value(lq.offsets()[0], value ? gpiod::line::value::ACTIVE : gpiod::line::value::INACTIVE);
    }
    catch (const std::runtime_error& e) {
      NXPLOG_TML_E("SetGpioDPin(%s) to '%d' failed: %s\n", line_descr, value, e.what());
    }
}

#endif // USE_LIBGPIOD
