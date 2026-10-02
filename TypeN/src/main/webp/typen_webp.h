#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode a static WebP image and render it CENTER + COVER into the requested
 * RGB565 destination box. Lossy, lossless and alpha are supported.
 * Animated WebP is intentionally rejected.
 *
 * This module uses the BSD-3-Clause SimpleWebP single-header decoder. Place
 * simplewebp.h beside typen_webp.c before building.
 */
esp_err_t typen_webp_draw_preview(
    const char *path,
    uint16_t *dst,
    int dst_width,
    int dst_height,
    int box_x,
    int box_y,
    int box_w,
    int box_h,
    uint16_t background);

#ifdef __cplusplus
}
#endif
