# TypeN Release Checklist

## Build identity

- Set the intended project version.
- Build with ESP-IDF 5.5.1.
- Confirm the About page shows the expected version/build.
- Confirm the startup log prints project, version, slot, and IDF version.

## Startup

- Display starts normally.
- Touch UI opens with the long press.
- SD mounts before Wi-Fi startup is allowed to affect the media path.
- Recursive media scan succeeds.
- Previously selected media restores.
- Device still supports local SD playback with the C6 unavailable.

## Media

- JPEG center-cover works.
- EXIF-rotated JPEG displays correctly.
- PNG center-cover works.
- Animated GIF center-cover works without watchdog resets.
- Transparent PNG/GIF behavior is acceptable.
- Local thumbnail browser remains responsive.
- Thumbnail cache directory does not appear as media.
- Deleted media does not leave an active thumbnail entry.

## Web manager

- `TypeN Setup` AP is visible and open.
- Local page opens at the TypeN address.
- Media list loads.
- JPEG/PNG/GIF upload succeeds.
- Interrupted replacement upload does not destroy the existing file.
- Delete confirmation works.
- Select/display action works.
- Settings persist.
- Repeated reloads do not produce HTTP socket exhaustion.

## Update / recovery

- SD firmware preflight identifies version/target correctly.
- SD update writes, verifies, reboots, and confirms.
- `factory -> ota_0 -> ota_1 -> ota_0` slot rotation is healthy.
- Running slot and boot slot match after successful reboot.
- Confirmed OTA image reports `valid`.
- Recovery/rollback status is readable.
- Full USB/serial recovery procedure is available.

## Final packaging

- Include `README.md`.
- Include `INSTALL.md`.
- Include `UPDATE_RECOVERY.md`.
- Include the correct application `.bin` for OTA users.
- Clearly distinguish application `.bin` from bootloader, partition-table, and
  merged/full-flash images.
