# Publish this TypeN Web Flasher

**Use the contents of this folder — the ZIP itself is not a web page.**

## New GitHub Pages repository

1. Create a public repository (for example `TypeN-WebFlasher`).
2. Upload **all extracted files and folders** from this package at the repository root, including `index.html`, `assets/`, and `firmware/`.
3. Open GitHub **Settings → Pages → Build and deployment**.
4. Select **Deploy from a branch**, **main**, **/(root)**, and Save.
5. Open the HTTPS URL shown in Pages settings, in desktop Chrome or Edge.
6. Connect a TypeN with a USB data cable, choose the correct variant, and select **Connect & install**.

## Existing TypeN repository

Put these extracted files under a `docs/` directory in that repository. Set GitHub Pages to serve **main /docs** instead.

## Hardware warning

- **P4 Pico**: P4 firmware for an external C6 on UART. External C6 requires a second USB installation with **External C6 Slave**.
- **P4 Pico WiFi**: P4 firmware for the built-in C6 over SDIO. The **External C6 Slave** package is not intended for its onboard C6.
- **External C6 Slave**: firmware for the separate ESP32-C6 Zero only.

This USB recovery/full install may erase onboard settings and OTA state. For regular app-only updates on working TypeN hardware, use TypeN's built-in WebUI/SD updater instead.

The static site depends on `esp-web-tools@10.4.0` loaded over the internet; no Python, pip, build system, or npm is required for users. A browser with Web Serial and HTTPS is required. This bundle includes the complete firmware binaries for all three targets.

The developer ZIP includes the standard-library-only script used to rebuild the site firmware and manifests from a future TypeN release ZIP.


## Repair package (v2)

- The flasher now flashes 4 independently addressed original segments per target.
- The bootloader bytes carry the flash header already patched for DIO, 80MHz and the correct physical flash size.
- The page downloads and hashes each published image before enabling the Install button.
- If the firmware checks pass and ESP Web Tools says *Failed to install*, copy the **complete error message** in its dialog and verify the selected serial port and BOOT/RESET mode.
- Replace the **entire** `docs/webflasher` directory to avoid stale binaries, `catalog.json` or manifests.
- Use Ctrl+Shift+R on the deployed page after publishing.
