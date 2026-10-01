#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque GIF playback handle.
 */
typedef struct typen_gif_player typen_gif_player_t;

/*
 * Browser thumbnail/preview.
 */
esp_err_t typen_gif_draw_preview(
    const char *path,
    uint16_t *framebuffer,
    int framebuffer_width,
    int framebuffer_height,
    int x,
    int y,
    int width,
    int height,
    uint16_t background);

/*
 * Incremental playback.
 *
 * The compressed GIF is cached in PSRAM once at open. Each next_frame call
 * updates only the current GIF frame/disposal region in the supplied RGB565
 * framebuffer and returns the vertical dirty range which needs presenting.
 */
esp_err_t typen_gif_player_open(
    const char *path,
    typen_gif_player_t **out_player);

esp_err_t typen_gif_player_next_frame(
    typen_gif_player_t *player,
    uint16_t *framebuffer,
    int framebuffer_width,
    int framebuffer_height,
    uint16_t background,
    uint32_t *delay_ms,
    int *dirty_y_start,
    int *dirty_y_end);

void typen_gif_player_close(
    typen_gif_player_t *player);

#ifdef __cplusplus
}
#endif
