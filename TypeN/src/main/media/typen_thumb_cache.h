#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TYPEN_THUMB_W 196
#define TYPEN_THUMB_H 88

typedef enum {
    TYPEN_THUMB_MISS = 0,
    TYPEN_THUMB_LOADING,
    TYPEN_THUMB_READY,
    TYPEN_THUMB_FAILED,
} typen_thumb_state_t;

/*
 * Lazy PSRAM thumbnail cache.
 *
 * Decode work runs on a dedicated worker task with its own larger stack.
 * UI/touch code never performs JPEG/PNG/GIF decode synchronously.
 */
esp_err_t typen_thumb_cache_init(void);
void typen_thumb_cache_reset(void);

/*
 * Shared 196x88 RGB565 renderer used by both the local PSRAM thumbnail cache
 * and the HTTP persistent-thumbnail cache. The caller owns the destination
 * buffer, which must contain TYPEN_THUMB_W * TYPEN_THUMB_H uint16_t pixels.
 */
esp_err_t typen_thumb_render_item(
    const typen_media_item_t *item,
    uint16_t *pixels);

/*
 * Returns the current state for media index.
 *
 * MISS automatically queues a background decode. When READY, *pixels points
 * to a TYPEN_THUMB_W x TYPEN_THUMB_H RGB565 PSRAM buffer owned by the cache.
 */
typen_thumb_state_t typen_thumb_cache_get(
    size_t media_index,
    const uint16_t **pixels);

/*
 * True once after one or more background thumbnails changed state.
 * The UI uses this to redraw only when there is something new to show.
 */
bool typen_thumb_cache_take_dirty(void);

/*
 * Pause/resume background thumbnail decoding.
 * Playback pauses the cache while media is on screen so SD/PSRAM bandwidth
 * is reserved for the active GIF/display path.
 */
void typen_thumb_cache_set_paused(bool paused);

#ifdef __cplusplus
}
#endif
