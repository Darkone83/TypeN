#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs.h"

#include "typen_update.h"
#include "typen_playback.h"

#define UPDATE_IO_BUFFER_SIZE       4096
#define UPDATE_TASK_STACK_BYTES     6144
#define UPDATE_TASK_PRIORITY           4
#define UPDATE_REBOOT_DELAY_MS      2500

#define UPDATE_NVS_NAMESPACE        "typen"
#define UPDATE_NVS_KEY_STATE        "upd_state"
#define UPDATE_NVS_KEY_VERSION      "upd_ver"
#define UPDATE_NVS_KEY_SLOT         "upd_slot"

static const char *TAG = "typen_update";

static SemaphoreHandle_t s_lock = NULL;

static bool s_busy = false;
static bool s_writer_open = false;
static bool s_reboot_scheduled = false;

static esp_ota_handle_t s_ota_handle = 0;
static const esp_partition_t *s_target = NULL;

static typen_update_status_t s_status = {
    .state = TYPEN_UPDATE_IDLE,
    .message = "Updater ready",
};

static bool safe_sd_path(
    const char *path)
{
    if (!path ||
        !*path) {
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

static void copy_label(
    char *dst,
    size_t dst_size,
    const char *src)
{
    if (!dst ||
        dst_size == 0) {
        return;
    }

    snprintf(
        dst,
        dst_size,
        "%s",
        src ? src : ""
    );
}


static void copy_fixed_text(
    char *dst,
    size_t dst_size,
    const char *src,
    size_t src_size)
{
    if (!dst ||
        dst_size == 0) {
        return;
    }

    dst[0] = '\0';

    if (!src ||
        src_size == 0) {
        return;
    }

    size_t length = 0;

    while (length < src_size &&
           src[length] != '\0') {
        ++length;
    }

    if (length >= dst_size) {
        length = dst_size - 1U;
    }

    if (length > 0) {
        memcpy(
            dst,
            src,
            length
        );
    }

    dst[length] = '\0';
}


typedef struct {
    char version[TYPEN_UPDATE_VERSION_MAX];
    char variant[TYPEN_UPDATE_VARIANT_MAX];
    uint16_t firmware_abi;
} typen_firmware_identity_t;

static const char *variant_name_from_token(
    const char *token)
{
    if (!token) {
        return NULL;
    }

    if (strcmp(token, "uart") == 0) {
        return "P4-UART";
    }

    if (strcmp(token, "sdio") == 0) {
        return "P4-SDIO";
    }

    return NULL;
}

static bool release_version_is_valid(
    const char *version)
{
    if (!version ||
        !*version) {
        return false;
    }

    const char *p = version;

    for (int field = 0;
         field < 3;
         ++field) {
        if (*p < '0' ||
            *p > '9') {
            return false;
        }

        while (*p >= '0' &&
               *p <= '9') {
            ++p;
        }

        if (field < 2) {
            if (*p != '.') {
                return false;
            }

            ++p;
        }
    }

    if (*p == '\0') {
        return true;
    }

    /*
     * Optional SemVer prerelease suffix such as -beta.1 or -rc.2.
     * TypeN reserves '+' build metadata for hardware/ABI identity.
     */
    if (*p != '-') {
        return false;
    }

    ++p;

    if (*p == '\0') {
        return false;
    }

    while (*p) {
        const char ch = *p++;

        const bool allowed =
            (ch >= '0' && ch <= '9') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            ch == '-' ||
            ch == '.';

        if (!allowed) {
            return false;
        }
    }

    return true;
}

static bool parse_firmware_identity(
    const char *raw_version,
    size_t raw_version_size,
    typen_firmware_identity_t *identity)
{
    if (!identity) {
        return false;
    }

    memset(
        identity,
        0,
        sizeof(*identity)
    );

    char text[TYPEN_UPDATE_VERSION_MAX];

    copy_fixed_text(
        text,
        sizeof(text),
        raw_version,
        raw_version_size
    );

    if (text[0] == '\0') {
        return false;
    }

    char *plus =
        strchr(
            text,
            '+'
        );

    if (!plus ||
        plus == text ||
        plus[1] == '\0') {
        return false;
    }

    *plus = '\0';

    if (!release_version_is_valid(
            text)) {
        return false;
    }

    copy_fixed_text(
        identity->version,
        sizeof(identity->version),
        text,
        strlen(text)
    );

    char *metadata =
        plus + 1;

    char *dot =
        strchr(
            metadata,
            '.'
        );

    if (!dot ||
        dot == metadata ||
        dot[1] == '\0') {
        return false;
    }

    *dot = '\0';

    const char *variant_name =
        variant_name_from_token(
            metadata
        );

    if (!variant_name) {
        return false;
    }

    copy_fixed_text(
        identity->variant,
        sizeof(identity->variant),
        variant_name,
        strlen(variant_name)
    );

    const char *abi_text =
        dot + 1;

    if (strncmp(
            abi_text,
            "abi",
            3) != 0) {
        return false;
    }

    abi_text += 3;

    if (*abi_text < '0' ||
        *abi_text > '9') {
        return false;
    }

    char *end = NULL;

    unsigned long abi =
        strtoul(
            abi_text,
            &end,
            10
        );

    if (end == abi_text ||
        *end != '\0' ||
        abi == 0 ||
        abi > UINT16_MAX) {
        return false;
    }

    identity->firmware_abi =
        (uint16_t)abi;

    return true;
}


static const char *ota_image_state_name(
    esp_ota_img_states_t state)
{
    switch (state) {
        case ESP_OTA_IMG_NEW:
            return "new";

        case ESP_OTA_IMG_PENDING_VERIFY:
            return "pending verify";

        case ESP_OTA_IMG_VALID:
            return "valid";

        case ESP_OTA_IMG_INVALID:
            return "invalid";

        case ESP_OTA_IMG_ABORTED:
            return "aborted";

        case ESP_OTA_IMG_UNDEFINED:
        default:
            return "undefined";
    }
}

static esp_err_t save_update_record(
    typen_update_result_t result,
    const char *version,
    const char *partition)
{
    nvs_handle_t handle;

    esp_err_t err =
        nvs_open(
            UPDATE_NVS_NAMESPACE,
            NVS_READWRITE,
            &handle
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unable to open update-result NVS: %s",
            esp_err_to_name(err)
        );
        return err;
    }

    err =
        nvs_set_u8(
            handle,
            UPDATE_NVS_KEY_STATE,
            (uint8_t)result
        );

    if (err == ESP_OK) {
        err =
            nvs_set_str(
                handle,
                UPDATE_NVS_KEY_VERSION,
                version ? version : ""
            );
    }

    if (err == ESP_OK) {
        err =
            nvs_set_str(
                handle,
                UPDATE_NVS_KEY_SLOT,
                partition ? partition : ""
            );
    }

    if (err == ESP_OK) {
        err =
            nvs_commit(
                handle
            );
    }

    nvs_close(
        handle
    );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unable to persist update result: %s",
            esp_err_to_name(err)
        );
    }

    return err;
}

static void load_update_record(
    typen_update_status_t *status)
{
    if (!status) {
        return;
    }

    status->last_result =
        TYPEN_UPDATE_RESULT_NONE;

    status->last_version[0] =
        '\0';

    status->last_partition[0] =
        '\0';

    nvs_handle_t handle;

    esp_err_t err =
        nvs_open(
            UPDATE_NVS_NAMESPACE,
            NVS_READONLY,
            &handle
        );

    if (err != ESP_OK) {
        return;
    }

    uint8_t raw_state = 0;

    if (nvs_get_u8(
            handle,
            UPDATE_NVS_KEY_STATE,
            &raw_state) == ESP_OK &&
        raw_state <=
            (uint8_t)TYPEN_UPDATE_RESULT_ROLLED_BACK) {
        status->last_result =
            (typen_update_result_t)raw_state;
    }

    size_t length =
        sizeof(status->last_version);

    if (nvs_get_str(
            handle,
            UPDATE_NVS_KEY_VERSION,
            status->last_version,
            &length) != ESP_OK) {
        status->last_version[0] =
            '\0';
    }

    length =
        sizeof(status->last_partition);

    if (nvs_get_str(
            handle,
            UPDATE_NVS_KEY_SLOT,
            status->last_partition,
            &length) != ESP_OK) {
        status->last_partition[0] =
            '\0';
    }

    nvs_close(
        handle
    );
}

static void collect_runtime_status(
    typen_update_status_t *status)
{
    if (!status) {
        return;
    }

    const esp_partition_t *running =
        esp_ota_get_running_partition();

    const esp_partition_t *boot =
        esp_ota_get_boot_partition();

    copy_label(
        status->running_partition,
        sizeof(status->running_partition),
        running ? running->label : "unknown"
    );

    copy_label(
        status->boot_partition,
        sizeof(status->boot_partition),
        boot ? boot->label : "unknown"
    );

    const esp_app_desc_t *running_desc =
        esp_app_get_description();

    if (running_desc) {
        typen_firmware_identity_t identity;

        if (parse_firmware_identity(
                running_desc->version,
                sizeof(running_desc->version),
                &identity)) {
            copy_fixed_text(
                status->running_version,
                sizeof(status->running_version),
                identity.version,
                strlen(identity.version)
            );

            copy_fixed_text(
                status->running_variant,
                sizeof(status->running_variant),
                identity.variant,
                strlen(identity.variant)
            );

            status->running_firmware_abi =
                identity.firmware_abi;
        } else {
            /*
             * Keep raw ESP-IDF version visible for diagnostics if an older
             * pre-versioning build is ever encountered.
             */
            copy_fixed_text(
                status->running_version,
                sizeof(status->running_version),
                running_desc->version,
                sizeof(running_desc->version)
            );
        }

        copy_fixed_text(
            status->running_project,
            sizeof(status->running_project),
            running_desc->project_name,
            sizeof(running_desc->project_name)
        );
    }

    status->rollback_possible =
        esp_ota_check_rollback_is_possible();

    if (running) {
        esp_ota_img_states_t image_state =
            ESP_OTA_IMG_UNDEFINED;

        esp_err_t state_err =
            esp_ota_get_state_partition(
                running,
                &image_state
            );

        if (state_err == ESP_OK) {
            copy_fixed_text(
                status->image_state,
                sizeof(status->image_state),
                ota_image_state_name(image_state),
                strlen(ota_image_state_name(image_state))
            );
        } else if (running->subtype ==
                   ESP_PARTITION_SUBTYPE_APP_FACTORY) {
            copy_fixed_text(
                status->image_state,
                sizeof(status->image_state),
                "factory",
                strlen("factory")
            );
        } else {
            copy_fixed_text(
                status->image_state,
                sizeof(status->image_state),
                "unknown",
                strlen("unknown")
            );
        }
    }

    status->invalid_partition[0] =
        '\0';

    status->invalid_version[0] =
        '\0';

    const esp_partition_t *invalid =
        esp_ota_get_last_invalid_partition();

    if (invalid) {
        copy_label(
            status->invalid_partition,
            sizeof(status->invalid_partition),
            invalid->label
        );

        esp_app_desc_t invalid_desc;

        if (esp_ota_get_partition_description(
                invalid,
                &invalid_desc) == ESP_OK) {
            copy_fixed_text(
                status->invalid_version,
                sizeof(status->invalid_version),
                invalid_desc.version,
                sizeof(invalid_desc.version)
            );
        }
    }
}

static void set_info_message(
    typen_update_sd_info_t *info,
    const char *message)
{
    if (!info) {
        return;
    }

    copy_fixed_text(
        info->message,
        sizeof(info->message),
        message ? message : "",
        message ? strlen(message) : 0U
    );
}

static esp_err_t validate_candidate_identity(
    const esp_app_desc_t *candidate,
    typen_update_sd_info_t *info,
    const char *source_name)
{
    if (!candidate ||
        !info) {
        return ESP_ERR_INVALID_ARG;
    }

    copy_fixed_text(
        info->project_name,
        sizeof(info->project_name),
        candidate->project_name,
        sizeof(candidate->project_name)
    );

    typen_firmware_identity_t candidate_identity;

    if (!parse_firmware_identity(
            candidate->version,
            sizeof(candidate->version),
            &candidate_identity)) {
        set_info_message(
            info,
            "Firmware is missing a valid embedded TypeN identity"
        );

        ESP_LOGW(
            TAG,
            "%s OTA preflight: invalid embedded firmware identity",
            source_name ? source_name : "Firmware"
        );

        return ESP_ERR_INVALID_VERSION;
    }

    copy_fixed_text(
        info->version,
        sizeof(info->version),
        candidate_identity.version,
        strlen(candidate_identity.version)
    );

    copy_fixed_text(
        info->variant,
        sizeof(info->variant),
        candidate_identity.variant,
        strlen(candidate_identity.variant)
    );

    info->firmware_abi =
        candidate_identity.firmware_abi;

    const esp_app_desc_t *running_desc =
        esp_app_get_description();

    if (!running_desc) {
        set_info_message(
            info,
            "Unable to read the running TypeN firmware identity"
        );

        return ESP_ERR_INVALID_STATE;
    }

    char running_project[
        TYPEN_UPDATE_PROJECT_MAX
    ] = {0};

    copy_fixed_text(
        running_project,
        sizeof(running_project),
        running_desc->project_name,
        sizeof(running_desc->project_name)
    );

    if (running_project[0] == '\0' ||
        info->project_name[0] == '\0' ||
        strcmp(
            running_project,
            info->project_name) != 0) {
        set_info_message(
            info,
            "Firmware project does not match the running TypeN application"
        );

        ESP_LOGW(
            TAG,
            "%s OTA preflight: project mismatch current=%s new=%s",
            source_name ? source_name : "Firmware",
            running_project[0]
                ? running_project
                : "unknown",
            info->project_name[0]
                ? info->project_name
                : "unknown"
        );

        return ESP_ERR_INVALID_ARG;
    }

    typen_firmware_identity_t running_identity;

    if (!parse_firmware_identity(
            running_desc->version,
            sizeof(running_desc->version),
            &running_identity)) {
        set_info_message(
            info,
            "Running firmware does not contain a valid TypeN identity"
        );

        ESP_LOGE(
            TAG,
            "%s OTA preflight: running firmware identity is invalid",
            source_name ? source_name : "Firmware"
        );

        return ESP_ERR_INVALID_STATE;
    }

    if (strcmp(
            running_identity.variant,
            candidate_identity.variant) != 0) {
        char message[
            TYPEN_UPDATE_MESSAGE_MAX
        ];

        snprintf(
            message,
            sizeof(message),
            "Wrong hardware variant: unit=%s image=%s",
            running_identity.variant,
            candidate_identity.variant
        );

        set_info_message(
            info,
            message
        );

        ESP_LOGW(
            TAG,
            "%s OTA preflight: variant mismatch current=%s new=%s",
            source_name ? source_name : "Firmware",
            running_identity.variant,
            candidate_identity.variant
        );

        return ESP_ERR_INVALID_ARG;
    }

    if (running_identity.firmware_abi !=
        candidate_identity.firmware_abi) {
        char message[
            TYPEN_UPDATE_MESSAGE_MAX
        ];

        snprintf(
            message,
            sizeof(message),
            "Firmware ABI mismatch: unit=%u image=%u",
            (unsigned)running_identity.firmware_abi,
            (unsigned)candidate_identity.firmware_abi
        );

        set_info_message(
            info,
            message
        );

        ESP_LOGW(
            TAG,
            "%s OTA preflight: ABI mismatch current=%u new=%u",
            source_name ? source_name : "Firmware",
            (unsigned)running_identity.firmware_abi,
            (unsigned)candidate_identity.firmware_abi
        );

        return ESP_ERR_INVALID_VERSION;
    }

    return ESP_OK;
}


static void set_message_locked(
    typen_update_state_t state,
    const char *message)
{
    s_status.state = state;

    snprintf(
        s_status.message,
        sizeof(s_status.message),
        "%s",
        message ? message : ""
    );
}

static void update_running_partition_locked(void)
{
    const esp_partition_t *running =
        esp_ota_get_running_partition();

    copy_label(
        s_status.running_partition,
        sizeof(s_status.running_partition),
        running ? running->label : "unknown"
    );
}

static void set_error_and_release(
    const char *message)
{
    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    set_message_locked(
        TYPEN_UPDATE_ERROR,
        message
    );

    s_busy = false;
    s_writer_open = false;
    s_target = NULL;
    s_ota_handle = 0;

    xSemaphoreGive(
        s_lock
    );

    typen_playback_pause(
        false
    );
}

static esp_err_t reserve_operation(
    typen_update_state_t state,
    const char *message)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    if (s_busy ||
        s_reboot_scheduled) {
        xSemaphoreGive(
            s_lock
        );
        return ESP_ERR_INVALID_STATE;
    }

    s_busy = true;
    s_status.total_bytes = 0;
    s_status.written_bytes = 0;
    s_status.target_partition[0] = '\0';
    s_status.target_version[0] = '\0';
    s_status.target_variant[0] = '\0';
    s_status.target_firmware_abi = 0;

    set_message_locked(
        state,
        message
    );

    xSemaphoreGive(
        s_lock
    );

    /*
     * Firmware update has priority over media playback. Pausing here prevents
     * GIF/PPA/SD activity from competing with OTA flash writes. On failure the
     * error path resumes playback; on success the device reboots.
     */
    typen_playback_pause(
        true
    );

    return ESP_OK;
}

static esp_err_t writer_begin_reserved(
    size_t image_size)
{
    if (image_size == 0) {
        set_error_and_release(
            "Firmware image is empty"
        );
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_partition_t *target =
        esp_ota_get_next_update_partition(
            NULL
        );

    if (!target) {
        set_error_and_release(
            "No OTA partition available"
        );
        return ESP_ERR_NOT_FOUND;
    }

    if (image_size > target->size) {
        set_error_and_release(
            "Firmware image is larger than OTA slot"
        );
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    s_target = target;
    s_status.total_bytes = image_size;
    s_status.written_bytes = 0;

    copy_label(
        s_status.target_partition,
        sizeof(s_status.target_partition),
        target->label
    );

    set_message_locked(
        TYPEN_UPDATE_PREPARING,
        "Erasing update slot"
    );

    xSemaphoreGive(
        s_lock
    );

    esp_err_t err =
        esp_ota_begin(
            target,
            image_size,
            &s_ota_handle
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_ota_begin failed: %s",
            esp_err_to_name(err)
        );

        set_error_and_release(
            "Unable to prepare OTA slot"
        );

        return err;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    s_writer_open = true;

    set_message_locked(
        TYPEN_UPDATE_WRITING,
        "Writing firmware to inactive OTA slot"
    );

    xSemaphoreGive(
        s_lock
    );

    ESP_LOGI(
        TAG,
        "OTA target=%s size=%u",
        target->label,
        (unsigned)image_size
    );

    return ESP_OK;
}

static esp_err_t writer_write(
    const void *data,
    size_t length)
{
    if (!data ||
        length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    const bool ready =
        s_busy &&
        s_writer_open &&
        s_status.state ==
            TYPEN_UPDATE_WRITING;

    const size_t written =
        s_status.written_bytes;

    const size_t total =
        s_status.total_bytes;

    xSemaphoreGive(
        s_lock
    );

    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (written + length > total) {
        typen_update_upload_abort();
        set_error_and_release(
            "Firmware stream exceeded declared size"
        );
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err =
        esp_ota_write(
            s_ota_handle,
            data,
            length
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_ota_write failed: %s",
            esp_err_to_name(err)
        );

        typen_update_upload_abort();
        set_error_and_release(
            "Firmware write failed"
        );

        return err;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    s_status.written_bytes +=
        length;

    xSemaphoreGive(
        s_lock
    );

    return ESP_OK;
}

static void reboot_task(
    void *arg)
{
    (void)arg;

    vTaskDelay(
        pdMS_TO_TICKS(
            UPDATE_REBOOT_DELAY_MS
        )
    );

    ESP_LOGI(
        TAG,
        "Rebooting into updated firmware"
    );

    esp_restart();
}

static void schedule_reboot(void)
{
    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    if (s_reboot_scheduled) {
        xSemaphoreGive(
            s_lock
        );
        return;
    }

    s_reboot_scheduled = true;
    s_status.reboot_scheduled = true;

    xSemaphoreGive(
        s_lock
    );

    BaseType_t ok =
        xTaskCreate(
            reboot_task,
            "typen_update_reboot",
            3072,
            NULL,
            UPDATE_TASK_PRIORITY,
            NULL
        );

    if (ok != pdPASS) {
        xSemaphoreTake(
            s_lock,
            portMAX_DELAY
        );

        s_reboot_scheduled = false;
        s_status.reboot_scheduled = false;

        snprintf(
            s_status.message,
            sizeof(s_status.message),
            "%s",
            "Update installed; manual reset required"
        );

        xSemaphoreGive(
            s_lock
        );

        ESP_LOGE(
            TAG,
            "Unable to create reboot task"
        );
    }
}

static esp_err_t writer_finish(void)
{
    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    const bool ready =
        s_busy &&
        s_writer_open;

    const size_t total =
        s_status.total_bytes;

    const size_t written =
        s_status.written_bytes;

    const esp_partition_t *target =
        s_target;

    set_message_locked(
        TYPEN_UPDATE_VERIFYING,
        "Verifying firmware"
    );

    xSemaphoreGive(
        s_lock
    );

    if (!ready ||
        !target) {
        return ESP_ERR_INVALID_STATE;
    }

    if (written != total) {
        typen_update_upload_abort();

        set_error_and_release(
            "Firmware upload was incomplete"
        );

        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err =
        esp_ota_end(
            s_ota_handle
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_ota_end failed: %s",
            esp_err_to_name(err)
        );

        xSemaphoreTake(
            s_lock,
            portMAX_DELAY
        );

        s_writer_open = false;
        s_ota_handle = 0;

        xSemaphoreGive(
            s_lock
        );

        set_error_and_release(
            "Firmware image validation failed"
        );

        return err;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    s_writer_open = false;
    s_ota_handle = 0;

    xSemaphoreGive(
        s_lock
    );

    esp_app_desc_t installed_desc;
    memset(
        &installed_desc,
        0,
        sizeof(installed_desc)
    );

    const bool have_installed_desc =
        esp_ota_get_partition_description(
            target,
            &installed_desc
        ) == ESP_OK;

    typen_update_sd_info_t installed_info;
    memset(
        &installed_info,
        0,
        sizeof(installed_info)
    );

    if (!have_installed_desc) {
        set_error_and_release(
            "Unable to read installed firmware identity"
        );

        return ESP_ERR_INVALID_VERSION;
    }

    err =
        validate_candidate_identity(
            &installed_desc,
            &installed_info,
            "Installed"
        );

    if (err != ESP_OK) {
        set_error_and_release(
            installed_info.message[0]
                ? installed_info.message
                : "Installed firmware identity is invalid"
        );

        return err;
    }

    const char *installed_version =
        installed_info.version;

    err =
        esp_ota_set_boot_partition(
            target
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_ota_set_boot_partition failed: %s",
            esp_err_to_name(err)
        );

        set_error_and_release(
            "Unable to select updated firmware"
        );

        return err;
    }

    (void)save_update_record(
        TYPEN_UPDATE_RESULT_PENDING,
        installed_version,
        target->label
    );

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    s_status.last_result =
        TYPEN_UPDATE_RESULT_PENDING;

    copy_fixed_text(
        s_status.last_version,
        sizeof(s_status.last_version),
        installed_version,
        strlen(installed_version)
    );

    copy_label(
        s_status.last_partition,
        sizeof(s_status.last_partition),
        target->label
    );

    copy_fixed_text(
        s_status.target_version,
        sizeof(s_status.target_version),
        installed_version,
        strlen(installed_version)
    );

    copy_fixed_text(
        s_status.target_variant,
        sizeof(s_status.target_variant),
        installed_info.variant,
        strlen(installed_info.variant)
    );

    s_status.target_firmware_abi =
        installed_info.firmware_abi;

    if (installed_version[0] != '\0') {
        snprintf(
            s_status.message,
            sizeof(s_status.message),
            "Firmware %s verified; rebooting",
            installed_version
        );

        s_status.state =
            TYPEN_UPDATE_READY_REBOOT;
    } else {
        set_message_locked(
            TYPEN_UPDATE_READY_REBOOT,
            "Update verified; rebooting"
        );
    }

    s_status.written_bytes =
        s_status.total_bytes;

    /*
     * Keep the operation reserved until reboot so another update cannot start
     * after the boot partition has already been changed.
     */
    s_busy = true;

    xSemaphoreGive(
        s_lock
    );

    ESP_LOGI(
        TAG,
        "OTA image verified; boot partition -> %s",
        target->label
    );

    schedule_reboot();
    return ESP_OK;
}

static void sd_update_task(
    void *arg)
{
    char *path =
        (char *)arg;

    FILE *file = NULL;
    uint8_t *buffer = NULL;

    struct stat st = {0};

    if (stat(path, &st) != 0 ||
        !S_ISREG(st.st_mode) ||
        st.st_size <= 0) {
        ESP_LOGE(
            TAG,
            "SD OTA file disappeared before install: %s errno=%d",
            path,
            errno
        );

        set_error_and_release(
            "Firmware file disappeared from SD card"
        );
        goto done;
    }

    file =
        fopen(
            path,
            "rb"
        );

    if (!file) {
        ESP_LOGE(
            TAG,
            "SD OTA fopen failed: %s errno=%d",
            path,
            errno
        );

        set_error_and_release(
            "Unable to open firmware file from SD card"
        );
        goto done;
    }

    esp_err_t err =
        writer_begin_reserved(
            (size_t)st.st_size
        );

    if (err != ESP_OK) {
        goto done;
    }

    buffer =
        (uint8_t *)malloc(
            UPDATE_IO_BUFFER_SIZE
        );

    if (!buffer) {
        typen_update_upload_abort();

        set_error_and_release(
            "Update buffer allocation failed"
        );
        goto done;
    }

    for (;;) {
        size_t got =
            fread(
                buffer,
                1,
                UPDATE_IO_BUFFER_SIZE,
                file
            );

        if (got > 0) {
            err =
                writer_write(
                    buffer,
                    got
                );

            if (err != ESP_OK) {
                goto done;
            }
        }

        if (got < UPDATE_IO_BUFFER_SIZE) {
            if (ferror(file)) {
                typen_update_upload_abort();

                set_error_and_release(
                    "SD firmware read failed"
                );

                goto done;
            }

            break;
        }

        taskYIELD();
    }

    err =
        writer_finish();

    if (err != ESP_OK) {
        goto done;
    }

done:
    if (buffer) {
        free(buffer);
    }

    if (file) {
        fclose(file);
    }

    free(path);
    vTaskDelete(NULL);
}

esp_err_t typen_update_init(void)
{
    if (!s_lock) {
        s_lock =
            xSemaphoreCreateMutex();

        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    typen_update_status_t initial;
    memset(
        &initial,
        0,
        sizeof(initial)
    );

    initial.state =
        TYPEN_UPDATE_IDLE;

    snprintf(
        initial.message,
        sizeof(initial.message),
        "%s",
        "Updater ready"
    );

    load_update_record(
        &initial
    );

    collect_runtime_status(
        &initial
    );

    /*
     * A pending record means the previous firmware install selected a new boot
     * slot but had not yet reached TypeN's confirmation checkpoint.
     *
     * If we are now running a different slot, the bootloader returned to the
     * prior working image. Record that as a rollback so the web UI can tell
     * the user what happened after reconnecting.
     */
    if (initial.last_result ==
            TYPEN_UPDATE_RESULT_PENDING &&
        initial.last_partition[0] != '\0' &&
        initial.running_partition[0] != '\0' &&
        strcmp(
            initial.last_partition,
            initial.running_partition) != 0) {
        initial.last_result =
            TYPEN_UPDATE_RESULT_ROLLED_BACK;

        snprintf(
            initial.message,
            sizeof(initial.message),
            "%s",
            "Previous firmware update rolled back"
        );

        (void)save_update_record(
            TYPEN_UPDATE_RESULT_ROLLED_BACK,
            initial.last_version,
            initial.last_partition
        );

        ESP_LOGW(
            TAG,
            "OTA rollback detected: attempted=%s version=%s running=%s version=%s",
            initial.last_partition,
            initial.last_version[0]
                ? initial.last_version
                : "unknown",
            initial.running_partition,
            initial.running_version[0]
                ? initial.running_version
                : "unknown"
        );
    } else if (initial.last_result ==
                   TYPEN_UPDATE_RESULT_CONFIRMED) {
        snprintf(
            initial.message,
            sizeof(initial.message),
            "%s",
            "Last firmware update confirmed"
        );
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    s_status =
        initial;

    s_busy = false;
    s_writer_open = false;
    s_reboot_scheduled = false;
    s_target = NULL;
    s_ota_handle = 0;

    xSemaphoreGive(
        s_lock
    );

    ESP_LOGI(
        TAG,
        "Updater ready; running=%s version=%s variant=%s ABI=%u state=%s boot=%s rollback=%s OTA slots=%u",
        initial.running_partition,
        initial.running_version[0]
            ? initial.running_version
            : "unknown",
        initial.running_variant[0]
            ? initial.running_variant
            : "unknown",
        (unsigned)initial.running_firmware_abi,
        initial.image_state[0]
            ? initial.image_state
            : "unknown",
        initial.boot_partition,
        initial.rollback_possible
            ? "available"
            : "not available",
        (unsigned)esp_ota_get_app_partition_count()
    );

    return ESP_OK;
}

esp_err_t typen_update_confirm_running(void)
{
    const esp_partition_t *running =
        esp_ota_get_running_partition();

    if (!running) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_img_states_t state =
        ESP_OTA_IMG_UNDEFINED;

    esp_err_t err =
        esp_ota_get_state_partition(
            running,
            &state
        );

    if (err != ESP_OK) {
        /*
         * Factory applications do not participate in OTA image-state
         * confirmation. Runtime status still reports them as "factory".
         */
        return ESP_OK;
    }

    if (state !=
        ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "New OTA image reached TypeN core-ready checkpoint; confirming"
    );

    err =
        esp_ota_mark_app_valid_cancel_rollback();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Unable to confirm OTA image: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    const esp_app_desc_t *description =
        esp_app_get_description();

    char version[
        TYPEN_UPDATE_VERSION_MAX
    ] = {0};

    if (description) {
        typen_firmware_identity_t identity;

        if (parse_firmware_identity(
                description->version,
                sizeof(description->version),
                &identity)) {
            copy_fixed_text(
                version,
                sizeof(version),
                identity.version,
                strlen(identity.version)
            );
        } else {
            copy_fixed_text(
                version,
                sizeof(version),
                description->version,
                sizeof(description->version)
            );
        }
    }

    (void)save_update_record(
        TYPEN_UPDATE_RESULT_CONFIRMED,
        version,
        running->label
    );

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    s_status.last_result =
        TYPEN_UPDATE_RESULT_CONFIRMED;

    copy_fixed_text(
        s_status.last_version,
        sizeof(s_status.last_version),
        version,
        strlen(version)
    );

    copy_label(
        s_status.last_partition,
        sizeof(s_status.last_partition),
        running->label
    );

    s_status.state =
        TYPEN_UPDATE_IDLE;

    snprintf(
        s_status.message,
        sizeof(s_status.message),
        "%s",
        "Firmware update confirmed"
    );

    xSemaphoreGive(
        s_lock
    );

    ESP_LOGI(
        TAG,
        "OTA image confirmed valid: slot=%s version=%s",
        running->label,
        version[0]
            ? version
            : "unknown"
    );

    return ESP_OK;
}

esp_err_t typen_update_check_image_prefix(
    const void *data,
    size_t data_size,
    size_t image_size,
    typen_update_image_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(
        info,
        0,
        sizeof(*info)
    );

    const esp_partition_t *running =
        esp_ota_get_running_partition();

    copy_label(
        info->running_partition,
        sizeof(info->running_partition),
        running ? running->label : "unknown"
    );

    const size_t minimum_image_size =
        sizeof(esp_image_header_t) +
        sizeof(esp_image_segment_header_t) +
        sizeof(esp_app_desc_t);

    if (!data ||
        image_size < minimum_image_size ||
        data_size < minimum_image_size) {
        set_info_message(
            info,
            "Firmware image is too small to be a TypeN application"
        );

        return ESP_ERR_INVALID_SIZE;
    }

    const esp_partition_t *target =
        esp_ota_get_next_update_partition(
            NULL
        );

    if (!target) {
        set_info_message(
            info,
            "No inactive OTA partition is available"
        );

        return ESP_ERR_NOT_FOUND;
    }

    copy_label(
        info->target_partition,
        sizeof(info->target_partition),
        target->label
    );

    info->image_size =
        image_size;

    if (image_size >
        target->size) {
        set_info_message(
            info,
            "Firmware image is larger than the OTA slot"
        );

        return ESP_ERR_INVALID_SIZE;
    }

    esp_image_header_t image_header;

    memcpy(
        &image_header,
        data,
        sizeof(image_header)
    );

    if (image_header.magic !=
        ESP_IMAGE_HEADER_MAGIC) {
        set_info_message(
            info,
            "Selected file is not an ESP application image"
        );

        ESP_LOGW(
            TAG,
            "Browser OTA preflight: invalid image magic 0x%02x",
            (unsigned)image_header.magic
        );

        return ESP_ERR_INVALID_ARG;
    }

    const size_t app_desc_offset =
        sizeof(esp_image_header_t) +
        sizeof(esp_image_segment_header_t);

    esp_app_desc_t app_desc;

    memcpy(
        &app_desc,
        (const uint8_t *)data +
            app_desc_offset,
        sizeof(app_desc)
    );

    if (app_desc.magic_word !=
        ESP_APP_DESC_MAGIC_WORD) {
        set_info_message(
            info,
            "File is not a TypeN application image"
        );

        ESP_LOGW(
            TAG,
            "Browser OTA preflight: application descriptor missing or invalid"
        );

        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t identity_err =
        validate_candidate_identity(
            &app_desc,
            info,
            "Browser"
        );

    if (identity_err != ESP_OK) {
        return identity_err;
    }

    char ready_message[
        TYPEN_UPDATE_MESSAGE_MAX
    ];

    snprintf(
        ready_message,
        sizeof(ready_message),
        "TypeN %s %s ABI %u is ready to install",
        info->version,
        info->variant,
        (unsigned)info->firmware_abi
    );

    set_info_message(
        info,
        ready_message
    );

    ESP_LOGI(
        TAG,
        "Browser OTA preflight OK: project=%s version=%s variant=%s ABI=%u size=%u target=%s",
        info->project_name[0]
            ? info->project_name
            : "unknown",
        info->version[0]
            ? info->version
            : "unknown",
        info->variant[0]
            ? info->variant
            : "unknown",
        (unsigned)info->firmware_abi,
        (unsigned)info->image_size,
        info->target_partition
    );

    return ESP_OK;
}

esp_err_t typen_update_upload_begin(
    size_t image_size)
{
    esp_err_t err =
        reserve_operation(
            TYPEN_UPDATE_PREPARING,
            "Preparing browser update"
        );

    if (err != ESP_OK) {
        return err;
    }

    return writer_begin_reserved(
        image_size
    );
}

esp_err_t typen_update_upload_begin_checked(
    const typen_update_image_info_t *info)
{
    if (!info ||
        info->image_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        reserve_operation(
            TYPEN_UPDATE_PREPARING,
            "Browser firmware validated; preparing update"
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        writer_begin_reserved(
            info->image_size
        );

    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    copy_fixed_text(
        s_status.target_version,
        sizeof(s_status.target_version),
        info->version,
        strlen(info->version)
    );

    copy_fixed_text(
        s_status.target_variant,
        sizeof(s_status.target_variant),
        info->variant,
        strlen(info->variant)
    );

    s_status.target_firmware_abi =
        info->firmware_abi;

    if (info->target_partition[0] != '\0') {
        copy_label(
            s_status.target_partition,
            sizeof(s_status.target_partition),
            info->target_partition
        );
    }

    snprintf(
        s_status.message,
        sizeof(s_status.message),
        "Writing %s to %s",
        info->version[0]
            ? info->version
            : "firmware",
        s_status.target_partition[0]
            ? s_status.target_partition
            : "inactive OTA slot"
    );

    xSemaphoreGive(
        s_lock
    );

    return ESP_OK;
}

esp_err_t typen_update_upload_write(
    const void *data,
    size_t length)
{
    return writer_write(
        data,
        length
    );
}

esp_err_t typen_update_upload_finish(void)
{
    return writer_finish();
}

void typen_update_upload_abort(void)
{
    bool should_abort = false;
    esp_ota_handle_t handle = 0;

    if (!s_lock) {
        return;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    if (s_writer_open) {
        should_abort = true;
        handle = s_ota_handle;
    }

    s_writer_open = false;
    s_ota_handle = 0;
    s_target = NULL;
    s_busy = false;

    set_message_locked(
        TYPEN_UPDATE_ERROR,
        "Update aborted"
    );

    xSemaphoreGive(
        s_lock
    );

    if (should_abort) {
        esp_ota_abort(
            handle
        );
    }

    typen_playback_pause(
        false
    );
}


esp_err_t typen_update_check_sd(
    const char *path,
    typen_update_sd_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(
        info,
        0,
        sizeof(*info)
    );

    const esp_partition_t *running =
        esp_ota_get_running_partition();

    copy_label(
        info->running_partition,
        sizeof(info->running_partition),
        running ? running->label : "unknown"
    );

    if (!safe_sd_path(path)) {
        set_info_message(
            info,
            "Firmware path must be inside /sdcard"
        );

        return ESP_ERR_INVALID_ARG;
    }

    struct stat st = {0};

    if (stat(path, &st) != 0 ||
        !S_ISREG(st.st_mode)) {
        set_info_message(
            info,
            "Firmware file not found on SD card"
        );

        ESP_LOGW(
            TAG,
            "SD OTA preflight: file not found: %s errno=%d",
            path,
            errno
        );

        return ESP_ERR_NOT_FOUND;
    }

    const size_t minimum_image_size =
        sizeof(esp_image_header_t) +
        sizeof(esp_image_segment_header_t) +
        sizeof(esp_app_desc_t);

    if (st.st_size <= 0 ||
        (size_t)st.st_size <
            minimum_image_size) {
        set_info_message(
            info,
            "Firmware file is too small to be a TypeN application"
        );

        return ESP_ERR_INVALID_SIZE;
    }

    const esp_partition_t *target =
        esp_ota_get_next_update_partition(
            NULL
        );

    if (!target) {
        set_info_message(
            info,
            "No inactive OTA partition is available"
        );

        return ESP_ERR_NOT_FOUND;
    }

    copy_label(
        info->target_partition,
        sizeof(info->target_partition),
        target->label
    );

    info->image_size =
        (size_t)st.st_size;

    if (info->image_size >
        target->size) {
        set_info_message(
            info,
            "Firmware image is larger than the OTA slot"
        );

        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file =
        fopen(
            path,
            "rb"
        );

    if (!file) {
        set_info_message(
            info,
            "Unable to open firmware file from SD card"
        );

        ESP_LOGW(
            TAG,
            "SD OTA preflight: fopen failed: %s errno=%d",
            path,
            errno
        );

        return ESP_FAIL;
    }

    esp_image_header_t image_header;

    const size_t header_read =
        fread(
            &image_header,
            1,
            sizeof(image_header),
            file
        );

    if (header_read !=
        sizeof(image_header)) {
        fclose(file);

        set_info_message(
            info,
            "Unable to read firmware image header"
        );

        return ESP_FAIL;
    }

    if (image_header.magic !=
        ESP_IMAGE_HEADER_MAGIC) {
        fclose(file);

        set_info_message(
            info,
            "Selected file is not an ESP application image"
        );

        ESP_LOGW(
            TAG,
            "SD OTA preflight: invalid image magic 0x%02x",
            (unsigned)image_header.magic
        );

        return ESP_ERR_INVALID_ARG;
    }

    const long app_desc_offset =
        (long)(
            sizeof(esp_image_header_t) +
            sizeof(esp_image_segment_header_t)
        );

    if (fseek(
            file,
            app_desc_offset,
            SEEK_SET) != 0) {
        fclose(file);

        set_info_message(
            info,
            "Unable to locate application descriptor"
        );

        return ESP_FAIL;
    }

    esp_app_desc_t app_desc;

    const size_t desc_read =
        fread(
            &app_desc,
            1,
            sizeof(app_desc),
            file
        );

    fclose(file);

    if (desc_read !=
        sizeof(app_desc)) {
        set_info_message(
            info,
            "Unable to read application descriptor"
        );

        return ESP_FAIL;
    }

    if (app_desc.magic_word !=
        ESP_APP_DESC_MAGIC_WORD) {
        set_info_message(
            info,
            "Firmware application descriptor is invalid"
        );

        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t identity_err =
        validate_candidate_identity(
            &app_desc,
            info,
            "SD"
        );

    if (identity_err != ESP_OK) {
        return identity_err;
    }

    char ready_message[
        TYPEN_UPDATE_MESSAGE_MAX
    ];

    snprintf(
        ready_message,
        sizeof(ready_message),
        "TypeN %s %s ABI %u is ready to install",
        info->version,
        info->variant,
        (unsigned)info->firmware_abi
    );

    set_info_message(
        info,
        ready_message
    );

    ESP_LOGI(
        TAG,
        "SD OTA preflight OK: path=%s project=%s version=%s variant=%s ABI=%u size=%u target=%s",
        path,
        info->project_name[0]
            ? info->project_name
            : "unknown",
        info->version[0]
            ? info->version
            : "unknown",
        info->variant[0]
            ? info->variant
            : "unknown",
        (unsigned)info->firmware_abi,
        (unsigned)info->image_size,
        info->target_partition
    );

    return ESP_OK;
}

esp_err_t typen_update_start_sd(
    const char *path)
{
    typen_update_sd_info_t info;

    esp_err_t err =
        typen_update_check_sd(
            path,
            &info
        );

    if (err != ESP_OK) {
        if (s_lock) {
            set_error_and_release(
                info.message[0]
                    ? info.message
                    : "SD firmware preflight failed"
            );
        }

        return err;
    }

    err =
        reserve_operation(
            TYPEN_UPDATE_PREPARING,
            "SD firmware validated; starting update"
        );

    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    s_status.total_bytes =
        info.image_size;

    copy_label(
        s_status.target_partition,
        sizeof(s_status.target_partition),
        info.target_partition
    );

    copy_fixed_text(
        s_status.target_version,
        sizeof(s_status.target_version),
        info.version,
        strlen(info.version)
    );

    copy_fixed_text(
        s_status.target_variant,
        sizeof(s_status.target_variant),
        info.variant,
        strlen(info.variant)
    );

    s_status.target_firmware_abi =
        info.firmware_abi;

    snprintf(
        s_status.message,
        sizeof(s_status.message),
        "Installing %s from SD",
        info.version[0]
            ? info.version
            : "firmware"
    );

    xSemaphoreGive(
        s_lock
    );

    const size_t path_length =
        strlen(path) + 1U;

    char *copy =
        (char *)malloc(
            path_length
        );

    if (!copy) {
        set_error_and_release(
            "Unable to allocate SD update path"
        );

        return ESP_ERR_NO_MEM;
    }

    memcpy(
        copy,
        path,
        path_length
    );

    BaseType_t ok =
        xTaskCreate(
            sd_update_task,
            "typen_update_sd",
            UPDATE_TASK_STACK_BYTES,
            copy,
            UPDATE_TASK_PRIORITY,
            NULL
        );

    if (ok != pdPASS) {
        free(copy);

        set_error_and_release(
            "Unable to create SD update task"
        );

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "SD OTA task started: %s",
        path
    );

    return ESP_OK;
}

void typen_update_get_status(
    typen_update_status_t *status)
{
    if (!status) {
        return;
    }

    if (!s_lock) {
        memset(
            status,
            0,
            sizeof(*status)
        );

        status->state =
            TYPEN_UPDATE_ERROR;

        snprintf(
            status->message,
            sizeof(status->message),
            "%s",
            "Updater not initialized"
        );

        return;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    *status =
        s_status;

    xSemaphoreGive(
        s_lock
    );

    collect_runtime_status(
        status
    );
}

const char *typen_update_state_name(
    typen_update_state_t state)
{
    switch (state) {
        case TYPEN_UPDATE_IDLE:
            return "idle";

        case TYPEN_UPDATE_PREPARING:
            return "preparing";

        case TYPEN_UPDATE_WRITING:
            return "writing";

        case TYPEN_UPDATE_VERIFYING:
            return "verifying";

        case TYPEN_UPDATE_READY_REBOOT:
            return "rebooting";

        case TYPEN_UPDATE_ERROR:
            return "error";

        default:
            return "unknown";
    }
}

const char *typen_update_result_name(
    typen_update_result_t result)
{
    switch (result) {
        case TYPEN_UPDATE_RESULT_PENDING:
            return "pending";

        case TYPEN_UPDATE_RESULT_CONFIRMED:
            return "confirmed";

        case TYPEN_UPDATE_RESULT_ROLLED_BACK:
            return "rolled_back";

        case TYPEN_UPDATE_RESULT_NONE:
        default:
            return "none";
    }
}
