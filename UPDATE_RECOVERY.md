# TypeN Firmware Update and Recovery

## Supported application update paths

TypeN can install the normal ESP-IDF application binary through:

- the browser Update page
- a `.bin` file stored on the SD card

For normal builds the application image is:

    build/TypeN.bin

Do not use these as application OTA payloads:

- `bootloader.bin`
- `partition-table.bin`
- a merged/full-flash image

Those belong to the USB/serial full-flash workflow.

## Browser update

The browser update path performs a preflight before the confirmation dialog.

Only the first 512 bytes are read locally by the browser and sent to TypeN for
preflight; the full firmware file is not transmitted until the user confirms.

The preflight checks:

- complete file size is large enough for an ESP application
- image header magic is valid
- ESP application descriptor is present and valid
- project name matches the running TypeN application
- an inactive OTA partition is available
- the complete image fits the inactive OTA slot

The confirmation dialog then shows:

- project
- version
- full image size
- current slot
- target slot

The full upload repeats the same prefix checks on the actual incoming stream
before `esp_ota_begin()` is called. An invalid bootloader, partition table,
merged image, unrelated application, truncated file, or oversized image is
therefore rejected before the inactive OTA partition is erased/written.

After preflight the update sequence is:

    UPLOADING
    -> WRITING
    -> VERIFYING
    -> REBOOTING

ESP-IDF performs full application-image verification before TypeN selects the
new boot partition.

The browser upload also has bounded receive-timeout handling. A dead transfer
is aborted instead of holding the updater/socket indefinitely.

## SD update

Default firmware path:

    /sdcard/typen/update/TypeN.bin

The SD updater performs synchronous preflight before installation. It checks
that the file is a valid TypeN application image, identifies the embedded
project/version, verifies that it fits the inactive OTA slot, and shows the
current and target slots before installation.

After confirmation:

    PREPARING
    -> WRITING
    -> VERIFYING
    -> REBOOTING

On success the inactive OTA slot becomes the next boot target.

## OTA slots

The application slots are:

    factory
    ota_0
    ota_1

Normal application updates alternate between the inactive OTA slots.

A healthy sequence can therefore look like:

    factory -> ota_0 -> ota_1 -> ota_0

The Update page reports both the running and configured boot slot.

## Rollback checkpoint

Rollback support depends on:

    CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y

A newly installed OTA image starts in the ESP-IDF pending-verification state.

TypeN confirms the image only after the display, touch, settings, playback,
and local UI have initialized successfully and the application reaches its
core-ready updater checkpoint.

If the new application cannot reach that checkpoint, ESP-IDF can return to
the previous working image on the following boot.

The Update page records and reports:

- current firmware version
- running/boot slot
- image state
- last update result
- last attempted/confirmed version and slot
- rollback availability
- last invalid/aborted OTA image when available

## Recovery

If an application update fails preflight or final verification before reboot,
the current running firmware remains active.

If a newly installed OTA image fails during first boot before confirmation,
allow the bootloader rollback path to return to the previous image.

If application-level OTA is no longer usable, recover over USB/serial with a
known-good full build:

    idf.py build
    idf.py flash
    idf.py monitor

A full USB/serial flash is also the correct path whenever the bootloader or
partition table needs to change.

## Factory partition

The factory application remains a separate 8 MB application partition in the
current layout. Do not erase or repurpose it casually; it is part of the
device's recovery strategy.

## Before distributing an update

Verify:

- embedded project name is TypeN
- embedded version is intentional
- application binary fits the 8 MB OTA slot
- current partition table is unchanged
- rollback support is enabled
- one known-good image is already bootable
- browser preflight rejects bootloader/partition-table/merged images
- browser OTA completes, verifies, reboots, and confirms
- SD update completes and reboots correctly
- the new image reaches confirmation and reports `valid`
