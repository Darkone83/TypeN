#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"

#include "typen_settings.h"

#define TYPEN_NVS_NAMESPACE    "typen"
#define TYPEN_NVS_SELECTED     "selected_media"
#define TYPEN_NVS_UPLOAD_PATH  "upload_path"
#define TYPEN_NVS_THUMB_PATH   "thumb_path"
#define TYPEN_NVS_BRIGHTNESS   "brightness"
#define TYPEN_NVS_ADMIN_SALT   "admin_salt"
#define TYPEN_NVS_ADMIN_HASH   "admin_hash"

#define TYPEN_ADMIN_SALT_BYTES 16
#define TYPEN_ADMIN_HASH_BYTES 32

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


static bool valid_admin_pin(
    const char *pin)
{
    if (pin == NULL) {
        return false;
    }

    const size_t len =
        strlen(pin);

    if (len < TYPEN_ADMIN_PIN_MIN_DIGITS ||
        len > TYPEN_ADMIN_PIN_MAX_DIGITS) {
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        if (pin[i] < '0' ||
            pin[i] > '9') {
            return false;
        }
    }

    return true;
}

static esp_err_t hash_admin_pin(
    const uint8_t salt[TYPEN_ADMIN_SALT_BYTES],
    const char *pin,
    uint8_t hash[TYPEN_ADMIN_HASH_BYTES])
{
    if (salt == NULL ||
        pin == NULL ||
        hash == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);

    int rc =
        mbedtls_sha256_starts(
            &ctx,
            0
        );

    if (rc == 0) {
        rc =
            mbedtls_sha256_update(
                &ctx,
                salt,
                TYPEN_ADMIN_SALT_BYTES
            );
    }

    if (rc == 0) {
        static const unsigned char domain[] =
            "TypeN admin PIN v1";

        rc =
            mbedtls_sha256_update(
                &ctx,
                domain,
                sizeof(domain) - 1U
            );
    }

    if (rc == 0) {
        rc =
            mbedtls_sha256_update(
                &ctx,
                (const unsigned char *)pin,
                strlen(pin)
            );
    }

    if (rc == 0) {
        rc =
            mbedtls_sha256_finish(
                &ctx,
                hash
            );
    }

    mbedtls_sha256_free(&ctx);

    return
        rc == 0
            ? ESP_OK
            : ESP_FAIL;
}

static bool constant_time_equal(
    const uint8_t *a,
    const uint8_t *b,
    size_t len)
{
    if (a == NULL ||
        b == NULL) {
        return false;
    }

    uint8_t diff = 0;

    for (size_t i = 0; i < len; ++i) {
        diff |=
            (uint8_t)(a[i] ^ b[i]);
    }

    return diff == 0;
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

bool typen_settings_admin_pin_enabled(void)
{
    nvs_handle_t handle = 0;

    esp_err_t err =
        open_namespace(
            NVS_READONLY,
            &handle
        );

    if (err != ESP_OK) {
        return false;
    }

    size_t salt_len = 0;
    size_t hash_len = 0;

    const esp_err_t salt_err =
        nvs_get_blob(
            handle,
            TYPEN_NVS_ADMIN_SALT,
            NULL,
            &salt_len
        );

    const esp_err_t hash_err =
        nvs_get_blob(
            handle,
            TYPEN_NVS_ADMIN_HASH,
            NULL,
            &hash_len
        );

    nvs_close(handle);

    return
        salt_err == ESP_OK &&
        hash_err == ESP_OK &&
        salt_len == TYPEN_ADMIN_SALT_BYTES &&
        hash_len == TYPEN_ADMIN_HASH_BYTES;
}

esp_err_t typen_settings_set_admin_pin(
    const char *pin)
{
    if (!valid_admin_pin(pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t salt[TYPEN_ADMIN_SALT_BYTES];
    uint8_t hash[TYPEN_ADMIN_HASH_BYTES];

    esp_fill_random(
        salt,
        sizeof(salt)
    );

    esp_err_t err =
        hash_admin_pin(
            salt,
            pin,
            hash
        );

    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle = 0;

    err =
        open_namespace(
            NVS_READWRITE,
            &handle
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        nvs_set_blob(
            handle,
            TYPEN_NVS_ADMIN_SALT,
            salt,
            sizeof(salt)
        );

    if (err == ESP_OK) {
        err =
            nvs_set_blob(
                handle,
                TYPEN_NVS_ADMIN_HASH,
                hash,
                sizeof(hash)
            );
    }

    if (err == ESP_OK) {
        err =
            nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Admin PIN protection enabled"
        );
    }

    return err;
}

esp_err_t typen_settings_verify_admin_pin(
    const char *pin,
    bool *matches)
{
    if (matches == NULL ||
        pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *matches = false;

    if (!valid_admin_pin(pin)) {
        return ESP_OK;
    }

    nvs_handle_t handle = 0;

    esp_err_t err =
        open_namespace(
            NVS_READONLY,
            &handle
        );

    if (err != ESP_OK) {
        return err;
    }

    uint8_t salt[TYPEN_ADMIN_SALT_BYTES];
    uint8_t stored_hash[TYPEN_ADMIN_HASH_BYTES];
    size_t salt_len = sizeof(salt);
    size_t hash_len = sizeof(stored_hash);

    err =
        nvs_get_blob(
            handle,
            TYPEN_NVS_ADMIN_SALT,
            salt,
            &salt_len
        );

    if (err == ESP_OK) {
        err =
            nvs_get_blob(
                handle,
                TYPEN_NVS_ADMIN_HASH,
                stored_hash,
                &hash_len
            );
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        return err;
    }

    if (salt_len != sizeof(salt) ||
        hash_len != sizeof(stored_hash)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t candidate[TYPEN_ADMIN_HASH_BYTES];

    err =
        hash_admin_pin(
            salt,
            pin,
            candidate
        );

    if (err != ESP_OK) {
        return err;
    }

    *matches =
        constant_time_equal(
            candidate,
            stored_hash,
            sizeof(candidate)
        );

    return ESP_OK;
}

esp_err_t typen_settings_clear_admin_pin(void)
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

    esp_err_t salt_err =
        nvs_erase_key(
            handle,
            TYPEN_NVS_ADMIN_SALT
        );

    if (salt_err == ESP_ERR_NVS_NOT_FOUND) {
        salt_err = ESP_OK;
    }

    esp_err_t hash_err =
        nvs_erase_key(
            handle,
            TYPEN_NVS_ADMIN_HASH
        );

    if (hash_err == ESP_ERR_NVS_NOT_FOUND) {
        hash_err = ESP_OK;
    }

    if (salt_err == ESP_OK &&
        hash_err == ESP_OK) {
        err =
            nvs_commit(handle);
    } else {
        err =
            salt_err != ESP_OK
                ? salt_err
                : hash_err;
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Admin PIN protection disabled"
        );
    }

    return err;
}

esp_err_t typen_settings_factory_reset(void)
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
        nvs_erase_all(handle);

    if (err == ESP_OK) {
        err =
            nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGW(
            TAG,
            "TypeN settings factory reset"
        );
    }

    return err;
}

