---
layout: default
title: Programming SidecarTridge Multi-device
nav_order: 7
nav_exclude: false
parent: SidecarTridge Multi-device
redirect_from:
  - /programming
  - /progamming/
---

# Programming SidecarTridge Multi-device
{: .no_toc }

This section provides developers with in-depth guidance on programming the Multi-device board. It mainly covers the development of new applications or "microfirmware apps" for the RP2040 microcontroller. The guide is structured to help you understand the architecture of the Multi-device board, the microfirmware apps, and how to set up your development environment.

{: .warning}
This guide applies to the new version 2.0 of the firmware in beta stage.

{: .note}
Starting with template `v1.1.0` the framework adopts the architecture used in `md-drives-emulator`: 192 KB of general-purpose RAM, a single 64 KB ROM4 bank served entirely by chained DMA, and a polled ROM3 DMA-ring command channel dispatched through `chandler`. The "Microfirmware design" sections below describe this layout. Apps written against earlier templates must migrate (see `CHANGELOG.md`).



<details open markdown="block">
  <summary>
    Table of contents
  </summary>
  {: .text-delta }
1. TOC
{:toc}
</details>

## Setting up the development environment

### Requirements

{: .warning}
If you want to contribute to the projects as a developer, you'll need a Raspberry Pi Pico Debug Probe.

- The [atarist-toolkit-docker](https://github.com/sidecartridge/atarist-toolkit-docker) is pivotal. Familiarize yourself with its installation and usage.

- A `git` client, command line or GUI, your pick.

- A Makefile attuned with GNU Make.

- Visual Studio Code is highly recommended if you want to debug the code. You can find the configuration files in the `.vscode` folder. 

- An Atari ST/STE/MegaST/MegaSTE computer. You can also use an emulator such as Hatari or STEEM for testing purposes, but you cannot really test any real functionality of the app without a real computer.

### Setup a Raspberry Pi Pico development environment from scratch

If you are not familiar with the development on the Raspberry Pi Pico or RP2040, we recommend you to follow the [Getting Started with Raspberry Pi Pico](https://datasheets.raspberrypi.org/pico/getting-started-with-pico.pdf) guide. This guide will help you to setup the Raspberry Pi Pico development environment from scratch.

We also think it's very important to setup the picoprobe debugging hardware. The picoprobe is a hardware debugger that will allow you to debug the code running on the Raspberry Pi Pico. You can find more information in this two excellent tutorials from Shawn Hymel:
- [Raspberry Pi Pico and RP2040 - C/C++ Part 1: Blink and VS Code](https://www.digikey.es/en/maker/projects/raspberry-pi-pico-and-rp2040-cc-part-1-blink-and-vs-code/7102fb8bca95452e9df6150f39ae8422)
- [Raspberry Pi Pico and RP2040 - C/C++ Part 2 Debugging with VS Code](https://www.digikey.es/en/maker/projects/raspberry-pi-pico-and-rp2040-cc-part-2-debugging-with-vs-code/470abc7efb07432b82c95f6f67f184c0)

To support the debugging the SidecarTridge has four pins that are connected to the picoprobe hardware debugger. These pins are:
- UART TX: This pin is used to send the debug information from the RP2040 to the picoprobe hardware debugger.
- UART RX: This pin is used to send the debug information from the picoprobe hardware debugger to the RP2040.
- GND: Two ground pins. One MUST connect to the GND of the Raspberry Pi Pico W (the middle connector between DEBUG and SWCLK and SWDIO) and the other MUST connect to the GND of the picoprobe hardware debugger. Don't let this pins floating, otherwise the debugging will not work.

Also a good tutorial about setting up a debugging environment with the picoprobe can be found in the [Raspberry Pi Debug Probe](https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html) tutorial.

Trying to develop software for this microcontroller without the right environment is frustrating and a waste of time. So please, take your time to setup the development environment properly. It will save you a lot of time in the future.

#### Configure environment variables

The following environment variables are required to be set:

- `PICO_SDK_PATH`: The path to the Raspberry Pi Pico SDK.
- `PICO_EXTRAS_PATH`: The path to the Raspberry Pi Pico Extras SDK.
- `FATFS_SDK_PATH`: The path to the FatFS SDK.

This repository contains subrepos pointing to these SDKs in root folder.

All the compile, debug and build scripts use these environment variables to locate the SDKs. So it's very important to set them properly. An option would be to set them in your `.bashrc` file if you are using Bash, or in your `.zshrc` file if you are using ZSH. 

#### Configure Visual Studio Code

To configure Visual Studio Code to work with the Raspberry Pi Pico, please follow the [Raspberry Pi Pico and RP2040 - C/C++ Part 2 Debugging with VS Code](https://www.digikey.es/en/maker/projects/raspberry-pi-pico-and-rp2040-cc-part-2-debugging-with-vs-code/470abc7efb07432b82c95f6f67f184c0) tutorial.


**Linux/macOS**
```sh
export ARM_GDB_PATH=/path/to/your/arm-toolchain
export PICO_OPENOCD_PATH=/path/to/openocd/tcl
```

**Windows (PowerShell)**
```powershell
$env:ARM_GDB_PATH="C:\Path\to\your\arm-toolchain"
$env:PICO_OPENOCD_PATH="C:\Path\to\openocd\tcl"
```

**Windows (CMD)**
```cmd
set ARM_GDB_PATH=C:\Path\to\your\arm-toolchain
set PICO_OPENOCD_PATH=C:\Path\to\openocd\tcl
```

#### Install Visual Studio Code extensions

- [C/C++ Extension Pack](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools-extension-pack)
- [CMake](https://marketplace.visualstudio.com/items?itemName=twxs.cmake)
- [CMake Tools](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools)
- [Cortex-Debug](https://marketplace.visualstudio.com/items?itemName=marus25.cortex-debug)

## Architecture review

Before starting with the programming of the Multi-device board, it is important to understand the two basic ROM emulation modes available and describe in detail in the section [Architecture and Design](/sidecartridge-multidevice/architecture_and_design/#the-multi-device-board-from-the-atari-st-perspective). To summarize the two modes:

- Full ROM emulation mode: the Atari ST effortlessly reads the shared RAM memory as if perusing through a full address range of the ROM memory expansion, void of any perceptible differences. The Atari ST is NOT aware of the existence of the Multi-device.
- Hybrid ROM emulation mode: the Atari ST accesses the shared RAM memory but only a specific subrange of addresses of the ROM memory expansion. The other subranges are allocated to implement a command protocol between the host and the microcontroller, fostering an interactive session with applications running on the RP2040. The Atari ST is aware of the existence of the Multi-device.

## The Microfirmware apps

### What is a Microfirmware app?

The Microfirmware app is designed to run as a stand alone rp2040/rp235x plus the computer side firmware that communicates with it. The key components include:  

- **Microfirmware app Core**  
  The heart of the system, responsible for handling the micro SD card, communicating with the remote computer and providing the web interface if needed.  

- **Global Settings**  
  Stored in flash memory, these settings apply to the entire device and its apps. The microfirmware app does only read the settings, it does not modify them.

- **Local Settings**  
  Each app has its own isolated configuration stored in flash memory, ensuring app-specific settings remain independent. This section is read and written by the app.

- **Computer firmware**  
  The Booster app uses the remote computer as the terminal and implements the logic to communicate with the microfirmware app. Since it runs in ROM, it can be challenging to debug and develop it. Try to put all the debugging code in the microfirmware app and use the computer as a terminal. 

### General Memory Map  

The Pico W and Pico 2W feature a **2MB flash memory**, structured as follows:  

```plaintext
+-----------------------------------------------------+
|                                                     |
|              MICROFIRMWARE APP (1152K)              |  0x10000000
|          Active microfirmware flash space           |
|                                                     |
+-----------------------------------------------------+
...
+-----------------------------------------------------+
|                                                     |
|                  CONFIG FLASH (120K)                |  0x101E0000
|             Microfirmwares configurations           |
|                                                     |
+-----------------------------------------------------+
|                                                     |
|              GLOBAL LOOKUP TABLE (4K)               |  0x101FE000
|  Microfirmwares mapping to configuration sectors    |
|                                                     |
+-----------------------------------------------------+
|                                                     |
|              GLOBAL SETTINGS (4K)                   |  0x101FF000
|           Device-wide configurations                |
|                                                     |
+-----------------------------------------------------+
```

## How to Create a New Firmware Project Using `md-microfirmware-template`

This guide walks you through the steps to clone and configure a new firmware project for the SidecarTridge Multi-device platform, using `md-rtc-emulator` as an example. You can find the template repository in [https://github.com/sidecartridge/md-microfirmware-template](https://github.com/sidecartridge/md-microfirmware-template)

### 1. **Create a GitHub Repository from the Template**  

![Clone template](/sidecartridge-multidevice/assets/images/md-microfirmware-template-howto-1.png)

1. Navigate to the [`md-microfirmware-template`](https://github.com/sidecartridge/md-microfirmware-template) repository.
2. Click the green **“Use this template”** button and select **“Create a new repository”**.
3. Set the repository name to `md-rtc-emulator` (or any other descriptive name).
4. Ensure visibility is set to **Public** or **Private** as needed.
5. Click **“Create repository”**.

![Clone template 2](/sidecartridge-multidevice/assets/images/md-microfirmware-template-howto-2.png)

### 2. **Clone Your New Repository Locally**  

![Clone template 3](/sidecartridge-multidevice/assets/images/md-microfirmware-template-howto-3.png)

Pick the URL of the new repository you just created.

![Clone template 4](/sidecartridge-multidevice/assets/images/md-microfirmware-template-howto-4.png)

Clone your new repo using SSH or HTTPS:

```bash
git clone git@github.com:sidecartridge/md-rtc-emulator.git
cd md-rtc-emulator
```

![Clone template 5](/sidecartridge-multidevice/assets/images/md-microfirmware-template-howto-5.png)

### 3. **Initialize Git Submodules**  


Run the following commands to initialize all required submodules:

```bash
git submodule init
git submodule update --init --recursive
```

This pulls in essential dependencies like:
- `pico-sdk`
- `pico-extras`
- `fatfs-sdk`

![Clone template 5.1](/sidecartridge-multidevice/assets/images/md-microfirmware-template-howto-5.1.png)


### 4. **Build the Project**  

The main build script is `build.sh`, located in the root directory of the project executes both `buid.sh` scripts in the `/rp` and `/target` folders. The script in the `/rp` folder builds the microfirmware app for the RP2040, while the script in the `/target` folder builds the computer firmware for the target computer.

The parameters for the build script are:
1. The target device. In this version it can be `pico_w`.
2. The build type. It can be `debug` or `release`. `debug` will create a version with debug symbols and output traces through the UART. `release` will create a version without debug symbols and output traces.
3. The UUID of the app. This is a valid UUID v4 that will be used to identify the app. It must be unique for each app. The UUID is used to create the lookup table in the flash memory and to create the app info file. During development time, you can use the UUID `44444444-4444-4444-8444-444444444444` to test the app. 

```bash
./build.sh pico_w debug 44444444-4444-4444-8444-444444444444
```

![Clone template 5.2](/sidecartridge-multidevice/assets/images/md-microfirmware-template-howto-5.2.png)

and after a few seconds, you will see the build process output in the terminal.

![Clone template 5.3](/sidecartridge-multidevice/assets/images/md-microfirmware-template-howto-5.3.png)

In the `/dist` folder, you will find:
- the `44444444-4444-4444-8444-444444444444.uf2` binary file with the microfirmware app. 
- the `44444444-4444-4444-8444-444444444444.json` file with the app information. This file is used by the Booster app to identify the app and show it in the list of apps.
- the `44444444-4444-4444-8444-444444444444.md5` file with the md5 checksum of the binary file. This file is used by the Booster app to verify the integrity of the binary file.

![Clone template 5.4](/sidecartridge-multidevice/assets/images/md-microfirmware-template-howto-5.4.png)

The developer can now deploy the uf2 file to the Raspberry Pi Pico W using the `picotool` command line tool. For example:

```bash
picotool load dist/44444444-4444-4444-8444-444444444444.uf2
```

And rebooting the board connected to the computer will load the new firmware if the uf2 file is valid and in the Booster app the developer has selected the option to launch the `DEV APP`

But this is not a very quick way to develop a firmware. It's much easier to use the `picoprobe` hardware debugger and Visual Studio code to debug the code. 

### 5. **Open the Project in VS Code**  

![Clone template 6](/sidecartridge-multidevice/assets/images/md-microfirmware-template-howto-6.png)

Open the project folder in **Visual Studio Code** or **VS Code Insiders** using the entry in the menu `File` → `Add Folder to Workspace...` and select the folder you just cloned.

![Clone template 7](/sidecartridge-multidevice/assets/images/md-microfirmware-template-howto-7.png)

Use the **CMake Tools** extension to select a build environment:
- Open the **command palette** → “CMake: Select a Kit”
- Choose a compiler (e.g., `GCC 14.2.1 arm-none-eabi`)

Ensure that the `Auto Save` option is enabled for smooth development.

![Clone template 8](/sidecartridge-multidevice/assets/images/md-microfirmware-template-howto-8.png)

### 6. **Adjust the debugging options in Visual Studio code**  

The `.vscode` folder contains the configuration files for Visual Studio Code. **Please modify them as follows**:

- `launch.json`: Modify the `gdbPath` property to point to the `arm-none-eabi-gdb` file in your computer.
- `launch.json`: Modify the `searchDir` property to point to the `/tcl` folder inside the `openocd` project in your computer.
- `settings.json`: Modify the `cortex-debug.gdbPath` property to point to the `arm-none-eabi-gdb` file in your computer.

These variables allow the debugger to **automatically find GDB and OpenOCD** without modifying the `launch.json` file every time you switch environments.

## Microfirmware design

### Libraries and components  

#### External Libraries  

These libraries are in different repositories and are included as submodules:  

- Pico-SDK: The official Raspberry Pi Pico SDK, providing essential libraries and tools for development.
- Pico-Extras: A collection of additional libraries and utilities for the Pico platform, enhancing functionality and ease of use.
- [no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico): A library for interfacing with micro SD cards using the SPI protocol, enabling file system access and management.

#### Internal Libraries  

These libraries are part of the `rp` project and are included in the repository:  

- settings: The `rp-settings` library.

### Project structure

#### Two different architectures

The project is structure in two very different folders with code:

- `/rp`: where the code deployed in the RP2040 is located. This code is compiled and linked with the RP2040 SDK.
- `/target`: where the code for the target computer is located. The spirit of this folder is contain the code that will be compiled for the remote computer target. In this project the code is for `atarist`, so a folder `/target/atarist` is created. The code in this folder is compiled with the `atarist-toolkit-docker` project. This project is a docker image that contains all the tools needed to compile the code for the Atari ST/STE/MegaST/MegaSTE computers.

The binary file generated by the `build.sh` file in the `/target/atarist` folder must create a C array that should be included in the `/rp/src/include` folder with the filename `target_firmware.h`. The binary content of this array should be copied to the `ROM_IN_RAM` section of the RAM. 

The goal of this architecture is to have a clean separation between the code that runs in the RP2040 and the code that runs in the remote computer. Starting in version 2 of the firmware we will try to put as much code as possible in the `rp` folder. In 2025 is much easier to develop code for the RP2040 than for the Atari ST/STE/MegaST/MegaSTE computers. Hence, the code for the remote computers (Atari ST or any other future computers that we want to support) should be as minimal as possible and strictly focused on implementing the emulation of the targe device (ROM, Floppy, HD, RTC, Network, etc) and the communication with the RP2040.

#### The RP2040 and RP235x codebase

The code in the `/rp` folder is the code that will be compiled and linked with the RP2040 SDK. This code is responsible for handling the micro SD card, communicating with the remote computer, networking and providing the web interface if needed. 

Apart from the libraries described in the previous sections (`Pico-SDK`, `Pico-Extras`, `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` and `settings`), the code in the `/rp` folder is composed os several boilerplate modules that are used to implement the basic features of the microfirmware app. These modules are:

##### aconfig.c

This module is responsible for handling the configuration of the microfirmware app. It uses the `settings` library to store and retrieve the configuration settings. The configuration settings are stored in flash memory and are used to configure the microfirmware app.

Each app has its own configuration settings. To avoid misconfigurations during upgrades or downgrades, the list of default settings is stored in the `defaultEntries` array in this file. It is mandatory to define here the default settings for the app.

This app has two settings: the folder its Mac disk images and `PRAM.BIN` live in, and the app mode:

```c
static SettingsConfigEntry defaultEntries[] = {
    {ACONFIG_PARAM_FOLDER, SETTINGS_TYPE_STRING, "/jackintosh"},
    {ACONFIG_PARAM_MODE, SETTINGS_TYPE_INT, "255"},  // 255: Menu mode
};
```

Nothing on the ST edits them: the cartridge has no setup menu.

The local settings has to be initialized in the `main.c` file of the app once we have already verified that the app uuid key of the app exists and it is correct. The `CURRENT_APP_UUID_KEY` is the uuid of the app that is being executed.

The `aconfig_init` function will check if the app uuid key exists in the lookup table. If it does not exist, it will return an error and the app will be redirected to the booster. If it exists, it will check if the settings are already initialized. If they are not initialized, it will initialize them with the default values defined in the `defaultEntries` array.

```c
// If we are here, it means the app uuid key is correct. So we can read or
// initialize the app settings
err = aconfig_init(CURRENT_APP_UUID_KEY);
switch (err) {
  case ACONFIG_SUCCESS:
    DPRINTF("App settings found and already initialized\n");
    break;
  case ACONFIG_APPKEYLOOKUP_ERROR:
    // The app key is not found in the lookup table. Go to booster.
    DPRINTF("App key not found in the lookup table. Go to BOOSTER.\n");
    reset_jump_to_booster();
    // We should never reach this point
    break;
  case ACONFIG_INIT_ERROR:
    // No settings found. First time the app is executed? Then initialize the
    // settings
    DPRINTF("App settings not initialized. Initialize them first\n");
    err = settings_save(aconfig_getContext(), true);
    if (err < 0) {
      // Something went wrong saving the settings. Go to booster.
      DPRINTF("Error saving settings. Go to BOOSTER.\n");
      reset_jump_to_booster();
      // We should never reach this point
    }
    // Show the settings
    settings_print(aconfig_getContext(), NULL);
    break;
}
```

After the succesful initialization of the settings, the app can use the `aconfig_getContext()` function to get the context of the settings and use it to read and write the settings, as usual.

```c
  SettingsConfigEntry *appMode =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_MODE);
  if (appMode == NULL) {
    DPRINTF(
        "ACONFIG_PARAM_MODE not found in the configuration.\n");
  }
```

##### blink.c

This module is responsible for handling the blinking of the LED on the Raspberry Pi Pico W. It uses the `pico-sdk` library to control the GPIO pins and implement the blinking functionality. The LED is used to indicate the status of the microfirmware app.

##### emul.c

The critical path of the emulation is implemented in this module. After the initial checks in the `main.c` file, we implement in this module the initialization of the different services that are needed to run the microfirmware app, and then we enter in the main loop of the app.

The critical path is in the `emul_start` function called from the `main.c` file. This functions performs the following actions in this order:

1. Read the app mode from the settings.
2. Copy the cartridge image (`target_firmware.h`) into `ROM_IN_RAM`, then bring up the cartridge ROM4 read engine, the ROM3 command DMA ring and the command dispatcher, in this order: `init_romemul(false); commemul_init(); chandler_init(); chandler_addCB(...)`. Register one callback per subsystem that needs to react to commands: here `emul_bootScreenCb` (the boot screen's request for Booster) and `macdisk_commandCb` (the Mac's disks, time and PRAM), then `macdisk_init()` publishes the identity before the first command is answered.
3. Configure the SELECT button, before anything slow.
4. Init the sd card and, if it is there, mount the Mac's disk images (`macdisk_mountAll`). Without a card the app carries on and every unit stays empty.
5. Init the network, if needed. The polling callback, `emul_pollTick`, drains `chandler_loop()` and runs `select_poll()`, so commands sent during the multi-second WiFi connect window are not dropped.
6. Main loop. Each iteration runs the debug mailbox, `select_poll()`, `chandler_loop()` (drains the ROM3 ring → dispatches commands to registered callbacks) and `macdisk_syncPending()`; every 10 ms it polls the network and steps the NTP client (`ntp_pollSync()`).
7. When the boot screen asks for Booster, leave the main loop, write `CMD_RESET` into the cartridge sentinel (`WRITE_AND_SWAP_LONGWORD` at `CHANDLER_CMD_SENTINEL_OFFSET`), keep answering while the ST sees it, save the setup mode and jump to Booster.

To emulate a service or device of the remote computer in this module we will have to orchestrate the difference services to accomplish the task. As a rule of thumb, reuse as much boiler plate code as possible.

##### gconfig.c

This module is responsible for handling the global settings of the microfirmware app. It uses the `settings` library to store and retrieve the global settings. The global settings are stored in flash memory and are used to configure the microfirmware app.

In the global settings we have the parameters that are used for all the apps installed. To avoid misconfigurations during upgrades or downgrades, the list of global default settings is stored in the `defaultEntries` array and is a copy of the values found in the `booster` project. 

```c
static SettingsConfigEntry defaultEntries[] = {
    {PARAM_APPS_FOLDER, SETTINGS_TYPE_STRING, "/apps"},
    {PARAM_APPS_CATALOG_URL, SETTINGS_TYPE_STRING,
     "http://atarist.sidecartridge.com/apps.json"},
    {PARAM_BOOT_FEATURE, SETTINGS_TYPE_STRING, "CONFIGURATOR"},
    {PARAM_HOSTNAME, SETTINGS_TYPE_STRING, "sidecart"},
    {PARAM_SAFE_CONFIG_REBOOT, SETTINGS_TYPE_BOOL, "true"},
    {PARAM_SD_BAUD_RATE_KB, SETTINGS_TYPE_INT, "12500"},
    {PARAM_WIFI_AUTH, SETTINGS_TYPE_INT, "0"},
    {PARAM_WIFI_CONNECT_TIMEOUT, SETTINGS_TYPE_INT, "30"},
    {PARAM_WIFI_COUNTRY, SETTINGS_TYPE_STRING, "XX"},
    {PARAM_WIFI_DHCP, SETTINGS_TYPE_BOOL, "true"},
    {PARAM_WIFI_DNS, SETTINGS_TYPE_STRING, "8.8.8.8"},
    {PARAM_WIFI_GATEWAY, SETTINGS_TYPE_STRING, ""},
    {PARAM_WIFI_IP, SETTINGS_TYPE_STRING, ""},
    {PARAM_WIFI_MODE, SETTINGS_TYPE_INT, "0"},
    {PARAM_WIFI_NETMASK, SETTINGS_TYPE_STRING, ""},
    {PARAM_WIFI_PASSWORD, SETTINGS_TYPE_STRING, ""},
    {PARAM_WIFI_POWER, SETTINGS_TYPE_INT, "0"},
    {PARAM_WIFI_RSSI, SETTINGS_TYPE_BOOL, "true"},
    {PARAM_WIFI_SCAN_SECONDS, SETTINGS_TYPE_INT, "10"},
    {PARAM_WIFI_SSID, SETTINGS_TYPE_STRING, ""}};
```

The global settings has to be initialized in the `main.c` file right after changing the clock speed and voltage. It checks if the current app uuid key is the same as the one in the global settings. If it is not, it will be redirected to the booster. If it is, it will check if the settings are already initialized. If they are not initialized, it will initialize them with the default values defined in the `defaultEntries` array.

```c
  // Load the global configuration parameters
  int err = gconfig_init(CURRENT_APP_UUID_KEY);
  // If the global settings are not intialized, jump to the booster app to
  // initialize them
  if (err < 0) {
    DPRINTF("Settings not initialized. Jump to Booster application\n");
    reset_jump_to_booster();
  }
```

Then, access the global settings using the `gconfig_getContext()` function to get the context of the settings and use it to read and write the settings, as usual.

```c
  SettingsConfigEntry *appsFolder =
      settings_find_entry(gconfig_getContext(), PARAM_APPS_FOLDER);
  if (appsFolder == NULL) {
    DPRINTF("PARAM_APPS_FOLDER not found in the configuration.\n");
  }
```

Writing to the global settings is not allowed from the microfirmware apps. Use the Booster app to modify the global settings. 

##### macdisk.c

The Mac's floppy and hard disk images, served from the micro SD card as 512-byte block devices to the Jackintosh boot ROM (built in the sibling repository `md-jackintosh-roms`), which speaks the command protocol itself with no TOS. Units 0 and 1 are floppies and unit 2 the hard disk; at power-on `macdisk_mountAll()` mounts the first `FD0.*`, `FD1.*` and `HD0.*` in the app folder. The same app number, `$07`, also gives the Mac the time (from `ntp.c`) and keeps its 256 bytes of parameter RAM in `PRAM.BIN`.

`rp/src/include/macdisk.h` is the source of truth for the commands, their payloads, the result codes and the status block at `APP_FREE`; it is the contract with `md-jackintosh-roms`, so change it only after agreeing it there. Every command writes its result before it is answered and is harmless to repeat. `macdisk_syncPending()` syncs written images a few seconds after the first unsynced write; call it from the main loop.

##### network.c

This module is responsible for handling the network connection of the microfirmware app. It uses the `pico-sdk` library to control the network interface and implement the functionality. This app brings up Wi-Fi only for `ntp.c`: it downloads nothing.

This module implements several functions to handle the network connections. As a rule of thumb is strongly recommnded to try a network connection early in the critical path of the app, and give up and return an error if no connection is done.  

It's possible to start a WiFi connection in STA mode or AP mode. The STA mode is used to connect to a WiFi network, while the AP mode is used to create a WiFi network. An example code of how to start a WiFi connection in STA mode is:

```c
SettingsConfigEntry *wifiMode =
    settings_find_entry(gconfig_getContext(), PARAM_WIFI_MODE);
wifi_mode_t wifiModeValue = WIFI_MODE_STA;
if (wifiMode == NULL) {
  DPRINTF("No WiFi mode found in the settings. No initializing.\n");
} else {
  wifiModeValue = (wifi_mode_t)atoi(wifiMode->value);
  if (wifiModeValue != WIFI_MODE_AP) {
    DPRINTF("WiFi mode is STA\n");
    wifiModeValue = WIFI_MODE_STA;
    int err = network_wifiInit(wifiModeValue);
    if (err != 0) {
      DPRINTF("Error initializing the network: %i. No initializing.\n", err);
    } else {
      // Keep answering the ST while the connect blocks
      network_setPollingCallback(emul_pollTick);
      // Connect to the WiFi network
      int maxAttempts = 3;  // or any other number defined elsewhere
      int attempt = 0;
      err = NETWORK_WIFI_STA_CONN_ERR_TIMEOUT;

      while ((attempt < maxAttempts) &&
              (err == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT)) {
        err = network_wifiStaConnect();
        attempt++;

        if ((err > 0) && (err < NETWORK_WIFI_STA_CONN_ERR_TIMEOUT)) {
          DPRINTF("Error connecting to the WiFi network: %i\n", err);
        }
      }

      if (err == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT) {
        DPRINTF("Timeout connecting to the WiFi network after %d attempts\n",
                maxAttempts);
        // Optionally, return an error code here.
      }
      network_setPollingCallback(NULL);
    }
  } else {
    DPRINTF("WiFi mode is AP. No initializing.\n");
  }
}
```

In this example, we check if the WiFi mode is set to STA. If it is, we initialize the network and connect to the WiFi network. The code blocks until the connection is established or the maximum number of attempts is reached. If the connection is not established, we return an error code. The `network_setPollingCallback` allows to set a callback function that will be called during the polling period. This is useful to keep serving the ST while the connect blocks.

{: .note}
The polling callback must drain the ROM3 command ring so that commands sent during the multi-second WiFi connect window are not dropped. `emul.c` ships an `emul_pollTick` helper that calls `chandler_loop(); select_poll();` and installs it via `network_setPollingCallback(emul_pollTick);` while the connect is in flight.

To keep the code simple, give up the app if the network connection is not established. If the connection is successful, continue with the critical path of the app.

##### ntp.c

The time of day from an NTP server (`pool.ntp.org`), without blocking: a state machine that `ntp_pollSync()` steps from the main loop, next to the network poll. Nothing happens until the Wi-Fi has an address; then it asks, retrying every few seconds until one answer arrives, and from then on counts from the RP's timer. `ntp_getUnixTime()` gives UTC seconds since 1970, or false while no server has answered. The request and the checks on the answer come from `md-drives-emulator`'s `rtc.c`, whose query waits for the answer instead.

##### reset.c

This module is responsible for handling the reset of the microfirmware app. Resetting (and also erasing the app) can be sometimes tricky, so try this module first before implementing your own function.

##### romemul.c / romemul.pio (ROM4 read engine)

This is the core of all the microfirmware apps. It implements the emulation of the cartridge ROM4 region as a pure read engine.

Two PIO programs and a chained pair of DMA channels turn an Atari ST ROM4 access into a 16-bit reply with no CPU involvement at all:

1. The PIO state machine waits on `ROM4_GPIO`, latches the 16-bit address from the bus into the RX FIFO.
2. The first DMA channel reads that address out of the FIFO and writes it into the second channel's `al3_read_addr_trig` register.
3. The second DMA channel does a 16-bit read at that address (which falls inside the 64 KB `ROM_IN_RAM` region) and pushes the result into the PIO TX FIFO.
4. The state machine drives the data onto the bus.
5. Both channels are chained back-to-back, so the loop runs forever without a CPU IRQ.

Because the read path is entirely DMA-driven, the only thing the CPU has to do at startup is `init_romemul(bool copyFlashToRAM)` — pass `true` if you want the runtime to copy the ROM image from XIP flash into `ROM_IN_RAM` before enabling the engine, `false` if you populate the region yourself (e.g. from the SD card or by writing the image programmatically).

If you change any of these files you can find very strange bugs in the app...

##### commemul.c / commemul.pio (ROM3 command DMA ring)

The cartridge ROM3 region is no longer a data bank. It is reserved for the command channel: the m68k side encodes commands by reading from addresses inside ROM3 (`$FB0000`+), and the RP2040 captures every ROM3 access into a ring buffer.

`commemul_init()` brings up:
- A dedicated PIO state machine on `ROM3_GPIO` that waits on the ROM3 chip-select and pushes the 16-bit address onto the RX FIFO.
- A single DMA channel running in **ring mode** (`channel_config_set_ring`) that drains the FIFO into a 16 KB / 8,192-sample ring buffer. The size holds about two of the largest sends the ST can make, retries included; the arithmetic is next to `COMM_RING_BITS` in `commemul.c`.

There are no IRQs anywhere in this path. The application drains the ring by calling `commemul_poll(callback)`, which derives the producer index from `dma_hw->ch[ch].transfer_count` and invokes the callback for every new sample. If the application stops draining for long enough that a whole ring arrives, the lap is counted (`commemul_getOverruns()`) and the unread samples are dropped rather than parsed as a mix of old and new ones; the parser then resynchronises on the next header. The channel is re-armed from its live write address long before its transfer count runs out, so capture never stops.

`chandler.c` keeps counters of the handshake as plain globals: commands answered, dropped while one was pending, repeated with the previous token (the ST retrying), checksum errors, and the time spent busy, waiting for the ST, and draining. `tools/dev/swd.py counters` reads them from a running release or debug build.

##### chandler.c (command dispatcher)

`chandler` is the polled command parser/dispatcher that sits on top of `commemul` and `tprotocol`:

```c
chandler_init();
chandler_addCB(my_command_handler);   // can register multiple
...
while (running) {
  chandler_loop();   // drains commemul ring → tprotocol_parse → callbacks
  ...
}
```

`chandler_loop()` calls `commemul_poll`, feeds each captured 16-bit sample into `tprotocol_parse`, and once a complete command has been validated (header + checksum) it walks the registered callback list. Each callback has the signature `void cb(TransmissionProtocol *protocol, uint16_t *payloadPtr)` — `payloadPtr` already points past the random-token prefix, so callbacks can read parameters directly with the `TPROTO_GET_*` macros.

After dispatching, `chandler` writes the command's random-token reply into shared memory, so the m68k side can detect that the command was acknowledged.

##### sdcard.c

This module is responsible for handling the micro SD card at low level (signal level) and also file system level. It uses the `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` library to control the micro SD card and implement the functionality.

If your microfirmware app needs to access the micro SD card, you can use this module to read and write files. If you need access to the micro SD card at file system level, it's a good practice to create your own folder with the name of your app and implement the file system access in this folder. This way you can keep the code clean and easy to maintain. 

Let's look at an example of how to use the `sdcard.c` module to read a file from the micro SD card:

```c
FATFS fsys;
SettingsConfigEntry *folder =
    settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
char *folderName = "/jackintosh";  // MODIFY THIS TO YOUR FOLDER NAME
if (folder == NULL) {
  DPRINTF("FOLDER not found in the configuration. Using default value\n");
} else {
  DPRINTF("FOLDER: %s\n", folder->value);
  folderName = folder->value;
}
int sdcardErr = sdcard_initFilesystem(&fsys, folderName);
if (sdcardErr != SDCARD_INIT_OK) {
  DPRINTF("SD card unavailable (error %i). Continuing without SD.\n",
          sdcardErr);
} else {
  DPRINTF("SD card found & initialized\n");
  macdisk_mountAll(folderName);
}
```

As described in the section about networking, do not try to access the micro SD card if it is not initialized. 

##### select.c

This module is responsible for handling the SELECT button on the device. 

The SELECT can have two different functions:

- Short SELECT: Push and release. The RP restarts into the app; the ST keeps running whatever it was running.
- Long SELECT: Push and hold for ten (10) seconds (`SELECT_LONG_RESET`). This is a factory reset: `reset_deviceAndEraseFlash()` erases the global settings and the device restarts into the Booster app, which then clears the settings of every app.

`select_poll()` runs the button's state machine and never blocks. It debounces over 30 ms and calls the long-press callback as soon as the button has been held for `SELECT_LONG_RESET`; otherwise it calls the short-press callback on release. Call it from the main loop and from every long wait, next to `chandler_loop()`. A GPIO edge interrupt set up by `select_configure()` records a press that started and ended while nothing could poll, and the next call handles it.

```c
select_configure();
select_setResetCallback(reset_device);
select_setLongResetCallback(reset_deviceAndEraseFlash);

while (keepActive) {
  select_poll();    // never blocks
  chandler_loop();  // commands from the ST
  // ... the rest of the app
}
```

Keep SELECT on core 0. A watcher on core 1 ran the long press's flash erase while core 0 executed from the same flash, and both cores faulted until a power cycle.

#### Memory mapping

The memory mapping of the Multi-device board is defined in the file `rp/src/memmap_rp.ld` and it performs significant changes to the standard memory mapping of a RP2040 application. The memory mapping of the Multi-device board is:
- FLASH: Reduced from the 2MBytes found in the Raspberry Pi Pico W boards to 1024Kbytes for the active microfirmware app.
- RAM: Reduced from the 264KBytes found in the Raspberry Pi Pico W boards to **192Kbytes** (origin `0x20000000`, length `192K`).
- SCRATCH_X: No changes. Core 1's stack; the template never starts core 1.
- SCRATCH_Y: Core 0's stack, all 4Kbytes (`PICO_STACK_SIZE`), with an MPU guard on its bottom 32 bytes: an overflow is a HardFault instead of silent damage.
- CONFIG_FLASH: FLASH memory reserved for the configuration parameters of the Multi-device board. 4Kbytes.
- ROM_IN_RAM: RAM memory reserved for the cartridge ROM4 image. **64Kbytes** at `0x20030000`. This is exactly one cartridge ROM bank; the second 64 KB bank is no longer mirrored to RAM since ROM3 is now used as a command channel rather than a data bank.

The split between `RAM` (192K) and `ROM_IN_RAM` (64K) is fixed: the ROM4 read engine derives the address it serves from `__rom_in_ram_start__` (a symbol defined by the linker script), and changing the location or size of `ROM_IN_RAM` would require updating both the linker script and the C-side address shift in `init_romemul`.

The heap ends where `RAM` ends (`__StackLimit`), so it can never hand out memory inside the cartridge window, and malloc returns NULL when it runs out (`PICO_MALLOC_PANIC=0`): check every allocation. The link guarantees 32Kbytes of heap (`PICO_HEAP_SIZE`); if your static buffers leave less, it fails with ``region `RAM' overflowed``. Keep big buffers off the stack: core 0 has 4Kbytes.

##### Shared 64 KB region layout

The 64 KB `ROM_IN_RAM` region is mirrored 1:1 into the Atari ST address space at `$FA0000`. Both sides agree on a single layout, defined symbolically in `rp/src/include/chandler.h` (RP) and `target/atarist/src/main.s` (m68k). Apps must never hard-code an offset into this region; always derive it from the constants below so the layout stays the single source of truth.

| m68k addr | RP addr      | Size       | Region                                    |
| --------- | ------------ | ---------- | ----------------------------------------- |
| `$FA0000` | `0x20030000` | 8 KB max   | **CARTRIDGE** — m68k header + code        |
| `$FA2000` | `0x20032000` | 4 B        | **CMD_MAGIC_SENTINEL** — the boot screen polls here for `CMD_RESET` once it has asked for Booster |
| `$FA2004` | `0x20032004` | 4 B        | **RANDOM_TOKEN** — chandler echoes the request token here so `send_sync` returns |
| `$FA2008` | `0x20032008` | 4 B        | **RANDOM_TOKEN_SEED**                     |
| `$FA200C` | `0x2003200C` | 4 B        | reserved for framework use (zeroed by `chandler_init`) |
| `$FA2010` | `0x20032010` | 240 B      | **SHARED_VARIABLES** — 60 indexed 4-byte slots |
| `$FA2100` | `0x20032100` | 512 B      | **TRANSTABLE** — unused by this app       |
| `$FA2300` | `0x20032300` | ~48 KB     | **APP_FREE** — free arena for app-specific buffers; this app's status block and sector buffer (`macdisk.h`) |
| `$FAE0C0` | `0x2003E0C0` | 8000 B     | **FRAMEBUFFER** (320×200 monochrome) — unused by this app |
| `$FAFFFF` | `0x2003FFFF` | —          | end of region                             |

`TRANSTABLE` and `FRAMEBUFFER` belong to the template's setup menu, which this app does not have: nothing writes them. They stay in the layout because `APP_FREE` must stay at `$FA2300`, where the Jackintosh ROM reads the status block that `macdisk.h` defines.

Cartridge code budget:
- The cartridge image (header + m68k code) MUST fit in the first 8 KB of the region (`$FA0000`–`$FA1FFF`). Anything larger overlaps the shared block and breaks the protocol.
- `target/atarist/build.sh` enforces this against `BOOT.BIN` after `vlink` and aborts with a clear error if the image exceeds 8192 bytes. Direct `vasm`/`vlink` invocations bypass the check, so prefer the build script.

Constants — each row is the same value seen from both sides. RP-side
names live in `rp/src/include/chandler.h`; m68k-side names live in
`target/atarist/src/main.s`. Always reference the named symbol; never
hard-code the literal address.

| Purpose                                     | RP (chandler.h)                       | m68k (main.s)                | Value      |
| ------------------------------------------- | ------------------------------------- | ---------------------------- | ---------- |
| Cartridge code size budget                  | `CHANDLER_CARTRIDGE_CODE_SIZE`        | `CARTRIDGE_CODE_SIZE`        | 8 KB       |
| Base of the shared block                    | (= `CHANDLER_CMD_SENTINEL_OFFSET`)    | `SHARED_BLOCK_ADDR`          | `$FA2000`  |
| Command sentinel slot                       | `CHANDLER_CMD_SENTINEL_OFFSET`        | `CMD_MAGIC_SENTINEL_ADDR`    | `$FA2000`  |
| Random-token reply slot                     | `CHANDLER_RANDOM_TOKEN_OFFSET`        | `RANDOM_TOKEN_ADDR`          | `$FA2004`  |
| Random-token seed slot                      | `CHANDLER_RANDOM_TOKEN_SEED_OFFSET`   | `RANDOM_TOKEN_SEED_ADDR`     | `$FA2008`  |
| Reserved (zeroed by `chandler_init`)        | `CHANDLER_RESERVED_OFFSET`            | `RESERVED_SLOT_ADDR`         | `$FA200C`  |
| Shared variables base (60 × 4 B slots)      | `CHANDLER_SHARED_VARIABLES_OFFSET`    | `SHARED_VARIABLES`           | `$FA2010`  |
| Shared-variables slot count                 | `CHANDLER_SHARED_VARIABLES_SLOTS`     | (implicit: 60)               | 60         |
| App buffers base (TRANSTABLE + APP_FREE)    | `CHANDLER_APP_BUFFERS_OFFSET`         | `APP_BUFFERS_ADDR`           | `$FA2100`  |
| High-res mask table base                    | `CHANDLER_HIGHRES_TRANSTABLE_OFFSET`  | `TRANSTABLE`                 | `$FA2100`  |
| High-res mask table size                    | `CHANDLER_HIGHRES_TRANSTABLE_SIZE`    | (implicit: 512)              | 512 B      |
| Free arena for app-specific buffers         | `CHANDLER_APP_FREE_OFFSET`            | `APP_FREE_ADDR`              | `$FA2300`  |
| Framebuffer base                            | `CHANDLER_FRAMEBUFFER_OFFSET`         | `FRAMEBUFFER_ADDR`           | `$FAE0C0`  |
| Framebuffer size                            | `CHANDLER_FRAMEBUFFER_SIZE`           | `FRAMEBUFFER_SIZE`           | 8000 B     |

#### The remote computer codebase

Although the SidecarTridge Multidevice was developed with Atari ST in mind, it could be a chance to be ported to other target architectures. So the structure of the projects in the `target` folder is:

- **Architecture name**: Name of the computer architecture that implements the sender commands and emulation. In the current version, `atarist`. 

Inside the `atarist` folder we will find:

- `src`: source code in assemnbler of the scaffolding app.
- `build.sh`: script that compiles, build the app and also creates an array in C to embed it into the microcontroller project.
- `Makefile`: good old makefile of the project.
- `firmware.py`: Python script that generates the C array of the binary.
- `version.txt`: Version of the app.

If we open the `src` folder we will found the following folders and files:
- `inc`: function and macros to ease the development.
- `main.s`: Assembly code that implements a cartridge app that runs in ROM4 at boot time.
- `userfw.ld`: vlink linker script that places `main.s` at offset 0 of the cartridge image, with the whole 8 KB budget.

The app in `main.s` is deliberately simple: under TOS it only offers to leave. The Jackintosh ROM never runs it (there is no TOS, so nothing reads the cartridge header). It does as follows:

1. Defines the cartridge header format for an executable app after GEMDOS (`CA_INIT` bit 27), and turns a Mega STE's cache off.
2. Copies itself to the 4 KB below the screen memory and runs from there, so it survives an RP restart.
3. Says hello (`CMD_ST_HELLO`) until the RP answers, then publishes the machine and TOS (`detect_hw`, `get_tos_version`).
4. Shows the boot screen with GEMDOS `Cconws` and VT52 escapes: a 40-column page (centred on 80-column screens, from XBIOS `Getrez`) with the SidecarTridge Multi-device title bar, `JACKINTOSH` and the version (from `version.txt`, through `build/version.inc`, which the Makefile writes because vasm's `-D` takes no strings), what the cartridge is for, `[X] Back to Booster`, `[G] Continue to the GEM desktop`, the project's site and the copyright, with the cursor hidden. Each line is a row, a column and a text in the `screen_lines` table, printed by `print_lines`. A countdown from `BOOT_SCREEN_SECONDS` (30), read from the 200 Hz timer `_hz_200`, runs on the status line. Keys come from `Cconis` / `Cnecin`; any key other than X or G stops the countdown, and then only X or G leave.
5. On X, or when the countdown ends, sends `CMD_BOOSTER` (`$0002`, `EMUL_CMD_BOOSTER` in `rp/src/include/emul.h`) until it is answered, then waits up to 3 s for the RP to write `CMD_RESET` into the sentinel and resets the ST; the RP meanwhile jumps to Booster.
6. On G, puts the Mega STE's cache setting back and returns to TOS, which boots the GEM desktop. The next ST reset shows the boot screen again.

It's very important to note that this app does not use RAM memory beyond the 4 KB it copies itself to (and the system variables), so all developers must take these three ways:
1. Develop the app without using the RAM memory. Challenging, but possible sending the data to store as a command and letting the microcontroller shared memory space to handle the trick. 
2. Develop the app using some not occupied RAM memory setting manually the BSS and Heap.
3. Develop your app, place it in the ROM address range and finally copy it to RAM and initialize the memory settings before executing. This is how games and apps in ROM use to work.

#### Adding m68k modules

`userfw.ld` places each module's `.text` section at a fixed offset within the cartridge image; today only `main.s`, at offset 0. To add one (mirroring md-drives-emulator's `gemdrive.ld` pattern):

1. Pick an offset within the cartridge budget and add a new `.text_<name> 0x????? : { <name>.o(.text) }` section to `userfw.ld`.
2. Mirror the offset on the m68k side with an `equ (ROM4_ADDR + $????)` symbol in `inc/sidecart_layout.s`.
3. Add the `.o` target to `target/atarist/Makefile` and link it in.
4. Reach it from `main.s` with a `jsr` or `jmp` to that symbol.

What a module that talks to the RP includes (at its top, the senders at its end):

| File | What it gives |
| --- | --- |
| `inc/sidecart_layout.s` | The cartridge window (`RANDOM_TOKEN_ADDR`, `SHARED_VARIABLES`, `APP_FREE_ADDR`, …) and the command channel. Shared with `main.s`; the single source of truth on the m68k side. |
| `inc/sidecart_macros.s` | `send_sync` / `send_write_sync`, what a send keeps and destroys, and the Mega STE cache macros. |
| `inc/tos.s` | GEMDOS, BIOS and XBIOS function numbers. They are decimal in `cmp.w #n`; the documents often give them in hex. |
| `inc/sidecart_functions.s` | The senders themselves, `detect_hw` and `get_tos_version`. Included at the end of the module, followed by the NOP tail. |

Two rules for the code itself:
- **Everything is PC-relative.** A module runs from the cartridge but is linked at its offset, not at `$FA0000` plus the offset: use `lea label(pc), a0`, never an absolute label.
- **The module ends with the NOP tail**, after `include "inc/sidecart_functions.s"`: `even`, eight `nop`s, a `<module>_end:` label. `firmware.py` strips trailing zeros from the image and the RP copies only that many words into the window, while the write sender and the 68000's prefetch read past the end of the wait loop.

Keep the total cartridge image within `CARTRIDGE_CODE_SIZE = 8 KB`; `target/atarist/build.sh` enforces this against `BOOT.BIN` after `vlink`.

#### The Transmision Protocol (TPROTOCOL)

The `tprotocol.h` module is the core of the communication between the microfirmware app and the remote computer. It implements the communication protocol and the logic to handle the commands sent from the remote computer.

##### Anatomy of a command

The cartridge port of the Atari ST computers is a read-only port. If it is not possible to write to the cartridge port then we have to find a way to inform the microcontroller of the commands that are being sent from the remote computer.

The solution is to read from reserved memory addresses of the cartridge port. Example: If we want to send the value $1234 to the microcontroller, we can write the value $1234 to the address range of ROM3: $FB1234. The microcontroller will read the value from the address $FB1234, will keep the lower 16 bits and will ignore the upper 16 bits. Hence, the value $1234 can be interpreted as a command.

But obviously this is not enough. We cannot control that any app read from a valid memory address in the computer and that would trigger a false positive command. So how can we control that the command is valid and that it is not a false positive? Implementing a protocol with multiple data integrity checks.

So, if we want to send the value $1234 to the microcontroller, we should have to send something like this:

```plaintext
+---------+---------------------+------------------------------+
|  INDEX  |         VALUE       |          DESCRIPTION         |
+---------+---------------------+------------------------------+
|  0x00   |        0xABCD       |            HEADER            |
|  0x02   |        0x1234       |            VALUE             |
+---------+---------------------+------------------------------+
```

Adding a header with the magic number 0xABCD to the command will help to identify the command as a valid command. But this is a very simple approach and it is not enough. It does not have enough entropy and obviously we want to have more complex commands. So let's make a list of what we would need to implement a good command transmision protocol over a noisy channel like a cartridge port:
- **Header**: A magic number to identify the command as a valid command.
- **Command**: The command number to be executed.
- **Payload size**: The size of the payload in bytes.
- **Payload**: The data to be sent to the microcontroller.
- **Checksum**: A checksum to verify the integrity of the command.

So now we have our command structure:

```plaintext
+---------+---------------------+------------------------------+
|  INDEX  |         VALUE       |          DESCRIPTION         |
+---------+---------------------+------------------------------+
|  0x00   |        0xABCD       |            HEADER            |
|  0x02   |        0xZZZZ       |            COMMAND           |
|  0x04   |        0xNNNN       |         PAYLOAD SIZE         |
|  0x06   |        0xaaaa       |            PAYLOAD           |
...
|0xNNNN-2 |        0xzzzz       |            PAYLOAD           |
|0xNNNN+6 |        0xCHKS       |            CHECKSUM          |
+---------+---------------------+------------------------------+
```

This is the command protocol structure that efficiently `tprotocol.h` parses from the ROM readings from ROM3. 

##### Command catalog

Now that we have an structure for the command, we need to define the commands that we want to implement. And we have to implement them in two different places:

**Listener**

The listener is the code that will be executed when the command is received. This code will be executed in the context of the microcontroller, so it has to be fast and efficient. And it will be developed in C.

**Sender**

The sender is the code that will be executed when the command is sent from the remote computer. It has to be fast and eficcient also, and should be implemented in C or Assembler. In this firt version, for Atari ST series.


A command id is the app number in the high byte and the command in the low byte. This app has two sets:

- **`$0002`** `EMUL_CMD_BOOSTER`: sent by the boot screen, no payload. The RP resets the ST and starts Booster.
- **`$0700`-`$0705`**, app `$07`: the Mac's disks, time and PRAM, sent by the Jackintosh ROM. `rp/src/include/macdisk.h` defines them, with their payloads and results.

On the RP side:

```c
// rp/src/include/emul.h
#define EMUL_CMD_BOOSTER 0x0002

// rp/src/include/macdisk.h
#define APP_MACDISK 0x07
#define MACDISK_CMD_READ ((APP_MACDISK << 8) | 0x00)
```

In the Atari ST side, `main.s` mirrors the one it sends:

```asm
CMD_BOOSTER			equ $0002
```

The payload size of the command will be calculated during the handling command process.

##### Command handling

Starting with template `v1.1.0` the command path is **polled, not interrupt-driven**. ROM3 cartridge accesses are captured by a dedicated PIO + DMA ring (see `commemul.c`); the application reads commands out of that ring by calling `chandler_loop()` from its main loop. `chandler` parses the protocol via `tprotocol_parse` and dispatches each validated command to a list of registered callbacks.

To handle a command in your app you do two things:

1. **Register a callback** during initialization, after `chandler_init()`:

```c
init_romemul(false);     // ROM4 chained-DMA read engine
commemul_init();         // ROM3 DMA ring on GPIO 26
chandler_init();         // command parser/dispatcher
chandler_addCB(my_command_handler);
```

You can register multiple callbacks; `chandler` walks the list in registration order for every parsed command. `emul.c` registers `emul_bootScreenCb` and `macdisk_commandCb` this way.

2. **Implement the callback**. The signature is `void cb(TransmissionProtocol *protocol, uint16_t *payloadPtr)`. The `payloadPtr` argument has already been advanced past the 32-bit random token, so your handler can read parameters directly with the `TPROTO_GET_*` macros:

```c
void macdisk_commandCb(TransmissionProtocol *protocol, uint16_t *payloadPtr) {
  if ((protocol->command_id >> 8) != APP_MACDISK) return;  // another app's

  // payloadPtr already points past the random token: d3, then d4.
  uint32_t d3 = TPROTO_GET_PAYLOAD_PARAM32(payloadPtr);
  uint16_t unit = (uint16_t)(d3 & 0xFFFFu);
  uint16_t count = (uint16_t)(d3 >> 16);
  uint32_t lba = TPROTO_GET_NEXT32_PAYLOAD_PARAM32(payloadPtr);

  int32_t result;
  switch (protocol->command_id) {
    case MACDISK_CMD_READ:
      result = macdisk_readSectors(unit, count, lba);
      break;
    // ...
    default:
      result = MACDISK_ERR_PARAM;
      break;
  }
  WRITE_AND_SWAP_LONGWORD(macdisk_windowBase(), MACDISK_RESULT_OFFSET,
                          (uint32_t)result);
}
```

`chandler` writes the answer only after every callback has returned, so a callback writes its results into the window before it returns: the ST reads them as soon as it sees the answer.

3. **Drain the ring from your main loop**. Every iteration calls `chandler_loop()`. Inside, it polls the ROM3 ring, parses any new samples, dispatches completed commands to your callback, then writes the random-token reply into shared memory so the m68k side knows the command was acknowledged:

```c
while (keepActive) {
  chandler_loop();         // drains the ROM3 ring; calls registered callbacks
  macdisk_syncPending();   // the app's own work, never waiting
  ...
}
```

If your app does long-blocking work (e.g. `cyw43_arch_wait_for_work_until` during WiFi connect), make sure `chandler_loop` is also drained from the polling callback you install with `network_setPollingCallback`, otherwise commands will pile up in the ring during the blocking window. `emul.c` provides `emul_pollTick`, which calls `chandler_loop()` and `select_poll()`.

To avoid memory issues and manage little and big endian properly, use the macros defined in `tprotocol.h` to read and write the payload. The macros start with the prefix `TPROTO`.

> **Note on `TransmissionProtocol.payload`.** As of `v1.1.0` the payload buffer is typed as `uint16_t[]` (was `unsigned char[]`). The `TPROTO_GET_*` / `TPROTO_NEXT*` macros work the same way; only the underlying storage type changed. If you have older code that did `((uint16_t *)protocol->payload)[i]`, the cast is now a no-op but still compiles.

##### Returning the command results

Returning values to the Atari ST is a much more easy task. In order to do so, write the values to the shared memory address defined for the random token. A good place can be the ROM3 addressing. There is nothing avoiding us from using this ROM3 address space, so we can use it to return the values to the Atari ST. To read and write information to the shared memory and avoid the mess of big and little endian, use the macros defined in the `memfunc.h` file.

###### Sending commands from the remote computer

The ST sends commands with the functions in `target/atarist/src/inc/sidecart_functions.s`, through the macros in `inc/sidecart_macros.s`:

```asm
send_sync       <command>, <payload bytes>   ; payload in d3-d6 (0 to 16 bytes)
send_write_sync <command>, <buffer bytes>    ; d3, d4, d5 always sent, then the buffer at a4
```

Each macro sends the command, waits for the RP's answer and resends up to `CMD_RETRIES_COUNT` times. It returns with `d0 = 0` and Z set on success, `d0` non-zero and Z clear when every attempt timed out. The functions underneath (`send_sync_command_to_sidecart`, `send_sync_write_command_to_sidecart`) keep the same contract, so code may branch on the flags straight after either.

What a send keeps, measured on an ST:

| Macro | Keeps | Destroys |
| --- | --- | --- |
| `send_sync` | d1-d6 | d0, d7 (its retry count), a0-a1 (a0-a3 when the wait loop is copied, `COMMAND_SYNC_USE_DSKBUF` not 0) |
| `send_write_sync` | d1-d5, a4 | d0, d6 (its retry count), d7, a0-a1 (a0-a3 likewise) |

The senders leave a0 and a1 pointing into the ROM3 command window: a payload read through them is itself sampled by the RP, and the command fails its checksum on every retry. Take what you need into a kept register before the send, and reload address registers after it.

How the exchange works, and what each side may assume:

- **The command is read, not written.** The cartridge port is read-only, so the ST emits each 16-bit word by reading `$FB8000 + word`: the header `$ABCD`, the command, the payload size, the random token, the payload and a checksum.
- **The answer is a two-phase commit.** The token is the seed the ST read at `RANDOM_TOKEN_SEED_ADDR` just before sending. The RP writes the token back to `RANDOM_TOKEN_ADDR`, then a new seed; the ST accepts the answer only when the token matches *and* the seed has moved. That also rejects the reads of a missing or not yet running cartridge, where both read the same.
- **The timeout is a spin count, not time**, and it is per module: `COMMAND_TIMEOUT` counts turns of the loop that checks the token, so it is shorter on a faster CPU, and a module that defines its own before including `inc/sidecart_layout.s` keeps it.
- **A retry is a new command.** It carries a fresh token, and the RP may already have executed the attempt whose answer the ST stopped waiting for. A command must be harmless to run twice, or carry its own sequence number.
- **The RP answers first**, from its main loop, which drains the command ring on every pass without waiting. Measured on an ST: about 2 ms per command with a 4-byte payload, 3 ms with a 1 KB payload.
- **A Mega STE's cache must be off while the ST talks to the cartridge**; its speed does not matter. `main.s` turns it off from `pre_auto` for the boot screen and puts the user's setting back in `boot_gem`. Any other module wraps each send in `megaste_cache_off` / `megaste_cache_back`.
- **A 68030 (TT, Falcon) needs its instruction cache cleared** after code is copied into RAM and run. The senders do it when they copy their wait loop.

- **The cartridge window is read-only from the ST.** Writes to `$FA0000`-`$FAFFFF` are silently lost on the bus. The ST changes what the RP holds by sending a command, such as `CMD_SET_SHARED_VAR` below.

If an m68k module hooks a trap (GEMDOS, BIOS, XBIOS) to stay resident, md-drives-emulator learned these on hardware:

- **Push nothing on the caller's stack before knowing the call is yours.** TOS 1.04 starts GEM on a 132-byte stack and calls GEMDOS from it with about 110 bytes to spare; decide from `d0` (and `a0` for a user-mode caller) and run the calls you take on a stack of your own.
- **Find the arguments where the caller left them.** From supervisor mode they are on `sp` (plus 2 when `_longframe` is set, on a 68010 or later); from user mode they are at `usp` as it is, since the format word goes on the supervisor stack only.
- **Code reached from a trap's `rte` runs in the caller's mode.** In user mode the first 2 KB of memory is a bus error: no system variables there, and no sender that copies its wait loop (it reads `_dskbufp` at `$4C6`).
- **Give the caller its registers back as TOS does**: every one but `d0` to a supervisor-mode caller, all but `a0` to a user-mode one. Programs count on it.

`$FF00` (`CMD_SET_SHARED_VAR`, `CHANDLER_SET_SHARED_VAR` on the RP) is answered by the RP's chandler itself: "set shared variable d3 to d4". `main.s` uses it at every boot, through `detect_hw` and `get_tos_version`, to publish the machine (`_MCH` cookie, 0 for an ST) in shared variable 0 and the TOS version in variable 1.

`$FF01` (`CMD_ST_HELLO`, `CHANDLER_ST_HELLO` on the RP) has no payload and is also answered by the chandler itself. `main.s` sends it first thing at every boot, until it is answered, so the RP knows the ST has booted: `chandler_stPresent()` is true from then on, and `chandler_consumeStBoot()` is true once per boot. `chandler_consumeSharedVarSet()` is true once after the ST has set shared variables with `$FF00`, so whatever shows them can wait for it instead of polling. The RP and the ST reboot independently, so after an RP-only reboot the RP has not heard from the ST, and what the ST published at its boot was cleared with the window.

An example, the boot screen asking for Booster:

```asm
.booster_send:
    send_sync CMD_BOOSTER, 0
    bne.s .booster_send             ; Z clear: every attempt timed out, ask again
```

### Debugging in Visual Studio Code

#### Prerequisites

Before trying to deploy and debug your first microfirmware app, you must deploy in your Pico W the *Booster* app. This app is responsible for downloading the microfirmware app from the micro SD card and flashing it to the Pico W. The *Booster* app is located in the [repository](https://github.com/sidecartridge/rp2-booster-bootloader). Use the debug version of the *Booster* app to deploy it in your Pico W. The debug version contains the `DEV APP` version with the UUID4 `44444444-4444-4444-8444-444444444444`. This UUID4 is used by the *Booster* app to download the microfirmware app from the micro SD card.

On the web interface of the *Booster* app, select the `DEV APP` application and launch it. This will crete the basic structure for the microfirmware settings.  Without this step, the microfirmware app will not work properly.

#### Configuring Visual Studio Code

After modifying paths in the `.vscode/launch.json` file, you can build the project directly from Visual Studio Code.

Review the installation prerequisites in the previous section. 

#### Launching the app

When debugging the app it will be automatically placed at the top memory of the flash memory. This is the place where default apps for the RP2040 microcontroller are placed.

### Building a microfirmware app

#### From the command line

To build a production ready microfirmware app, follow these steps:

1. Clone this repository:

2. Navigate to the cloned repository:

```
cd MYREPO
```

3. Trigger the `build.sh` script to build the firmware:

To build a release version of the app, run the following command:
```sh
./build.sh pico_w release <YOUR UUID4 HERE>
```

To build a debug version of the app, run the following command:
```sh
./build.sh pico_w debug <YOUR UUID4 HERE>
```

All applications need a unique UUID4. You can generate one using the `uuidgen` command in Linux or macOS, or by using an online UUID generator. If you are developing a new app, you can use the UUID4 for development `44444444-4444-4444-8444-444444444444`.

4. The `dist` folder now houses the files `UUID4.uf2`, the `UUID4.json` file with the app description and the `rp.uf2.md5sum` with the MD5 hash of the binary file generated by the build process. 

5. Copy the `UUID4.uf2` and `UUID4.json` file to the micro SD card `/apps` folder. If you want to host your app in a remote repository, you will have to update or add the content of the `UUID4.json` file to the `apps.json` remote file. 



### Issues

#### Docker error

```
Unable to find image 'logronoide/atarist-toolkit-docker-i386:v0.0.1dev' locally
docker: Error response from daemon: manifest for logronoide/atarist-toolkit-docker-i386:v0.0.1dev not found: manifest unknown: manifest unknown.
See 'docker run --help'.
```

Please modify `stcmd` file located in the `/usr/local/bin/` folder to adjust manually the version and the architecture of your computer:

```sh
#!/bin/bash
if [ -z "$ST_WORKING_FOLDER" ]
then
      echo 'ST_WORKING_FOLDER is empty. It should have the absolute path to the source code working folder.'
      exit 1
fi
THEDOCKER="logronoide/atarist-toolkit-docker-x86_64:latest"
THEUSER=$(id -u)
THEGROUP=$(id -g)
docker run -it --rm  -v ${ST_WORKING_FOLDER}:'/tmp' --user "${THEUSER}:${THEGROUP}" ${THEDOCKER} $@
```

[Previous: Hardware interface](/sidecartridge-multidevice/hardware_interface/){: .btn .mr-4 }
[Main](/sidecartridge-multidevice/){: .btn .mr-4 }
[Next: Parameters](/sidecartridge-multidevice/parameters_v2/){: .btn }