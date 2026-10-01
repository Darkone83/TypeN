# TypeN Final Browser OTA Pass

Changed source files:

- `web/typen_web.c`
- `update/typen_update.c`
- `update/typen_update.h`

No partition-table, `main.c`, Wi-Fi, playback, display, or ESP-Hosted transport
changes are made in this pass.

## Browser preflight flow

Selecting a browser firmware file now performs a small preflight before the
install confirmation appears.

    browser reads first 512 bytes
        ->
    POST /api/update/upload/check?size=<full file size>
        ->
    TypeN validates application header / app descriptor / project / size / slot
        ->
    confirmation shows project, version, size and slot transition
        ->
    user confirms
        ->
    full application upload starts

Only the first 512 bytes are transmitted during the initial check.

## Server-side repeat validation

The full `/api/update/upload` request does not trust the earlier browser check.

It first buffers the incoming 512-byte prefix and runs the same TypeN
application-image validation before opening the OTA writer.

Only after that succeeds does TypeN call `esp_ota_begin()` and write the
inactive slot.

This protects the update target if:
- the wrong file is selected
- a bootloader is uploaded
- a partition-table binary is uploaded
- a merged/full-flash binary is uploaded
- an unrelated ESP application is uploaded
- the image is truncated
- the image is larger than the OTA partition

Final full-image validation still comes from `esp_ota_end()`.

## Transfer failure behavior

Browser OTA uses an 8 KiB streaming buffer and bounded receive-timeout retries.
Repeated network stalls abort the writer and leave the currently running boot
selection unchanged.

## Expected successful confirmation

The dialog should look conceptually like:

    TypeN.bin
    Project: TypeN
    Version: <embedded version>
    Size: <application size>
    Slot: ota_0 -> ota_1

After installation:

    WRITING
    -> VERIFYING
    -> REBOOTING
    -> reconnect
    -> CONFIRMED / VALID

## Final validation set

Test at least:

1. valid application image
2. bootloader image
3. partition-table image
4. merged/full-flash image
5. unrelated ESP application
6. truncated TypeN application
7. interrupted valid upload
8. normal successful `ota_0 -> ota_1` or `ota_1 -> ota_0` update

The invalid cases should be rejected before the target OTA partition is opened
for writing.
