# linux_libnfc-nci - EVerest

Find the original README at the bottom.

branch **everest**: Modified branch **NCI2.0_PN7160**, working with PN7160 NFC controllers on 64bit Linux platforms, and optimized for used with [EVerest](https://github.com/EVerest).

## Modifications Notice

The [original upstream source code](https://github.com/NXPNFCLinux/linux_libnfc-nci), released under the Apache 2.0 license and re-distributed here under the same license, has been modified as indicated below:

- Applied 64bit patches
- Added CMakeLists.txt
- Allow for runtime configuration of interface properties for alternative I²C/SPI interface
- Allow configuration files to be in a custom path
- Added support for using GPIO line names via libgpiod

## Building

### As Dependency

When using this library in EVerest, retrieval of the source code will be handled automatically.

Using ```add_subdirectory``` or ```find_package(libnfc_nci)``` in a depending CMake project, will provide the property ```LIBNFCNCI_CONFIG_INSTALL_PATH```, which can be retrieved to be used by the depending project:
 
 ```get_target_property(LIBNFCNCI_CONFIG_INSTALL_PATH libnfc_nci::libnfc_nci LIBNFCNCI_CONFIG_INSTALL_PATH)```

This path is where the configuration files

- ```libnfc-nci.conf```
- ```libnfc-nxp.conf```

are expected to be.

If *libnfc_nci* is included via ```add_subdirectory```, the depending project is responsible to install the required versions of these files.

If *libnfc_nci* is included via ```find_package()```, the depending project may still need to install the configuration files in order to overwrite the defaults installed by the library.

### As Standalone Build

Standalone test builds can be done as follows:
```
git clone git@github.com:EVerest/linux_libnfc-nci.git
cd linux_libnfc-nci
git checkout everest
mkdir build
cd build
CMAKE_PREFIX_PATH=<path to everest-cmake> cmake -DCMAKE_INSTALL_PREFIX=<prefered install location> ..
# or, if standalone examples are required:
CMAKE_PREFIX_PATH=<path to everest-cmake> cmake -DCMAKE_INSTALL_PREFIX=<prefered install location> -DLIBNFCNCI_BUILD_EXAMPLES=ON ..
cmake --build . --parallel -j
cmake --install .
```
This will also install the configuration files into the expected folder.

### libgpiod

Building against `libgpiod` can be enabled by passing the property `LIBNFCNCI_LIBGPIOD` set to `ON` to cmake.
By default, this is disabled.
Using libgpiod and GPIO line names should be the preferred way to access GPIO lines from user-space since
the older Linux' sysfs interface is deprecated since ages.

## Runtime Configuration

Runtime configuration (debug output levels, detailled NFC behaviour, interface specification) happens via the above-mentioned files.

Templates, which are also the defaults to be installed in a standalone build, can be found in ```conf/```:

- ```conf/libnfc-nci.conf```
- ```conf/libnfc-nxp.conf```

The specification of the interface to the NFC chip is defined in ```libnfc-nxp.conf```.
Relevant settings are:

```
NXP_TRANSPORT=0x02
PIN_INT=535
PIN_ENABLE=536
PIN_FWDNLD=537
I2C_ADDRESS=0x28
I2C_BUS="/dev/i2c-1"
SPI_BUS="/dev/spidev0.0"
```

When compiled with libgpiod support, then instead of using integers for Linux' legacy sysfs interface,
it is possible to use GPIO line names. These are usually defined via Device Tree for your board.
Such line names must be enclosed in quotation marks to indicate that the configuration value is a string.
And it cannot be mixed: all pin configuration values needs to be GPIO line names.

```
NXP_TRANSPORT=0x02
PIN_INT="RFID_IRQ"
PIN_ENABLE="nRESET_RFID"
PIN_FWDNLD="RFID_DWL_REQ"
I2C_ADDRESS=0x28
I2C_BUS="/dev/i2c-1"
```


---

  Original README:

---

linux_libnfc-nci
================

branch NCI2.0_PN7160: Linux NFC stack for PN7160 NCI2.0 based NXP NFC Controller.
For previous NXP NFC Controllers support (PN7150, PN7120) refer to branch master.

Information about NXP NFC Controller can be found on [NXP website](https://www.nxp.com/products/identification-and-security/nfc/nfc-reader-ics:NFC-READER).

Further details about the stack [here](https://www.nxp.com/doc/AN13287).

Release version
---------------
- NCI2.0-R1.1 fix limitation about multiple T5T, fix issue with MIFARE Classic read after write, cleanup of alternate transport definition, cleanup of logs 
- NCI2.0-R1.0 is the first official release of Linux libnfc-nci stack for PN7160

Possible problems, known errors and restrictions of R1.1:
---------------------------------------------------------
- LLCP1.3 support requires OpenSSL Cryptography and SSL/TLS Toolkit (version 1.0.1j or later)
- To install on 64bit OS, apply patch
