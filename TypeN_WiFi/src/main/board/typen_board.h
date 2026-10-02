#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TYPEN_LCD_W 800
#define TYPEN_LCD_H 800

#define TYPEN_TOUCH_MAX_POINTS 5

esp_err_t typen_board_display_early_start(void);
esp_err_t typen_board_ui_start(void);

uint16_t *typen_board_framebuffer(void);

/*
 * Playback uses two DPI-owned back buffers separate from the UI framebuffer.
 * GIF composition is done at native source resolution and the ESP32-P4 PPA
 * scales into the inactive back buffer before the buffer is presented.
 */
size_t typen_board_playback_buffer_count(void);
uint16_t *typen_board_playback_buffer(size_t index);

esp_err_t typen_board_present_buffer(uint16_t *buffer);

/*
 * Cache-sync/present only a vertical range of a DPI-owned framebuffer.
 * The ESP-IDF DSI driver recognizes the native framebuffer pointer and does
 * not memcpy the full image.
 */
esp_err_t typen_board_present_buffer_rows(
    uint16_t *buffer,
    int y_start,
    int y_end);

esp_err_t typen_board_present(void);

esp_err_t typen_board_touch_sample(
    uint16_t *x,
    uint16_t *y,
    bool *pressed);

/*
 * Multi-touch companion to typen_board_touch_sample().
 * The existing single-point API remains unchanged for normal callers.
 */
esp_err_t typen_board_touch_sample_points(
    uint16_t *x,
    uint16_t *y,
    uint8_t max_points,
    uint8_t *point_count);

esp_err_t typen_board_set_brightness(uint8_t percent);
uint32_t typen_board_first_frame_ms(void);

#ifdef __cplusplus
}
#endif
