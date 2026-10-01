#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "typen_settings.h"

#define TYPEN_NVS_NAMESPACE    "typen"
#define TYPEN_NVS_SELECTED     "selected_media"
#define TYPEN_NVS_UPLOAD_PATH  "upload_path"
#define TYPEN_NVS_THUMB_PATH   "thumb_path"
#define TYPEN_NVS_BRIGHTNESS   "brightness"

static const char *TAG = "typen_settings";

static esp_err_t open_namespace(
    nvs_open_mode_t mode,
    nvs_handle_t *handle)
{
    return nvs_open(
        TYPEN_NVS_NAMESPACE,
        mode,
        handle
    );
}

static bool valid_sd_path(
    const char *path)
{
    if (path == NULL ||
        path[0] == '\0') {
        return false;
    }

    const size_t len = strlen(path);

    if (len >= TYPEN_SETTINGS_PATH_MAX) {
        return false;
    }

    if (strcmp(path, "/sdcard") != 0 &&
        strncmp(path, "/sdcard/", 8) != 0) {
        return false;
    }

    if (strstr(path, "..") != NULL ||
        strchr(path, '\\') != NULL) {
        return false;
    }

    for (const char *p = path; *p; ++p) {
        if ((unsigned char)*p < 32) {
            return false;
        }
    }

    return true;
}

static esp_err_t set_string(
    const char *key,
    const char *value)
{
    nvs_handle_t handle = 0;

    esp_err_t err =
        open_namespace(
            NVS_READWRITE,
            &handle
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        nvs_set_str(
            handle,
            key,
            value
        );

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static esp_err_t get_string(
    const char *key,
    char *value,
    size_t value_capacity)
{
    if (value == NULL ||
        value_capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    value[0] = '\0';

    nvs_handle_t handle = 0;

    esp_err_t err =
        open_namespace(
            NVS_READONLY,
            &handle
        );

    if (err != ESP_OK) {
        return err;
    }

    size_t required = 0;

    err =
        nvs_get_str(
            handle,
            key,
            NULL,
            &required
        );

    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    if (required == 0 ||
        required > value_capacity) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    err =
        nvs_get_str(
            handle,
            key,
            value,
            &required
        );

    nvs_close(handle);
    return err;
}

static esp_err_t get_string_default(
    const char *key,
    const char *default_value,
    char *value,
    size_t value_capacity)
{
    esp_err_t err =
        get_string(
            key,
            value,
            value_capacity
        );

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        const size_t needed =
            strlen(default_value) + 1U;

        if (needed > value_capacity) {
            return ESP_ERR_INVALID_SIZE;
        }

        memcpy(
            value,
            default_value,
            needed
        );

        return ESP_OK;
    }

    return err;
}

esp_err_t typen_settings_init(void)
{
    esp_err_t err =
        nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(
            TAG,
            "NVS requires reinitialization: %s",
            esp_err_to_name(err)
        );

        err =
            nvs_flash_erase();

        if (err != ESP_OK) {
            return err;
        }

        err =
            nvs_flash_init();
    }

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "NVS ready"
        );
    }

    return err;
}

esp_err_t typen_settings_set_selected_media(
    const char *path)
{
    if (!valid_sd_path(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        set_string(
            TYPEN_NVS_SELECTED,
            path
        );

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Selected media saved: %s",
            path
        );
    }

    return err;
}

esp_err_t typen_settings_get_selected_media(
    char *path,
    size_t path_capacity)
{
    return get_string(
        TYPEN_NVS_SELECTED,
        path,
        path_capacity
    );
}

esp_err_t typen_settings_set_upload_path(
    const char *path)
{
    if (!valid_sd_path(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        set_string(
            TYPEN_NVS_UPLOAD_PATH,
            path
        );

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Upload path saved: %s",
            path
        );
    }

    return err;
}

esp_err_t typen_settings_get_upload_path(
    char *path,
    size_t path_capacity)
{
    return get_string_default(
        TYPEN_NVS_UPLOAD_PATH,
        TYPEN_DEFAULT_UPLOAD_PATH,
        path,
        path_capacity
    );
}

esp_err_t typen_settings_set_thumbnail_path(
    const char *path)
{
    if (!valid_sd_path(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        set_string(
            TYPEN_NVS_THUMB_PATH,
            path
        );

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Thumbnail path saved: %s",
            path
        );
    }

    return err;
}

esp_err_t typen_settings_get_thumbnail_path(
    char *path,
    size_t path_capacity)
{
    return get_string_default(
        TYPEN_NVS_THUMB_PATH,
        TYPEN_DEFAULT_THUMBNAIL_PATH,
        path,
        path_capacity
    );
}

esp_err_t typen_settings_set_brightness(
    uint8_t percent)
{
    if (percent < TYPEN_BRIGHTNESS_MIN ||
        percent > TYPEN_BRIGHTNESS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;

    esp_err_t err =
        open_namespace(
            NVS_READWRITE,
            &handle
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        nvs_set_u8(
            handle,
            TYPEN_NVS_BRIGHTNESS,
            percent
        );

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Brightness saved: %u%%",
            (unsigned)percent
        );
    }

    return err;
}

esp_err_t typen_settings_get_brightness(
    uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;

    esp_err_t err =
        open_namespace(
            NVS_READONLY,
            &handle
        );

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *percent =
            TYPEN_DEFAULT_BRIGHTNESS;

        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    uint8_t stored = 0;

    err =
        nvs_get_u8(
            handle,
            TYPEN_NVS_BRIGHTNESS,
            &stored
        );

    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *percent =
            TYPEN_DEFAULT_BRIGHTNESS;

        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    if (stored < TYPEN_BRIGHTNESS_MIN ||
        stored > TYPEN_BRIGHTNESS_MAX) {
        *percent =
            TYPEN_DEFAULT_BRIGHTNESS;
    } else {
        *percent = stored;
    }

    return ESP_OK;
}
