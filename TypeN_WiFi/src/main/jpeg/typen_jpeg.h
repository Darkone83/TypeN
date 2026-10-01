#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Direct RGB565 decode surface. No LVGL types are used.
 */
typedef struct {
    uint8_t *pixels;
    size_t pixels_allocated;

    uint16_t width;
    uint16_t height;
    uint16_t stride_pixels;

    /*
     * JPEG EXIF orientation, normalized to the standard 1..8 range.
     * 1 means the decoded pixel matrix is already display-oriented.
     */
    uint8_t orientation;
} typen_jpeg_frame_t;

esp_err_t typen_jpeg_init(void);
esp_err_t typen_jpeg_decode_file(
    const char *path,
    typen_jpeg_frame_t *out);

void typen_jpeg_free(typen_jpeg_frame_t *frame);

#ifdef __cplusplus
}
#endif
