#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#include "typen_board.h"
#include "typen_storage.h"
#include "typen_media.h"
#include "typen_playback.h"
#include "typen_settings.h"
#include "typen_ui.h"
#include "typen_update.h"
#include "typen_wifi.h"
#include "typen_web.h"

static const char *TAG = "TypeN";

static bool s_storage_ready = false;


static void log_firmware_identity(void)
{
    const esp_app_desc_t *app =
        esp_app_get_description();

    const esp_partition_t *running =
        esp_ota_get_running_partition();

    ESP_LOGI(
        TAG,
        "Firmware: project=%s version=%s build=%s %s slot=%s idf=%s",
        app && app->project_name[0]
            ? app->project_name
            : "TypeN",
        app && app->version[0]
            ? app->version
            : "unknown",
        app && app->date[0]
            ? app->date
            : "unknown",
        app && app->time[0]
            ? app->time
            : "unknown",
        running
            ? running->label
            : "unknown",
        esp_get_idf_version()
    );
}


static void web_select_cb(
    const char *name,
    void *ctx)
{
    (void)ctx;

    typen_media_item_t item;

    if (typen_media_find(
            name,
            &item) == ESP_OK) {
        typen_ui_show_media(
            &item
        );
    }
}

static void restore_saved_media(void)
{
    char path[512];

    esp_err_t err =
        typen_settings_get_selected_media(
            path,
            sizeof(path)
        );

    if (err != ESP_OK) {
        return;
    }

    typen_media_item_t item;

    err =
        typen_media_find(
            path,
            &item
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Saved media not present: %s",
            path
        );
        return;
    }

    err =
        typen_playback_set(
            &item
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Unable to restore saved media: %s",
            esp_err_to_name(err)
        );
        return;
    }

    ESP_LOGI(
        TAG,
        "Restored selected media: %s",
        path
    );
}

static void wifi_task(void *arg)
{
    (void)arg;

    typen_ui_set_wifi_status(
        "Starting"
    );

    esp_err_t wifi_err =
        typen_wifi_start();

    if (wifi_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SoftAP failed: %s",
            esp_err_to_name(wifi_err)
        );

        typen_ui_set_wifi_status(
            "Unavailable"
        );

        vTaskDelete(NULL);
        return;
    }

    typen_ui_set_wifi_status(
        typen_wifi_ip_string()
    );

    /*
     * Web management requires both Wi-Fi and the SD-backed media store.
     * Wi-Fi itself remains independent of storage.
     */
    if (s_storage_ready) {
        esp_err_t web_err =
            typen_web_start(
                web_select_cb,
                NULL
            );

        if (web_err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Web server failed: %s",
                esp_err_to_name(web_err)
            );

            typen_ui_set_status(
                "Web unavailable"
            );
        } else {
            typen_ui_set_status(
                "Ready"
            );
        }
    }

    ESP_LOGI(
        TAG,
        "Wi-Fi ready: %s",
        typen_wifi_ip_string()
    );

    vTaskDelete(NULL);
}

void app_main(void)
{
    log_firmware_identity();

    /*
     * Preserve the proven display/touch/playback baseline.
     */
    ESP_ERROR_CHECK(
        typen_board_display_early_start()
    );

    ESP_ERROR_CHECK(
        typen_board_ui_start()
    );

    ESP_ERROR_CHECK(
        typen_settings_init()
    );

    ESP_ERROR_CHECK(
        typen_playback_init()
    );

    ESP_ERROR_CHECK(
        typen_ui_init()
    );

    /*
     * OTA rollback checkpoint:
     *
     * A newly installed OTA image is confirmed only after the proven
     * display/touch/settings/playback/UI baseline has initialized. If the new
     * image crashes or resets before reaching this point, ESP-IDF rollback can
     * return to the previous working image.
     */
    esp_err_t update_err =
        typen_update_init();

    if (update_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Updater init failed: %s",
            esp_err_to_name(update_err)
        );
    } else {
        update_err =
            typen_update_confirm_running();

        if (update_err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "OTA confirmation failed: %s",
                esp_err_to_name(update_err)
            );
        }
    }

    /*
     * HARD BOOT ORDER:
     *
     * The microSD/media path is primary and must always be attempted before
     * ESP-Hosted/Wi-Fi.
     *
     * The external ESP32-C6 now uses UART, so it has no dependency on the
     * P4 SDMMC controller:
     *
     *   P4 GPIO18 TX -> C6 GPIO20 RX
     *   P4 GPIO19 RX <- C6 GPIO21 TX
     *   P4 GPIO54    -> C6 EN/RST
     *
     * A slow or failed C6 must never prevent SD/media startup.
     */
    typen_ui_set_status(
        "Mounting SD"
    );

    esp_err_t storage_err =
        typen_storage_start();

    if (storage_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SD mount failed: %s",
            esp_err_to_name(storage_err)
        );

        typen_ui_set_status(
            "SD unavailable"
        );
    } else {
        esp_err_t err =
            typen_media_init();

        if (err == ESP_OK) {
            err =
                typen_media_scan();
        }

        if (err == ESP_OK) {
            typen_ui_set_media_count(
                typen_media_count()
            );

            typen_ui_set_status(
                "Media ready"
            );

            restore_saved_media();

            s_storage_ready = true;
        } else {
            ESP_LOGE(
                TAG,
                "Media scan failed: %s",
                esp_err_to_name(err)
            );

            typen_ui_set_status(
                "Media scan failed"
            );

            storage_err = err;
        }
    }

    /*
     * SD/media has now completed its startup attempt.
     *
     * Start Hosted/Wi-Fi separately so its UART handshake/retry behavior can
     * never stall the TypeN UI or media path.
     */
    BaseType_t task_ok =
        xTaskCreate(
            wifi_task,
            "typen_wifi",
            6144,
            NULL,
            5,
            NULL
        );

    if (task_ok != pdPASS) {
        ESP_LOGE(
            TAG,
            "Unable to create Wi-Fi task"
        );

        typen_ui_set_wifi_status(
            "Unavailable"
        );
    }

    ESP_LOGI(
        TAG,
        "Startup complete: storage=%s wifi=starting first_frame=%u ms",
        s_storage_ready
            ? "ready"
            : "unavailable",
        (unsigned)typen_board_first_frame_ms()
    );
}
