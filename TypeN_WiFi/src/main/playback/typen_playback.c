#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "typen_board.h"
#include "typen_bmp.h"
#include "typen_gif.h"
#include "typen_jpeg.h"
#include "typen_media.h"
#include "typen_playback.h"
#include "typen_png.h"
#include "typen_webp.h"

#define PLAYBACK_W               TYPEN_LCD_W
#define PLAYBACK_H               TYPEN_LCD_H
#define PLAYBACK_BG              0x0000

#define PLAYBACK_STACK_BYTES     (24 * 1024)
#define PLAYBACK_PRIORITY        4
#define GIF_COMMAND_POLL_MS      5

static const char *TAG = "typen_playback";

static QueueHandle_t s_queue = NULL;
static SemaphoreHandle_t s_state_lock = NULL;
static SemaphoreHandle_t s_present_lock = NULL;
static TaskHandle_t s_task = NULL;

static uint16_t *s_playback_fb[2] = {0};
static uint16_t *s_last_presented = NULL;

static bool s_paused = false;
static bool s_has_frame = false;

static bool playback_is_paused(void)
{
    if (!s_state_lock) {
        return false;
    }

    xSemaphoreTake(
        s_state_lock,
        portMAX_DELAY
    );

    const bool paused =
        s_paused;

    xSemaphoreGive(
        s_state_lock
    );

    return paused;
}

static void set_last_presented(
    uint16_t *frame)
{
    xSemaphoreTake(
        s_state_lock,
        portMAX_DELAY
    );

    s_last_presented = frame;
    s_has_frame =
        frame != NULL;

    xSemaphoreGive(
        s_state_lock
    );
}

static void clear_frame(
    uint16_t *frame)
{
    if (!frame) {
        return;
    }

    memset(
        frame,
        0,
        (size_t)PLAYBACK_W *
        PLAYBACK_H *
        sizeof(uint16_t)
    );
}

static esp_err_t present_buffer(
    uint16_t *frame,
    bool ignore_pause)
{
    if (!frame ||
        !s_present_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!ignore_pause &&
        playback_is_paused()) {
        return ESP_OK;
    }

    xSemaphoreTake(
        s_present_lock,
        portMAX_DELAY
    );

    if (!ignore_pause &&
        playback_is_paused()) {
        xSemaphoreGive(
            s_present_lock
        );

        return ESP_OK;
    }

    esp_err_t err =
        typen_board_present_buffer(
            frame
        );

    if (err == ESP_OK) {
        set_last_presented(
            frame
        );
    }

    xSemaphoreGive(
        s_present_lock
    );

    return err;
}

static void jpeg_oriented_dimensions(
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

static void jpeg_map_oriented_to_source(
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
            sx =
                sw - 1 - oriented_x;
            sy =
                oriented_y;
            break;

        case 3:
            sx =
                sw - 1 - oriented_x;
            sy =
                sh - 1 - oriented_y;
            break;

        case 4:
            sx =
                oriented_x;
            sy =
                sh - 1 - oriented_y;
            break;

        case 5:
            sx =
                oriented_y;
            sy =
                oriented_x;
            break;

        case 6:
            sx =
                oriented_y;
            sy =
                sh - 1 - oriented_x;
            break;

        case 7:
            sx =
                sw - 1 - oriented_y;
            sy =
                sh - 1 - oriented_x;
            break;

        case 8:
            sx =
                sw - 1 - oriented_y;
            sy =
                oriented_x;
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

static void draw_jpeg_cover(
    const typen_jpeg_frame_t *frame,
    uint16_t *target)
{
    if (!frame ||
        !frame->pixels ||
        !target ||
        frame->width == 0 ||
        frame->height == 0 ||
        frame->stride_pixels == 0) {
        return;
    }

    int oriented_w = 0;
    int oriented_h = 0;

    jpeg_oriented_dimensions(
        frame,
        &oriented_w,
        &oriented_h
    );

    int crop_x = 0;
    int crop_y = 0;
    int crop_w = oriented_w;
    int crop_h = oriented_h;

    /*
     * CENTER + COVER against the display-oriented dimensions. This means an
     * EXIF-rotated portrait JPEG is cropped after orientation, not before it.
     */
    if ((int64_t)oriented_w * PLAYBACK_H >
        (int64_t)oriented_h * PLAYBACK_W) {
        crop_w =
            (int)(((int64_t)oriented_h *
                   PLAYBACK_W) /
                  PLAYBACK_H);

        if (crop_w < 1) {
            crop_w = 1;
        }

        crop_x =
            (oriented_w - crop_w) / 2;
    } else if ((int64_t)oriented_h * PLAYBACK_W >
               (int64_t)oriented_w * PLAYBACK_H) {
        crop_h =
            (int)(((int64_t)oriented_w *
                   PLAYBACK_H) /
                  PLAYBACK_W);

        if (crop_h < 1) {
            crop_h = 1;
        }

        crop_y =
            (oriented_h - crop_h) / 2;
    }

    const uint16_t *src =
        (const uint16_t *)
        frame->pixels;

    for (int dy = 0;
         dy < PLAYBACK_H;
         ++dy) {
        const int oriented_y =
            crop_y +
            (int)(((int64_t)dy *
                   crop_h) /
                  PLAYBACK_H);

        uint16_t *dst =
            target +
            (size_t)dy *
            PLAYBACK_W;

        for (int dx = 0;
             dx < PLAYBACK_W;
             ++dx) {
            const int oriented_x =
                crop_x +
                (int)(((int64_t)dx *
                       crop_w) /
                      PLAYBACK_W);

            int sx = 0;
            int sy = 0;

            jpeg_map_oriented_to_source(
                frame,
                oriented_x,
                oriented_y,
                &sx,
                &sy
            );

            dst[dx] =
                src[
                    (size_t)sy *
                    frame->stride_pixels +
                    sx
                ];
        }

        if ((dy & 31) == 0) {
            taskYIELD();
        }
    }

    ESP_LOGI(
        TAG,
        "JPEG cover: source=%ux%u orientation=%u oriented=%dx%d crop=%dx%d@%d,%d -> %dx%d",
        (unsigned)frame->width,
        (unsigned)frame->height,
        (unsigned)frame->orientation,
        oriented_w,
        oriented_h,
        crop_w,
        crop_h,
        crop_x,
        crop_y,
        PLAYBACK_W,
        PLAYBACK_H
    );
}

static esp_err_t render_jpeg(
    const typen_media_item_t *item,
    uint16_t *target)
{
    typen_jpeg_frame_t frame = {0};

    esp_err_t err =
        typen_jpeg_decode_file(
            item->path,
            &frame
        );

    if (err != ESP_OK) {
        return err;
    }

    draw_jpeg_cover(
        &frame,
        target
    );

    typen_jpeg_free(
        &frame
    );

    return ESP_OK;
}

static esp_err_t render_png(
    const typen_media_item_t *item,
    uint16_t *target)
{
    clear_frame(
        target
    );

    return typen_png_draw_preview(
        item->path,
        target,
        PLAYBACK_W,
        PLAYBACK_H,
        0,
        0,
        PLAYBACK_W,
        PLAYBACK_H,
        PLAYBACK_BG
    );
}

static esp_err_t render_bmp(
    const typen_media_item_t *item,
    uint16_t *target)
{
    clear_frame(
        target
    );

    return typen_bmp_draw_preview(
        item->path,
        target,
        PLAYBACK_W,
        PLAYBACK_H,
        0,
        0,
        PLAYBACK_W,
        PLAYBACK_H,
        PLAYBACK_BG
    );
}

static esp_err_t render_webp(
    const typen_media_item_t *item,
    uint16_t *target)
{
    clear_frame(
        target
    );

    return typen_webp_draw_preview(
        item->path,
        target,
        PLAYBACK_W,
        PLAYBACK_H,
        0,
        0,
        PLAYBACK_W,
        PLAYBACK_H,
        PLAYBACK_BG
    );
}

static bool receive_replacement(
    typen_media_item_t *replacement,
    TickType_t wait_ticks)
{
    if (!replacement) {
        return false;
    }

    return xQueueReceive(
        s_queue,
        replacement,
        wait_ticks
    ) == pdTRUE;
}

static bool wait_remaining_frame_time(
    uint32_t remaining_ms,
    typen_media_item_t *replacement)
{
    uint32_t left =
        remaining_ms;

    while (left > 0) {
        if (playback_is_paused()) {
            if (receive_replacement(
                    replacement,
                    pdMS_TO_TICKS(25))) {
                return true;
            }

            continue;
        }

        uint32_t slice_ms =
            left > GIF_COMMAND_POLL_MS
                ? GIF_COMMAND_POLL_MS
                : left;

        TickType_t ticks =
            pdMS_TO_TICKS(slice_ms);

        if (ticks == 0) {
            ticks = 1;
        }

        const TickType_t before =
            xTaskGetTickCount();

        if (receive_replacement(
                replacement,
                ticks)) {
            return true;
        }

        const TickType_t after =
            xTaskGetTickCount();

        uint32_t elapsed_ms =
            (uint32_t)(
                (after - before) *
                portTICK_PERIOD_MS
            );

        if (elapsed_ms == 0) {
            elapsed_ms = 1;
        }

        if (elapsed_ms >= left) {
            left = 0;
        } else {
            left -= elapsed_ms;
        }
    }

    return false;
}

static esp_err_t run_static_item(
    const typen_media_item_t *item)
{
    uint16_t *target =
        s_playback_fb[0];

    if (!target) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "Render static media: %s",
        item->path
    );

    esp_err_t err =
        ESP_ERR_NOT_SUPPORTED;

    switch (item->type) {
        case TYPEN_MEDIA_JPEG:
            err =
                render_jpeg(
                    item,
                    target
                );
            break;

        case TYPEN_MEDIA_PNG:
            err =
                render_png(
                    item,
                    target
                );
            break;

        case TYPEN_MEDIA_BMP:
            err =
                render_bmp(
                    item,
                    target
                );
            break;

        case TYPEN_MEDIA_WEBP:
            err =
                render_webp(
                    item,
                    target
                );
            break;

        default:
            return ESP_ERR_NOT_SUPPORTED;
    }

    if (err != ESP_OK) {
        return err;
    }

    err =
        present_buffer(
            target,
            false
        );

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Static frame ready: %s",
            item->path
        );
    }

    return err;
}

static esp_err_t run_gif_item(
    const typen_media_item_t *item,
    typen_media_item_t *replacement,
    bool *have_replacement)
{
    *have_replacement = false;

    ESP_LOGI(
        TAG,
        "Start PPA GIF playback: %s",
        item->path
    );

    typen_gif_player_t *player = NULL;

    esp_err_t err =
        typen_gif_player_open(
            item->path,
            &player
        );

    if (err != ESP_OK) {
        return err;
    }

    /*
     * Two playback buffers:
     * one is scanned by DSI while PPA renders the next frame into the other.
     */
    clear_frame(
        s_playback_fb[0]
    );

    clear_frame(
        s_playback_fb[1]
    );

    size_t render_index = 0;

    uint32_t perf_frames = 0;
    uint64_t perf_total_us = 0;
    uint32_t perf_max_us = 0;

    for (;;) {
        if (receive_replacement(
                replacement,
                0)) {
            *have_replacement = true;
            err = ESP_OK;
            break;
        }

        if (playback_is_paused()) {
            if (receive_replacement(
                    replacement,
                    pdMS_TO_TICKS(25))) {
                *have_replacement = true;
                err = ESP_OK;
                break;
            }

            continue;
        }

        uint16_t *target =
            s_playback_fb[
                render_index
            ];

        const int64_t frame_start_us =
            esp_timer_get_time();

        uint32_t delay_ms = 0;
        int dirty_y0 = 0;
        int dirty_y1 = PLAYBACK_H;

        err =
            typen_gif_player_next_frame(
                player,
                target,
                PLAYBACK_W,
                PLAYBACK_H,
                PLAYBACK_BG,
                &delay_ms,
                &dirty_y0,
                &dirty_y1
            );

        if (err != ESP_OK) {
            break;
        }

        err =
            present_buffer(
                target,
                false
            );

        if (err != ESP_OK) {
            break;
        }

        render_index ^=
            1U;

        const uint32_t work_us =
            (uint32_t)(
                esp_timer_get_time() -
                frame_start_us
            );

        perf_frames++;
        perf_total_us += work_us;

        if (work_us > perf_max_us) {
            perf_max_us = work_us;
        }

        if ((perf_frames % 120U) == 0U) {
            const uint32_t avg_us =
                (uint32_t)(
                    perf_total_us /
                    perf_frames
                );

            ESP_LOGI(
                TAG,
                "GIF perf PPA: avg=%u.%03u ms max=%u.%03u ms frame-delay=%u ms",
                (unsigned)(avg_us / 1000U),
                (unsigned)(avg_us % 1000U),
                (unsigned)(perf_max_us / 1000U),
                (unsigned)(perf_max_us % 1000U),
                (unsigned)delay_ms
            );
        }

        const uint32_t work_ms =
            (work_us + 999U) /
            1000U;

        const uint32_t remaining_ms =
            delay_ms > work_ms
                ? delay_ms - work_ms
                : 0U;

        if (remaining_ms > 0) {
            if (wait_remaining_frame_time(
                    remaining_ms,
                    replacement)) {
                *have_replacement = true;
                err = ESP_OK;
                break;
            }
        } else {
            /*
             * Never let an over-budget GIF monopolize CPU0 indefinitely.
             * The previous implementation had zero blocking time whenever
             * work_ms > frame delay, so IDLE0 could never run and the task
             * watchdog reset the system.
             */
            vTaskDelay(1);
        }
    }

    typen_gif_player_close(
        player
    );

    return err;
}

static void playback_task(void *arg)
{
    (void)arg;

    typen_media_item_t current;
    typen_media_item_t replacement;

    bool have_current = false;

    for (;;) {
        if (!have_current) {
            if (xQueueReceive(
                    s_queue,
                    &current,
                    portMAX_DELAY) != pdTRUE) {
                continue;
            }

            have_current = true;
        }

        esp_err_t err =
            ESP_OK;

        if (current.type ==
            TYPEN_MEDIA_GIF) {
            bool replaced =
                false;

            err =
                run_gif_item(
                    &current,
                    &replacement,
                    &replaced
                );

            if (replaced) {
                current =
                    replacement;

                have_current =
                    true;

                continue;
            }
        } else {
            err =
                run_static_item(
                    &current
                );
        }

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Playback failed for %s: %s",
                current.path,
                esp_err_to_name(err)
            );
        }

        if (xQueueReceive(
                s_queue,
                &current,
                portMAX_DELAY) == pdTRUE) {
            have_current =
                true;
        } else {
            have_current =
                false;
        }
    }
}

esp_err_t typen_playback_init(void)
{
    if (typen_board_playback_buffer_count() < 2) {
        ESP_LOGE(
            TAG,
            "PPA GIF playback requires two back buffers"
        );

        return ESP_ERR_INVALID_STATE;
    }

    s_playback_fb[0] =
        typen_board_playback_buffer(0);

    s_playback_fb[1] =
        typen_board_playback_buffer(1);

    if (!s_playback_fb[0] ||
        !s_playback_fb[1]) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_state_lock) {
        s_state_lock =
            xSemaphoreCreateMutex();

        if (!s_state_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_present_lock) {
        s_present_lock =
            xSemaphoreCreateMutex();

        if (!s_present_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_queue) {
        s_queue =
            xQueueCreate(
                1,
                sizeof(typen_media_item_t)
            );

        if (!s_queue) {
            return ESP_ERR_NO_MEM;
        }
    }

    clear_frame(
        s_playback_fb[0]
    );

    clear_frame(
        s_playback_fb[1]
    );

    if (!s_task) {
        BaseType_t ok =
            xTaskCreate(
                playback_task,
                "typen_playback",
                PLAYBACK_STACK_BYTES,
                NULL,
                PLAYBACK_PRIORITY,
                &s_task
            );

        if (ok != pdPASS) {
            s_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(
        TAG,
        "PPA playback ready: backbuffers=%p,%p",
        (void *)s_playback_fb[0],
        (void *)s_playback_fb[1]
    );

    return ESP_OK;
}

esp_err_t typen_playback_set(
    const typen_media_item_t *item)
{
    if (!item ||
        item->type == TYPEN_MEDIA_UNKNOWN ||
        item->path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueOverwrite(
            s_queue,
            item) != pdTRUE) {
        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "Queued: %s",
        item->path
    );

    return ESP_OK;
}

void typen_playback_pause(bool paused)
{
    if (!s_state_lock) {
        return;
    }

    xSemaphoreTake(
        s_state_lock,
        portMAX_DELAY
    );

    s_paused = paused;

    xSemaphoreGive(
        s_state_lock
    );

    if (paused &&
        s_present_lock) {
        xSemaphoreTake(
            s_present_lock,
            portMAX_DELAY
        );

        xSemaphoreGive(
            s_present_lock
        );
    }
}

const uint16_t *typen_playback_current_framebuffer(void)
{
    if (!s_state_lock) {
        return NULL;
    }

    xSemaphoreTake(
        s_state_lock,
        portMAX_DELAY
    );

    const uint16_t *frame =
        s_last_presented;

    xSemaphoreGive(
        s_state_lock
    );

    return frame;
}

esp_err_t typen_playback_restore(void)
{
    xSemaphoreTake(
        s_state_lock,
        portMAX_DELAY
    );

    uint16_t *frame =
        s_last_presented;

    xSemaphoreGive(
        s_state_lock
    );

    if (!frame) {
        return ESP_ERR_NOT_FOUND;
    }

    return present_buffer(
        frame,
        true
    );
}

bool typen_playback_has_frame(void)
{
    if (!s_state_lock) {
        return false;
    }

    xSemaphoreTake(
        s_state_lock,
        portMAX_DELAY
    );

    const bool has =
        s_has_frame;

    xSemaphoreGive(
        s_state_lock
    );

    return has;
}
