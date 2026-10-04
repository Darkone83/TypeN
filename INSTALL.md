# TypeN Installation

## Requirements

- ESP-IDF 5.5.5
- ESP32-P4 TypeN hardware
- external ESP32-C6 with matching ESP-Hosted slave firmware
- microSD card
- USB access to the P4 for initial flashing
- USB access to the C6 for its firmware/debug path

## P4 / C6 wiring

    P4 GPIO18 TX -> C6 GPIO20 RX
    P4 GPIO19 RX <- C6 GPIO21 TX
    P4 GPIO54    -> C6 EN/RST
    GND          -> GND

The current product transport is ESP-Hosted over UART.

Do not substitute the older SDIO wiring from early TypeN bring-up notes.

## Flash layout

TypeN uses a 32 MB flash layout with:

    nvs      data nvs      0x00009000   24K
    phy_init data phy      0x0000F000    4K
    factory  app  factory  0x00010000    8M
    ota_0    app  ota_0    0x00810000    8M
    ota_1    app  ota_1    0x01010000    8M
    otadata  data ota      0x01810000    8K

Application rollback must be enabled:

    CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y

## First installation

The first installation of an OTA-capable TypeN build must be a normal
USB/serial flash so the bootloader and partition table are installed together.

From the project root:

    idf.py build
    idf.py flash
    idf.py monitor

A normal full flash installs the bootloader, partition table, and factory
application.

Do not attempt to introduce a new partition layout using application-only OTA.

## First boot

Expected behavior:

1. The round display initializes.
2. Touch/local UI initializes.
3. Settings and playback initialize.
4. The updater initializes and handles any rollback checkpoint.
5. The microSD card mounts.
6. Media is scanned and the saved item is restored if present.
7. Wi-Fi starts asynchronously through the C6.
8. The browser manager becomes available after Wi-Fi is ready.

The device must remain usable for local SD playback if Wi-Fi startup fails.

## Wi-Fi access

Current network:

    SSID: TypeN Setup
    Security: Open
    Address: 192.168.4.1

Connect a phone/computer directly to that network. The captive-portal helper
may open the TypeN interface automatically. Otherwise open the local TypeN
address in a browser.

## SD layout

The application creates configured directories as needed. Recommended layout:

    /sdcard/typen/media/
    /sdcard/typen/thumbs/
    /sdcard/typen/update/

The thumbnail directory is application-managed and should not be used for
normal media storage.

## Release version

Before producing a release image, set an explicit ESP-IDF project version.
Preferred approaches are:

- a root `version.txt`
- `set(PROJECT_VER "...")` in the root project CMake configuration

The resulting embedded version is shown on the TypeN About and Update pages
and is also printed once in the startup log.
