#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "typen_gif.h"
#include "typen_jpeg.h"
#include "typen_media.h"
#include "typen_png.h"
#include "typen_thumb_cache.h"

#define THUMB_CACHE_SLOTS       12
#define THUMB_QUEUE_DEPTH       12
#define THUMB_WORKER_STACK      (24 * 1024)
#define THUMB_WORKER_PRIORITY   3
#define THUMB_BG                0x0000

typedef enum {
    SLOT_EMPTY = 0,
    SLOT_QUEUED,
    SLOT_LOADING,
    SLOT_READY,
    SLOT_FAILED,
} slot_state_t;

typedef struct {
    size_t media_index;
    slot_state_t state;
    uint32_t age;
    uint32_t generation;
    uint16_t *pixels;
} thumb_slot_t;

static const char *TAG = "typen_thumb_cache";

static thumb_slot_t s_slots[THUMB_CACHE_SLOTS];
static QueueHandle_t s_queue = NULL;
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_worker = NULL;

static uint32_t s_age = 1;
static uint32_t s_generation = 1;
static bool s_dirty = false;
static bool s_paused = true;

static void clear_thumb(uint16_t *pixels)
{
    if (!pixels) {
        return;
    }

    memset(
        pixels,
        0,
        TYPEN_THUMB_W *
        TYPEN_THUMB_H *
        sizeof(uint16_t)
    );
}

static void jpeg_thumb_oriented_dimensions(
    const typen_jpeg_frame_t *frame,
    int *width,
    int *height)
{
    const uint8_t orientation =
        (frame->orientation >= 1U &&
         frame->orientation <= 8U)
            ? frame->orientation
            : 1U;

    if (orientation >= 5U) {
        *width =
            frame->height;

        *height =
            frame->width;
    } else {
        *width =
            frame->width;

        *height =
            frame->height;
    }
}

static void jpeg_thumb_map_to_source(
    const typen_jpeg_frame_t *frame,
    int oriented_x,
    int oriented_y,
    int *source_x,
    int *source_y)
{
    const int sw =
        frame->width;

    const int sh =
        frame->height;

    const uint8_t orientation =
        (frame->orientation >= 1U &&
         frame->orientation <= 8U)
            ? frame->orientation
            : 1U;

    int sx = oriented_x;
    int sy = oriented_y;

    switch (orientation) {
        case 2:
            sx = sw - 1 - oriented_x;
            sy = oriented_y;
            break;

        case 3:
            sx = sw - 1 - oriented_x;
            sy = sh - 1 - oriented_y;
            break;

        case 4:
            sx = oriented_x;
            sy = sh - 1 - oriented_y;
            break;

        case 5:
            sx = oriented_y;
            sy = oriented_x;
            break;

        case 6:
            sx = oriented_y;
            sy = sh - 1 - oriented_x;
            break;

        case 7:
            sx = sw - 1 - oriented_y;
            sy = sh - 1 - oriented_x;
            break;

        case 8:
            sx = sw - 1 - oriented_y;
            sy = oriented_x;
            break;

        case 1:
        default:
            break;
    }

    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= sw) sx = sw - 1;
    if (sy >= sh) sy = sh - 1;

    *source_x = sx;
    *source_y = sy;
}

static void draw_jpeg_thumb(
    const typen_media_item_t *item,
    uint16_t *dst,
    esp_err_t *result)
{
    typen_jpeg_frame_t frame = {0};

    *result =
        typen_jpeg_decode_file(
            item->path,
            &frame
        );

    if (*result != ESP_OK) {
        return;
    }

    int oriented_w = 0;
    int oriented_h = 0;

    jpeg_thumb_oriented_dimensions(
        &frame,
        &oriented_w,
        &oriented_h
    );

    int crop_x = 0;
    int crop_y = 0;
    int crop_w = oriented_w;
    int crop_h = oriented_h;

    /*
     * Thumbnail cards use CENTER + COVER just like playback, but against the
     * 196x88 preview aspect ratio.
     */
    if ((int64_t)oriented_w *
            TYPEN_THUMB_H >
        (int64_t)oriented_h *
            TYPEN_THUMB_W) {
        crop_w =
            (int)(((int64_t)oriented_h *
                   TYPEN_THUMB_W) /
                  TYPEN_THUMB_H);

        if (crop_w < 1) crop_w = 1;

        crop_x =
            (oriented_w - crop_w) / 2;
    } else if ((int64_t)oriented_h *
                   TYPEN_THUMB_W >
               (int64_t)oriented_w *
                   TYPEN_THUMB_H) {
        crop_h =
            (int)(((int64_t)oriented_w *
                   TYPEN_THUMB_H) /
                  TYPEN_THUMB_W);

        if (crop_h < 1) crop_h = 1;

        crop_y =
            (oriented_h - crop_h) / 2;
    }

    const uint16_t *src =
        (const uint16_t *)
        frame.pixels;

    for (int dy = 0;
         dy < TYPEN_THUMB_H;
         ++dy) {
        const int oriented_y =
            crop_y +
            (int)(((int64_t)dy *
                   crop_h) /
                  TYPEN_THUMB_H);

        uint16_t *row =
            dst +
            (size_t)dy *
            TYPEN_THUMB_W;

        for (int dx = 0;
             dx < TYPEN_THUMB_W;
             ++dx) {
            const int oriented_x =
                crop_x +
                (int)(((int64_t)dx *
                       crop_w) /
                      TYPEN_THUMB_W);

            int sx = 0;
            int sy = 0;

            jpeg_thumb_map_to_source(
                &frame,
                oriented_x,
                oriented_y,
                &sx,
                &sy
            );

            row[dx] =
                src[
                    (size_t)sy *
                    frame.stride_pixels +
                    sx
                ];
        }
    }

    typen_jpeg_free(
        &frame
    );

    *result = ESP_OK;
}

esp_err_t typen_thumb_render_item(
    const typen_media_item_t *item,
    uint16_t *dst)
{
    if (!item || !dst) {
        return ESP_ERR_INVALID_ARG;
    }

    clear_thumb(dst);

    switch (item->type) {
        case TYPEN_MEDIA_JPEG: {
            esp_err_t err = ESP_FAIL;
            draw_jpeg_thumb(
                item,
                dst,
                &err
            );
            return err;
        }

        case TYPEN_MEDIA_PNG:
            return typen_png_draw_preview(
                item->path,
                dst,
                TYPEN_THUMB_W,
                TYPEN_THUMB_H,
                0,
                0,
                TYPEN_THUMB_W,
                TYPEN_THUMB_H,
                THUMB_BG
            );

        case TYPEN_MEDIA_GIF:
            return typen_gif_draw_preview(
                item->path,
                dst,
                TYPEN_THUMB_W,
                TYPEN_THUMB_H,
                0,
                0,
                TYPEN_THUMB_W,
                TYPEN_THUMB_H,
                THUMB_BG
            );

        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

static int find_slot_locked(size_t index)
{
    for (int i = 0; i < THUMB_CACHE_SLOTS; ++i) {
        if (s_slots[i].state != SLOT_EMPTY &&
            s_slots[i].generation ==
                s_generation &&
            s_slots[i].media_index == index) {
            return i;
        }
    }

    return -1;
}

static int choose_slot_locked(void)
{
    for (int i = 0; i < THUMB_CACHE_SLOTS; ++i) {
        if (s_slots[i].state == SLOT_EMPTY) {
            return i;
        }
    }

    int chosen = -1;
    uint32_t oldest = UINT32_MAX;

    for (int i = 0; i < THUMB_CACHE_SLOTS; ++i) {
        if (s_slots[i].state == SLOT_LOADING ||
            s_slots[i].state == SLOT_QUEUED) {
            continue;
        }

        if (s_slots[i].age < oldest) {
            oldest = s_slots[i].age;
            chosen = i;
        }
    }

    return chosen;
}

static bool cache_is_paused(void)
{
    bool paused = true;

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    paused = s_paused;

    xSemaphoreGive(
        s_lock
    );

    return paused;
}

static void worker_task(void *arg)
{
    (void)arg;

    for (;;) {
        int slot_index = -1;

        if (xQueueReceive(
                s_queue,
                &slot_index,
                portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (slot_index < 0 ||
            slot_index >= THUMB_CACHE_SLOTS) {
            continue;
        }

        while (cache_is_paused()) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }

        size_t media_index = 0;
        uint32_t job_generation = 0;
        uint16_t *pixels = NULL;

        xSemaphoreTake(
            s_lock,
            portMAX_DELAY
        );

        thumb_slot_t *slot =
            &s_slots[slot_index];

        if (slot->state != SLOT_QUEUED) {
            xSemaphoreGive(s_lock);
            continue;
        }

        slot->state = SLOT_LOADING;
        media_index = slot->media_index;
        job_generation =
            slot->generation;
        pixels = slot->pixels;

        xSemaphoreGive(s_lock);

        typen_media_item_t item;
        esp_err_t err =
            typen_media_get(
                media_index,
                &item
            );

        if (err == ESP_OK) {
            const size_t psram_before =
                heap_caps_get_free_size(
                    MALLOC_CAP_SPIRAM
                );

            const size_t internal_before =
                heap_caps_get_free_size(
                    MALLOC_CAP_INTERNAL
                );

            ESP_LOGI(
                TAG,
                "Decode [%u] %s | PSRAM=%uK internal=%uK",
                (unsigned)media_index,
                item.path,
                (unsigned)(psram_before / 1024U),
                (unsigned)(internal_before / 1024U)
            );

            err =
                typen_thumb_render_item(
                    &item,
                    pixels
                );
        }

        xSemaphoreTake(
            s_lock,
            portMAX_DELAY
        );

        slot =
            &s_slots[slot_index];

        /*
         * A media rescan can occur while decoding. Catalog indices are not
         * stable across a rescan, so never publish pixels produced for an old
         * generation under the new catalog's index.
         */
        if (job_generation !=
                s_generation ||
            slot->generation !=
                job_generation) {
            slot->state =
                SLOT_EMPTY;

            slot->media_index = 0;
            slot->age = 0;
            slot->generation = 0;
        } else {
            slot->state =
                (err == ESP_OK)
                    ? SLOT_READY
                    : SLOT_FAILED;

            slot->age =
                s_age++;
        }

        s_dirty = true;

        xSemaphoreGive(s_lock);

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Thumbnail [%u] failed: %s",
                (unsigned)media_index,
                esp_err_to_name(err)
            );
        }

        /*
         * Thumbnail generation is background work. Give the panel/SD/touch
         * paths room between files even when browsing a page of four images.
         */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t typen_thumb_cache_init(void)
{
    if (s_lock == NULL) {
        s_lock =
            xSemaphoreCreateMutex();

        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_queue == NULL) {
        s_queue =
            xQueueCreate(
                THUMB_QUEUE_DEPTH,
                sizeof(int)
            );

        if (!s_queue) {
            return ESP_ERR_NO_MEM;
        }
    }

    for (int i = 0; i < THUMB_CACHE_SLOTS; ++i) {
        if (s_slots[i].pixels != NULL) {
            continue;
        }

        s_slots[i].pixels =
            (uint16_t *)heap_caps_malloc(
                TYPEN_THUMB_W *
                TYPEN_THUMB_H *
                sizeof(uint16_t),
                MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
            );

        if (!s_slots[i].pixels) {
            ESP_LOGE(
                TAG,
                "PSRAM thumbnail slot allocation failed at %d",
                i
            );
            return ESP_ERR_NO_MEM;
        }

        s_slots[i].state = SLOT_EMPTY;
        clear_thumb(s_slots[i].pixels);
    }

    if (s_worker == NULL) {
        BaseType_t ok =
            xTaskCreate(
                worker_task,
                "typen_thumb",
                THUMB_WORKER_STACK,
                NULL,
                THUMB_WORKER_PRIORITY,
                &s_worker
            );

        if (ok != pdPASS) {
            s_worker = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(
        TAG,
        "PSRAM thumbnail cache ready: %u slots, %uK",
        (unsigned)THUMB_CACHE_SLOTS,
        (unsigned)(
            THUMB_CACHE_SLOTS *
            TYPEN_THUMB_W *
            TYPEN_THUMB_H *
            sizeof(uint16_t) /
            1024U
        )
    );

    return ESP_OK;
}

void typen_thumb_cache_reset(void)
{
    if (!s_lock) {
        return;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    ++s_generation;

    if (s_generation == 0U) {
        s_generation = 1U;
    }

    for (int i = 0; i < THUMB_CACHE_SLOTS; ++i) {
        /*
         * Do not invalidate a buffer currently owned by the worker.
         * Rescan/reset can mark all completed entries empty; an in-flight
         * entry will age out naturally on the next browse.
         */
        if (s_slots[i].state != SLOT_LOADING) {
            s_slots[i].state = SLOT_EMPTY;
            s_slots[i].media_index = 0;
            s_slots[i].age = 0;
            s_slots[i].generation = 0;
        }
    }

    s_dirty = true;

    xSemaphoreGive(s_lock);

    if (s_queue) {
        xQueueReset(s_queue);
    }
}

typen_thumb_state_t typen_thumb_cache_get(
    size_t media_index,
    const uint16_t **pixels)
{
    if (pixels) {
        *pixels = NULL;
    }

    if (!s_lock ||
        !s_queue) {
        return TYPEN_THUMB_FAILED;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    int slot_index =
        find_slot_locked(
            media_index
        );

    if (slot_index >= 0) {
        thumb_slot_t *slot =
            &s_slots[slot_index];

        slot->age = s_age++;

        typen_thumb_state_t state =
            TYPEN_THUMB_LOADING;

        if (slot->state == SLOT_READY) {
            state = TYPEN_THUMB_READY;

            if (pixels) {
                *pixels = slot->pixels;
            }
        } else if (slot->state == SLOT_FAILED) {
            state = TYPEN_THUMB_FAILED;
        }

        xSemaphoreGive(s_lock);
        return state;
    }

    slot_index =
        choose_slot_locked();

    if (slot_index < 0) {
        xSemaphoreGive(s_lock);
        return TYPEN_THUMB_LOADING;
    }

    thumb_slot_t *slot =
        &s_slots[slot_index];

    slot->media_index =
        media_index;

    slot->generation =
        s_generation;

    slot->state =
        SLOT_QUEUED;

    slot->age =
        s_age++;

    clear_thumb(
        slot->pixels
    );

    xSemaphoreGive(s_lock);

    if (xQueueSend(
            s_queue,
            &slot_index,
            0) != pdTRUE) {

        xSemaphoreTake(
            s_lock,
            portMAX_DELAY
        );

        /*
         * Only revert if this is still the exact queued request.
         */
        if (s_slots[slot_index].state ==
                SLOT_QUEUED &&
            s_slots[slot_index].generation ==
                s_generation &&
            s_slots[slot_index].media_index ==
                media_index) {
            s_slots[slot_index].state =
                SLOT_EMPTY;

            s_slots[slot_index].generation =
                0;
        }

        xSemaphoreGive(s_lock);

        return TYPEN_THUMB_FAILED;
    }

    return TYPEN_THUMB_LOADING;
}

bool typen_thumb_cache_take_dirty(void)
{
    if (!s_lock) {
        return false;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    const bool dirty = s_dirty;
    s_dirty = false;

    xSemaphoreGive(s_lock);

    return dirty;
}


void typen_thumb_cache_set_paused(bool paused)
{
    if (!s_lock) {
        return;
    }

    xSemaphoreTake(
        s_lock,
        portMAX_DELAY
    );

    s_paused = paused;

    xSemaphoreGive(
        s_lock
    );
}
