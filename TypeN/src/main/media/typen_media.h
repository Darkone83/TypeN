#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TYPEN_MEDIA_UNKNOWN = 0,
    TYPEN_MEDIA_JPEG,
    TYPEN_MEDIA_GIF,
    TYPEN_MEDIA_PNG,
    TYPEN_MEDIA_BMP,
    TYPEN_MEDIA_WEBP,
} typen_media_type_t;

typedef struct {
    char name[256];
    char path[512];
    typen_media_type_t type;
    size_t size;
} typen_media_item_t;

esp_err_t typen_media_init(void);

/*
 * Recursively scans the entire mounted SD card, not only /sdcard/typen/media.
 * Nested folders are flattened into one browser catalog while the full path is
 * retained in each item for later playback/NVS selection.
 */
esp_err_t typen_media_scan(void);

size_t typen_media_count(void);
esp_err_t typen_media_get(size_t index, typen_media_item_t *out);

/*
 * Matches full path first, then basename for compatibility with web callers.
 * Full paths are the preferred stable identifier because duplicate filenames
 * can exist in different folders.
 */
esp_err_t typen_media_find(const char *name_or_path, typen_media_item_t *out);

/*
 * Deletes a catalogued media file by full path and removes that item from the
 * live in-memory catalog. It deliberately does not perform a recursive rescan
 * from the caller's task. The caller should refresh UI/thumbnail state after
 * success.
 */
esp_err_t typen_media_delete(const char *path);

const char *typen_media_type_name(typen_media_type_t type);

#ifdef __cplusplus
}
#endif
