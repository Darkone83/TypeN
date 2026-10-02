#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode a Windows BMP directly from SD and render it CENTER + COVER into
 * the requested RGB565 destination box.
 *
 * Supported BMP pixel formats:
 * - BI_RGB: 1, 4, 8, 16, 24 and 32 bpp
 * - BI_BITFIELDS / BI_ALPHABITFIELDS: 16 and 32 bpp
 * - top-down and bottom-up row order
 *
 * RLE/compressed BMP variants are intentionally rejected.
 */
esp_err_t typen_bmp_draw_preview(
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
