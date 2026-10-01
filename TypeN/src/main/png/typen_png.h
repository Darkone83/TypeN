#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode a PNG and draw an aspect-fit preview directly into an RGB565
 * framebuffer rectangle.
 */
esp_err_t typen_png_draw_preview(
    const char *path,
    uint16_t *framebuffer,
    int framebuffer_width,
    int framebuffer_height,
    int x,
    int y,
    int width,
    int height,
    uint16_t background);

#ifdef __cplusplus
}
#endif
