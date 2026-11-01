# TypeN

<div align=center>

<img src="https://github.com/Darkone83/TypeN/blob/main/img/logo.png" width=600>

<img src="https://github.com/Darkone83/TypeN/blob/main/img/Darkone83.png" width=400> <img src="https://github.com/Darkone83/TypeN/blob/main/img/Nacho.png" width=200>

</div>

**TypeN** is an ESP32-P4-powered 800×800 round media display with touch control, broad still-image and GIF support, SD-backed playback, local Wi-Fi management, and rollback-safe firmware updates.

The project is designed around a simple goal: load media onto the device, choose what to display, and manage it locally without requiring an external router or Internet connection.

---

## Highlights

- 800×800 round MIPI-DSI display
- Direct framebuffer UI with touch control
- JPEG, PNG, animated GIF, BMP, and static WebP playback
- Center-cover rendering for mixed aspect ratios
- JPEG EXIF orientation handling
- PNG and static WebP alpha support
- Recursive SD-card media discovery
- Persistent selected media
- Local thumbnail browser and cache
- Browser-based media upload, selection, and deletion
- Local SoftAP and captive-portal style access
- Optional Admin PIN protection for the management interface
- On-device and WebUI diagnostics
- On-device and WebUI factory reset
- Browser and SD-card firmware updates
- Dual OTA slots with rollback support
- Separate firmware configurations for:
  - **P4 Pico + external C6 slave**
  - **P4 Pico WiFi / onboard C6**

---


### 🌐 TypeN Web Flasher

Install or update TypeN firmware directly from your browser.
No Python, drivers from the installer, or additional software setup required.

**[🚀 Launch TypeN Web Flasher](https://darkone83.github.io/TypeN/webflasher/)**

Supports:
- TypeN P4 Pico
- TypeN P4 Pico WiFi
- TypeN C6 Slave

Requires Chrome or Edge on a desktop computer with USB access.


## Hardware

TypeN targets the ESP32-P4 platform and the 800×800 JD9365 round display used by the Waveshare P4 ecosystem.

Typical TypeN hardware includes:

- Waveshare ESP32-P4 Nano / P4 Nano WiFi
- Waveshare 3.4inch DSI Round Capacitive Touch Display, 800 × 800 Resolution
- microSD card (FAT32 formatted)
- Waveshare ESP32-C6 Zero Wi-Fi coprocessor (only if using the P4 Nano)
- 32 MB flash
- PSRAM-backed media and framebuffer workloads

### Parts and purchase links

The display is common to both TypeN hardware configurations. Choose either the **P4 Pico + external C6 Zero** combination or the **P4 Pico WiFi** with its onboard C6.

| Part | Used for | Purchase |
|---|---|---|
| 800×800 round display | All TypeN builds | [Amazon](https://www.amazon.com/dp/B0GDF8NL4M?ref=ppx_yo2ov_dt_b_fed_asin_title) |
| P4 Pico | External-C6 / UART build | [Amazon](https://www.amazon.com/dp/B0FM3VBHBJ?ref=ppx_yo2ov_dt_b_fed_asin_title) |
| P4 Pico WiFi | Onboard-C6 / SDIO build | [Amazon](https://www.amazon.com/dp/B0FKTK8LC2?ref=ppx_yo2ov_dt_b_fed_asin_title) |
| Waveshare ESP32-C6 Zero | External-C6 / UART build | [Amazon](https://www.amazon.com/waveshare-Development-ESP32-C6FH4-Processors-Integrates/dp/B0CZQPFC48/ref=sr_1_2_pp?crid=2GG41RYMM3YL9&dib=eyJ2IjoiMSJ9.BY6UWLD6RX8rKEuX1O2E1KOPcGAfFFQH6PuIZKAr2WlA2Ti6otPcUr21HXe1WzeqKeQ9wuxNtSN4A5RLIKpsdZ5UWHEly9llfo9ROR_qLDcSJNiqU5qOMRE5g9efR5vvuOGJpp_4uUqEGzpMQSG3a5Tz07pqPBgwhyHwIPxUAld0kru6rsI0sj6hy3_f8OIBDtE-oyS_FG9nD0WoMp6T1O0vXGr6Z5c81WH-5PwSdVcP_yVr_pqlfLbIxNotwO_opobJ8QvfWWH_VA288suIZz-L_AWa1F8E9coT9wBJsAM.5HHJ1Zlvh17O2NM9xbwc6evtwPM_i9JcYhKWOMsv360&dib_tag=se&keywords=esp32+c6+zero&qid=1790887205&s=electronics&sprefix=esp32+c6+zero%2Celectronics%2C602&sr=1-2) |

Amazon listings, pricing, and availability can change over time.

For the two supported configurations:

```text
P4 Pico build
    Display + P4 Pico + ESP32-C6 Zero

P4 Pico WiFi build
    Display + P4 Pico WiFi
    (C6 is onboard; no external C6 Zero required)
```

---

## Supported hardware configurations

TypeN supports two ESP32-C6 arrangements.

### P4 Pico + external C6 slave

The discrete P4 Pico configuration uses **ESP-Hosted over UART**.

| ESP32-P4 | Direction | ESP32-C6 |
|---|---|---|
| GPIO18 — TX | P4 → C6 | GPIO20 — RX |
| GPIO19 — RX | C6 → P4 | GPIO21 — TX |
| GPIO54 — Reset | P4 → C6 | EN / CHIP_PU |
| GND | Common | GND |

```text
ESP32-P4 Pico                         ESP32-C6 Slave
────────────────────────────────────────────────────────

GPIO18  TX  ───────────────────────► GPIO20  RX
GPIO19  RX  ◄─────────────────────── GPIO21  TX
GPIO54      ───────────────────────► EN / CHIP_PU
GND         ──────────────────────── GND
```

The P4 and C6 may share the same 5 V / USB supply, but **do not connect the two regulated 3.3 V outputs together**.

The external C6 may keep its USB connection available for initial flashing, recovery, and serial diagnostics.

See [C6_Slave_Wiring.md](C6_Slave_Wiring.md) for the complete wiring and bring-up guide.

### P4 Pico WiFi / onboard C6

The onboard-C6 variant uses the board's native **4-bit SDIO** connection rather than the external UART wiring above.

Current P4-side Hosted SDIO configuration:

| Signal | ESP32-P4 GPIO |
|---|---:|
| CLK | 18 |
| CMD | 19 |
| D0 | 14 |
| D1 | 15 |
| D2 | 16 |
| D3 | 17 |
| C6 Reset | 54 |

The two variants therefore use separate ESP-Hosted firmware configurations:

```text
P4 Pico + external C6
    ESP-Hosted UART

P4 Pico WiFi / onboard C6
    ESP-Hosted SDIO
```

Do not mix the UART slave firmware/configuration with the onboard SDIO variant.

---

## Software requirements

- **ESP-IDF 5.5.5**
- ESP32-P4 toolchain
- Matching ESP32-C6 slave firmware for the selected hardware variant
- `simplewebp.h` in `main/webp/` for native static WebP decoding

Static WebP support uses the lightweight [SimpleWebP](https://github.com/MikuAuahDark/simplewebp) decoder. The current TypeN integration pins the decoder to commit `d1a728a1f8ec7348ca2a5039b6dd813b83986fbb`.

---

## Building

From an ESP-IDF 5.5.5 environment:

```bash
idf.py set-target esp32p4
idf.py build
```

Before the first build with WebP support, make sure this file exists:

```text
main/webp/simplewebp.h
```

If the repository includes the supplied SimpleWebP fetch script, run it once from the project root before building. The fetch helper downloads the pinned decoder revision into `main/webp/simplewebp.h`.

For an already-configured source tree, avoid replacing `sdkconfig` unless you are intentionally changing the hardware variant.

The P4 Pico and P4 Pico WiFi builds should use separate configuration/source trees so their ESP-Hosted transport settings remain unambiguous.

---

## First installation

A new board should receive a **full USB/UART flash** so the bootloader, partition table, OTA metadata, and factory application are installed together.

Typical development flow:

```bash
idf.py build
idf.py flash
idf.py monitor
```

Do not introduce a new partition layout through an application-only OTA update.

For more detailed setup instructions, see [INSTALL.md](INSTALL.md).

---

## Flash layout

The current TypeN layout keeps OTA metadata and all boot-critical application slots below the 16 MB boundary while retaining the board's 32 MB physical flash:

| Partition | Type | Offset | Size |
|---|---|---:|---:|
| NVS | data | `0x00009000` | 24 KB |
| PHY init | data | `0x0000F000` | 4 KB |
| OTA data | data | `0x00010000` | 8 KB |
| Factory | app | `0x00020000` | 5 MB |
| OTA 0 | app | `0x00520000` | 5 MB |
| OTA 1 | app | `0x00A20000` | 5 MB |

`ota_1` ends at `0x00F20000`, so the complete boot/OTA application set remains below 16 MB.

For builds using ESP-IDF automatic rollback, enable:

```text
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

---

## Boot behavior

TypeN initializes the local display/media path first so the device remains useful even when Wi-Fi is unavailable.

Expected startup sequence:

```text
display
→ touch / local UI
→ settings
→ playback
→ updater / rollback checkpoint
→ microSD
→ media scan
→ restore selected media
→ C6 / Wi-Fi
→ local web manager
```

A missing or unavailable C6 should not prevent local SD media playback on the external-UART configuration.

---

## SD-card layout

Recommended directories:

```text
/sdcard/typen/media/
/sdcard/typen/thumbs/
/sdcard/typen/update/
```

- `media/` — normal uploaded media
- `thumbs/` — TypeN-managed thumbnail cache
- `update/` — SD firmware update location

The media scanner is recursive, while the configured thumbnail-cache directory is excluded from the media library.

Default SD firmware path:

```text
/sdcard/typen/update/TypeN.bin
```

---

## Local web interface

TypeN exposes a local control surface through its ESP32-C6 Wi-Fi link.

Current network:

```text
SSID: TypeN Setup
Security: Open
Address: 192.168.4.1
```

Management access can optionally be protected with a **4–8 digit Admin PIN** from the TypeN settings page. The PIN protects the management interface rather than changing the SoftAP password.

The web interface provides:

- media library browsing
- JPEG, PNG, GIF, BMP, and static WebP upload
- display selection
- delete confirmation
- brightness/settings control
- optional Admin PIN management
- diagnostics and support information
- factory reset
- firmware update
- update status and rollback information
- firmware/build identity

Admin sessions expire after inactivity, and repeated failed PIN attempts are rate-limited.

### Diagnostics

The WebUI Diagnostics page exposes useful support information including:

- uptime and reset reason
- internal heap and PSRAM usage
- SD-card total/free space
- current media and media count
- Wi-Fi/IP status
- firmware version, build, running slot, and image state
- updater status and rollback availability
- last browser transfer status

The on-device **System → Diagnostics** page provides a smaller local snapshot of the same core health information.

### Factory reset

Factory reset is available both on-device and from the WebUI.

A factory reset clears TypeN configuration such as:

- selected media
- brightness
- upload/thumbnail paths
- Admin PIN
- other settings stored in the TypeN NVS namespace

Factory reset **does not erase SD-card media and does not rewrite firmware or OTA partitions**.

---

## Media support

TypeN currently supports:

- **JPEG** — hardware-assisted decode with EXIF orientation handling
- **PNG** — static images with alpha support
- **GIF** — static and animated GIF playback
- **BMP** — uncompressed indexed, 16-bit, 24-bit, and 32-bit Windows BMP variants
- **WebP** — static lossy and lossless WebP, including alpha

Animated WebP is intentionally not supported. If a WebP file advertises animation, TypeN rejects it rather than displaying only a partial frame sequence.

BMP support covers the common uncompressed forms used in normal image workflows, including palette-based BMP and RGB555/RGB565 bitfields. RLE-compressed BMP is not currently supported.

Playback uses center-cover rendering so content fills the round 800×800 display without stretching.

Local thumbnails are generated and cached separately from playback media, and the cache directory is excluded from recursive media discovery.

---

## Firmware updates

TypeN supports two application-update paths:

1. **Browser upload**
2. **SD-card update**

Normal application OTA uses:

```text
TypeN.bin
```

Do **not** use these as application OTA payloads:

```text
bootloader.bin
partition-table.bin
merged/full-flash images
```

Those belong to the full USB/UART flash workflow.

### Browser update

The browser updater preflights the firmware before the confirmation dialog.

It validates:

- ESP application image header
- application descriptor
- project identity
- full image size
- inactive OTA slot availability
- target-slot capacity

The full upload repeats the checks before opening the OTA writer.

Successful flow:

```text
UPLOAD
→ WRITE
→ VERIFY
→ REBOOT
→ CONFIRM
```

### SD update

The SD updater performs the same type of application/slot validation before installation and shows the current and target firmware information before the write begins.

See [UPDATE_RECOVERY.md](UPDATE_RECOVERY.md) for the full update and rollback behavior.

---

## OTA and rollback

Normal application updates rotate through the inactive OTA slots:

```text
factory
  ↓
ota_0
  ↓
ota_1
  ↓
ota_0
  ↓
...
```

A newly installed OTA image is not considered permanently valid immediately.

TypeN confirms the new image only after the core local system has initialized successfully. If the new firmware cannot reach that checkpoint, ESP-IDF can return to the previous working image.

The Update page reports:

- current firmware version
- running slot
- configured boot slot
- image state
- last update result
- last attempted/confirmed version
- rollback availability
- last invalid/aborted OTA image when available

---

## Recovery

If a firmware image fails preflight or final verification, the current running firmware remains active.

If a newly installed OTA image fails during its first boot before confirmation, the bootloader rollback path can return to the previous working image.

If OTA recovery is not available, perform a known-good full USB/UART flash:

```bash
idf.py build
idf.py flash
idf.py monitor
```

A full flash is also required whenever the bootloader or partition table changes.
