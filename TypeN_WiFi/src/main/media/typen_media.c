#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "typen_storage.h"
#include "typen_settings.h"
#include "typen_media.h"

#define MAX_MEDIA_ITEMS       512
#define MAX_SCAN_DEPTH         16

static const char *TAG = "typen_media";

static typen_media_item_t *s_items = NULL;
static size_t s_count = 0;
static SemaphoreHandle_t s_lock = NULL;

/*
 * Web thumbnails are generated as PNG files on the SD card, but they are
 * implementation/cache data and must never appear in the user media catalog.
 * The configured thumbnail directory is resolved once at the start of each
 * scan and skipped recursively.
 */
static char s_scan_exclude_dir[TYPEN_SETTINGS_PATH_MAX] = {0};

static bool path_is_scan_excluded(
    const char *path)
{
    return path != NULL &&
           s_scan_exclude_dir[0] != '\0' &&
           strcmp(path, s_scan_exclude_dir) == 0;
}

static void update_scan_exclude_dir(void)
{
    s_scan_exclude_dir[0] = '\0';

    if (typen_settings_get_thumbnail_path(
            s_scan_exclude_dir,
            sizeof(s_scan_exclude_dir)) != ESP_OK) {
        snprintf(
            s_scan_exclude_dir,
            sizeof(s_scan_exclude_dir),
            "%s",
            TYPEN_DEFAULT_THUMBNAIL_PATH
        );
    }

    /*
     * Normalize a trailing slash so exact directory comparisons work whether
     * the setting was entered as /sdcard/typen/thumbs or .../thumbs/.
     */
    size_t len =
        strlen(s_scan_exclude_dir);

    while (len > 8U &&
           s_scan_exclude_dir[len - 1U] == '/') {
        s_scan_exclude_dir[len - 1U] = '\0';
        --len;
    }

    /*
     * Never allow a bad setting to hide the entire SD card.
     */
    if (strcmp(s_scan_exclude_dir, TYPEN_SD_ROOT) == 0) {
        snprintf(
            s_scan_exclude_dir,
            sizeof(s_scan_exclude_dir),
            "%s",
            TYPEN_DEFAULT_THUMBNAIL_PATH
        );
    }
}

static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);

        if (ca != cb) {
            return ca - cb;
        }
    }

    return (unsigned char)*a - (unsigned char)*b;
}

static int item_cmp(const void *a, const void *b)
{
    const typen_media_item_t *ia =
        (const typen_media_item_t *)a;
    const typen_media_item_t *ib =
        (const typen_media_item_t *)b;

    int by_name = ci_cmp(ia->name, ib->name);

    if (by_name != 0) {
        return by_name;
    }

    return ci_cmp(ia->path, ib->path);
}

static typen_media_type_t detect_type(const char *name)
{
    const char *ext = strrchr(name, '.');

    if (ext == NULL) {
        return TYPEN_MEDIA_UNKNOWN;
    }

    if (!strcasecmp(ext, ".jpg") ||
        !strcasecmp(ext, ".jpeg")) {
        return TYPEN_MEDIA_JPEG;
    }

    if (!strcasecmp(ext, ".gif")) {
        return TYPEN_MEDIA_GIF;
    }

    /*
     * PNG is catalogued now so the browser is already ready for the PNG
     * playback pass. Its thumbnail currently uses a clean format placeholder.
     */
    if (!strcasecmp(ext, ".png")) {
        return TYPEN_MEDIA_PNG;
    }

    if (!strcasecmp(ext, ".bmp")) {
        return TYPEN_MEDIA_BMP;
    }

    if (!strcasecmp(ext, ".webp")) {
        return TYPEN_MEDIA_WEBP;
    }

    return TYPEN_MEDIA_UNKNOWN;
}

static bool join_path(
    char *out,
    size_t out_cap,
    const char *dir,
    const char *name)
{
    if (out == NULL ||
        out_cap == 0 ||
        dir == NULL ||
        name == NULL) {
        return false;
    }

    const size_t dir_len = strlen(dir);
    const size_t name_len = strlen(name);
    const bool needs_slash =
        (dir_len > 0 && dir[dir_len - 1] != '/');

    const size_t total =
        dir_len +
        (needs_slash ? 1U : 0U) +
        name_len +
        1U;

    if (total > out_cap) {
        return false;
    }

    memcpy(out, dir, dir_len);

    size_t pos = dir_len;

    if (needs_slash) {
        out[pos++] = '/';
    }

    memcpy(out + pos, name, name_len + 1U);

    return true;
}

static esp_err_t add_file(
    const char *full_path,
    const char *base_name,
    typen_media_type_t type,
    size_t size)
{
    if (s_count >= MAX_MEDIA_ITEMS) {
        return ESP_ERR_NO_MEM;
    }

    const size_t name_len = strlen(base_name);
    const size_t path_len = strlen(full_path);

    if (name_len >= sizeof(s_items[0].name) ||
        path_len >= sizeof(s_items[0].path)) {
        ESP_LOGW(
            TAG,
            "Skipping media with overlong path: %s",
            full_path
        );
        return ESP_OK;
    }

    typen_media_item_t *item = &s_items[s_count];

    memset(item, 0, sizeof(*item));

    memcpy(item->name, base_name, name_len + 1U);
    memcpy(item->path, full_path, path_len + 1U);

    item->type = type;
    item->size = size;

    ++s_count;

    return ESP_OK;
}

static esp_err_t scan_dir(
    const char *dir_path,
    unsigned depth)
{
    if (depth > MAX_SCAN_DEPTH) {
        ESP_LOGW(TAG, "Maximum scan depth reached at %s", dir_path);
        return ESP_OK;
    }

    DIR *dir = opendir(dir_path);

    if (dir == NULL) {
        ESP_LOGW(TAG, "Unable to open directory: %s", dir_path);
        return ESP_OK;
    }

    esp_err_t result = ESP_OK;
    struct dirent *entry = NULL;

    while ((entry = readdir(dir)) != NULL) {
        if (s_count >= MAX_MEDIA_ITEMS) {
            ESP_LOGW(
                TAG,
                "Media catalog reached %u items; stopping scan",
                (unsigned)MAX_MEDIA_ITEMS
            );
            result = ESP_OK;
            break;
        }

        if (entry->d_name[0] == '.') {
            continue;
        }

        char full_path[sizeof(s_items[0].path)];

        if (!join_path(
                full_path,
                sizeof(full_path),
                dir_path,
                entry->d_name)) {
            ESP_LOGW(TAG, "Skipping overlong path below %s", dir_path);
            continue;
        }

        struct stat st;

        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (path_is_scan_excluded(full_path)) {
                ESP_LOGD(
                    TAG,
                    "Skipping thumbnail cache directory: %s",
                    full_path
                );
                continue;
            }

            esp_err_t err =
                scan_dir(
                    full_path,
                    depth + 1U
                );

            if (err != ESP_OK) {
                result = err;
                break;
            }

            continue;
        }

        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        typen_media_type_t type =
            detect_type(entry->d_name);

        if (type == TYPEN_MEDIA_UNKNOWN) {
            continue;
        }

        esp_err_t err = add_file(
            full_path,
            entry->d_name,
            type,
            (size_t)st.st_size
        );

        if (err == ESP_ERR_NO_MEM) {
            ESP_LOGW(TAG, "Media catalog full");
            result = ESP_OK;
            break;
        }

        if (err != ESP_OK) {
            result = err;
            break;
        }
    }

    closedir(dir);
    return result;
}

esp_err_t typen_media_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();

        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_items == NULL) {
        s_items = (typen_media_item_t *)heap_caps_calloc(
            MAX_MEDIA_ITEMS,
            sizeof(typen_media_item_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

        if (s_items == NULL) {
            ESP_LOGE(TAG, "Unable to allocate media catalog in PSRAM");
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

esp_err_t typen_media_scan(void)
{
    ESP_RETURN_ON_ERROR(
        typen_media_init(),
        TAG,
        "media init failed"
    );

    xSemaphoreTake(s_lock, portMAX_DELAY);

    s_count = 0;
    memset(
        s_items,
        0,
        MAX_MEDIA_ITEMS * sizeof(s_items[0])
    );

    update_scan_exclude_dir();

    ESP_LOGI(
        TAG,
        "Recursively scanning SD card: %s (excluding %s)",
        TYPEN_SD_ROOT,
        s_scan_exclude_dir
    );

    esp_err_t err =
        scan_dir(
            TYPEN_SD_ROOT,
            0
        );

    if (err == ESP_OK && s_count > 1) {
        qsort(
            s_items,
            s_count,
            sizeof(s_items[0]),
            item_cmp
        );
    }

    const size_t count = s_count;

    xSemaphoreGive(s_lock);

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Media catalog: %u items across SD card",
            (unsigned)count
        );
    }

    return err;
}

size_t typen_media_count(void)
{
    if (s_lock == NULL) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = s_count;
    xSemaphoreGive(s_lock);

    return count;
}

esp_err_t typen_media_get(
    size_t index,
    typen_media_item_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (index >= s_count) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    *out = s_items[index];

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t typen_media_find(
    const char *name_or_path,
    typen_media_item_t *out)
{
    if (name_or_path == NULL ||
        out == NULL ||
        s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_items[i].path, name_or_path) == 0) {
            *out = s_items[i];
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }

    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_items[i].name, name_or_path) == 0) {
            *out = s_items[i];
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t typen_media_delete(
    const char *path)
{
    if (path == NULL ||
        path[0] == '\0' ||
        s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Do not call typen_media_scan() here.
     *
     * Local deletes run inside the 4 KiB typen_touch_ui task. The recursive
     * scanner carries path buffers at each recursion level, which can overflow
     * that task stack. Delete the file and update the live catalog in place.
     */
    bool catalogued = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_items[i].path, path) == 0) {
            catalogued = true;
            break;
        }
    }

    xSemaphoreGive(s_lock);

    if (!catalogued) {
        return ESP_ERR_NOT_FOUND;
    }

    errno = 0;

    if (remove(path) != 0) {
        const int delete_errno = errno;

        ESP_LOGE(
            TAG,
            "Delete failed: %s errno=%d",
            path,
            delete_errno
        );

        return ESP_FAIL;
    }

    /*
     * Remove the deleted path from the current sorted catalog. If another
     * task rescanned between remove() and this lock, the entry may already
     * be absent; the filesystem delete still succeeded.
     */
    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_items[i].path, path) == 0) {
            if (i + 1U < s_count) {
                memmove(
                    &s_items[i],
                    &s_items[i + 1U],
                    (s_count - i - 1U) *
                        sizeof(s_items[0])
                );
            }

            --s_count;

            memset(
                &s_items[s_count],
                0,
                sizeof(s_items[0])
            );

            break;
        }
    }

    xSemaphoreGive(s_lock);

    ESP_LOGI(
        TAG,
        "Deleted media: %s",
        path
    );

    return ESP_OK;
}

const char *typen_media_type_name(
    typen_media_type_t type)
{
    switch (type) {
        case TYPEN_MEDIA_JPEG:
            return "JPG";

        case TYPEN_MEDIA_GIF:
            return "GIF";

        case TYPEN_MEDIA_PNG:
            return "PNG";

        case TYPEN_MEDIA_BMP:
            return "BMP";

        case TYPEN_MEDIA_WEBP:
            return "WEBP";

        default:
            return "UNKNOWN";
    }
}
