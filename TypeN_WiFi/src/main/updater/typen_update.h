#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TYPEN_UPDATE_PATH_MAX         512
#define TYPEN_UPDATE_MESSAGE_MAX       96
#define TYPEN_UPDATE_LABEL_MAX         17
#define TYPEN_UPDATE_VERSION_MAX       32
#define TYPEN_UPDATE_PROJECT_MAX       32

#define TYPEN_UPDATE_DEFAULT_SD_PATH \
    "/sdcard/typen/update/TypeN.bin"

typedef enum {
    TYPEN_UPDATE_IDLE = 0,
    TYPEN_UPDATE_PREPARING,
    TYPEN_UPDATE_WRITING,
    TYPEN_UPDATE_VERIFYING,
    TYPEN_UPDATE_READY_REBOOT,
    TYPEN_UPDATE_ERROR,
} typen_update_state_t;

typedef enum {
    TYPEN_UPDATE_RESULT_NONE = 0,
    TYPEN_UPDATE_RESULT_PENDING,
    TYPEN_UPDATE_RESULT_CONFIRMED,
    TYPEN_UPDATE_RESULT_ROLLED_BACK,
} typen_update_result_t;

typedef struct {
    typen_update_state_t state;
    typen_update_result_t last_result;

    size_t total_bytes;
    size_t written_bytes;

    bool reboot_scheduled;
    bool rollback_possible;

    char running_partition[TYPEN_UPDATE_LABEL_MAX];
    char boot_partition[TYPEN_UPDATE_LABEL_MAX];
    char target_partition[TYPEN_UPDATE_LABEL_MAX];

    char running_version[TYPEN_UPDATE_VERSION_MAX];
    char running_project[TYPEN_UPDATE_PROJECT_MAX];
    char target_version[TYPEN_UPDATE_VERSION_MAX];

    char image_state[24];

    char last_version[TYPEN_UPDATE_VERSION_MAX];
    char last_partition[TYPEN_UPDATE_LABEL_MAX];

    char invalid_version[TYPEN_UPDATE_VERSION_MAX];
    char invalid_partition[TYPEN_UPDATE_LABEL_MAX];

    char message[TYPEN_UPDATE_MESSAGE_MAX];
} typen_update_status_t;


typedef struct {
    size_t image_size;
    char version[TYPEN_UPDATE_VERSION_MAX];
    char project_name[TYPEN_UPDATE_PROJECT_MAX];
    char running_partition[TYPEN_UPDATE_LABEL_MAX];
    char target_partition[TYPEN_UPDATE_LABEL_MAX];
    char message[TYPEN_UPDATE_MESSAGE_MAX];
} typen_update_sd_info_t;

/*
 * Browser and SD preflight expose the same application-image metadata.
 */
typedef typen_update_sd_info_t typen_update_image_info_t;

/*
 * Synchronously validate an SD-card application image before starting an
 * update. This verifies:
 * - path is inside /sdcard
 * - file exists and is non-empty
 * - image header magic is valid
 * - application descriptor is present/valid
 * - image fits the inactive OTA slot
 *
 * Full image verification is still performed by esp_ota_end() after writing.
 */
/*
 * Validate the beginning of an ESP application image before any OTA erase or
 * write begins. `data` must include the image header, first segment header,
 * and esp_app_desc_t. `image_size` is the complete file/request size.
 *
 * This rejects non-application images (bootloader, partition table, merged
 * flash images), project mismatches, truncated images, and images that do not
 * fit the inactive OTA slot.
 */
esp_err_t typen_update_check_image_prefix(
    const void *data,
    size_t data_size,
    size_t image_size,
    typen_update_image_info_t *info);

esp_err_t typen_update_check_sd(
    const char *path,
    typen_update_sd_info_t *info);

/*
 * Initialize updater state. Safe to call on every boot.
 */
esp_err_t typen_update_init(void);

/*
 * Rollback checkpoint.
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y, a newly installed OTA image
 * enters ESP_OTA_IMG_PENDING_VERIFY. TypeN calls this only after the proven
 * display/touch/settings/playback/UI baseline has initialized successfully.
 */
esp_err_t typen_update_confirm_running(void);

/*
 * Streaming update path used by HTTP upload.
 */
esp_err_t typen_update_upload_begin(size_t image_size);

/*
 * Preferred browser-upload path after typen_update_check_image_prefix().
 * Carries the already-validated version/target metadata into updater status.
 */
esp_err_t typen_update_upload_begin_checked(
    const typen_update_image_info_t *info);

esp_err_t typen_update_upload_write(
    const void *data,
    size_t length);
esp_err_t typen_update_upload_finish(void);
void typen_update_upload_abort(void);

/*
 * SD update path. This starts a background task and returns immediately.
 * The file must be inside /sdcard.
 */
esp_err_t typen_update_start_sd(
    const char *path);

/*
 * Snapshot current updater state.
 */
void typen_update_get_status(
    typen_update_status_t *status);

const char *typen_update_state_name(
    typen_update_state_t state);

const char *typen_update_result_name(
    typen_update_result_t result);

#ifdef __cplusplus
}
#endif
