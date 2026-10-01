#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "typen_media.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Direct-framebuffer UI. No LVGL runtime is used.
 */
esp_err_t typen_ui_init(void);

void typen_ui_set_status(const char *text);
void typen_ui_set_wifi_status(const char *text);
void typen_ui_set_media_count(size_t count);

/*
 * Brightness is shared by the local touch UI and the HTTP control surface.
 * Values are persisted through typen_settings.
 */
esp_err_t typen_ui_set_brightness(uint8_t percent);
uint8_t typen_ui_get_brightness(void);

/*
 * Media rendering is intentionally deferred until the UX pass is locked.
 */
esp_err_t typen_ui_show_media(const typen_media_item_t *item);

#ifdef __cplusplus
}
#endif
