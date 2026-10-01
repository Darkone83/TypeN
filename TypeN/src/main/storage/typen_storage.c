#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"
#include "bsp/esp32_p4_platform.h"

#include "typen_storage.h"

static const char *TAG = "typen_storage";
static bool s_ready = false;

static esp_err_t ensure_dir(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "mkdir(%s) failed, errno=%d", path, errno);
    return ESP_FAIL;
}

esp_err_t typen_storage_start(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /*
     * Exact known-good TypeN SD hardware path.
     * Let the Waveshare BSP own SDMMC slot/LDO setup.
     */
    ESP_LOGI(TAG, "Mounting SD using known-good Waveshare BSP path");

    esp_err_t err = bsp_sdcard_mount();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_sdcard_mount failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_RETURN_ON_ERROR(
        ensure_dir(TYPEN_DATA_DIR),
        TAG,
        "data dir failed"
    );

    ESP_RETURN_ON_ERROR(
        ensure_dir(TYPEN_MEDIA_DIR),
        TAG,
        "media dir failed"
    );

    s_ready = true;

    ESP_LOGI(TAG, "SD ready: %s", TYPEN_MEDIA_DIR);

    return ESP_OK;
}

bool typen_storage_ready(void)
{
    return s_ready;
}