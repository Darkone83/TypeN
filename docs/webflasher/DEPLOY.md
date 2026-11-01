# TypeN GitHub Pages / direct USB installer

1. Copy all files from `site/` into your main TypeN repository at `docs/webflasher/`.
2. In GitHub repository Settings → Pages, select Deploy from a branch → main → /docs.
3. Open the Pages URL ending with `/webflasher/` in desktop Chrome or Edge.
4. Plug the P4's **Type-C programming/UART port** into the computer. The 4-pin USB-OTG connector is not the Type-C programming port.
5. Select the exact P4 or C6 firmware variant. The page checks all four files and SHA-256 hashes.
6. Click Connect & install, choose the USB serial port, and allow the automatic reset attempts to complete.
7. If initialization still fails, open Connection details and save the diagnostic log for troubleshooting. BOOT should not be needed on boards with a functional auto-download reset circuit, but software cannot implement a circuit that the hardware lacks.

This edition switches from esp-web-tools 10.4.0 to direct esptool-js 0.7.0. It includes P4-specific fixes and real connection diagnostics. Physical USB flashing was not available for testing during packaging.

For future firmware updates use `tools/build_webflasher.py` from the source kit. Do not mix firmware or manifest files from different published versions.
