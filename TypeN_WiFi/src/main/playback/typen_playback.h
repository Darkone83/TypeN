#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "typen_media.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PSRAM-backed media playback controller.
 *
 * All decode work happens on a dedicated worker task.
 * UI/menu drawing never performs media decode synchronously.
 */
esp_err_t typen_playback_init(void);

/*
 * Queue a media item for immediate playback. The full item is copied.
 */
esp_err_t typen_playback_set(
    const typen_media_item_t *item);

/*
 * Pause/resume frame presentation. When paused, GIF timing is also paused.
 * Decode/open work may finish in the background, but the LCD framebuffer is
 * not touched until resumed/restored.
 */
void typen_playback_pause(bool paused);

/*
 * Restore the most recently completed 800x800 RGB565 presentation frame.
 */
esp_err_t typen_playback_restore(void);

bool typen_playback_has_frame(void);

#ifdef __cplusplus
}
#endif
