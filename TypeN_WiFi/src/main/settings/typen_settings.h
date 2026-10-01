#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TYPEN_SETTINGS_PATH_MAX          512

#define TYPEN_DEFAULT_UPLOAD_PATH        "/sdcard/typen/media"
#define TYPEN_DEFAULT_THUMBNAIL_PATH     "/sdcard/typen/thumbs"

#define TYPEN_BRIGHTNESS_MIN             5
#define TYPEN_BRIGHTNESS_MAX             100
#define TYPEN_DEFAULT_BRIGHTNESS         40

esp_err_t typen_settings_init(void);

esp_err_t typen_settings_set_selected_media(
    const char *path);

esp_err_t typen_settings_get_selected_media(
    char *path,
    size_t path_capacity);

esp_err_t typen_settings_set_upload_path(
    const char *path);

esp_err_t typen_settings_get_upload_path(
    char *path,
    size_t path_capacity);

esp_err_t typen_settings_set_thumbnail_path(
    const char *path);

esp_err_t typen_settings_get_thumbnail_path(
    char *path,
    size_t path_capacity);

esp_err_t typen_settings_set_brightness(
    uint8_t percent);

esp_err_t typen_settings_get_brightness(
    uint8_t *percent);

#ifdef __cplusplus
}
#endif
