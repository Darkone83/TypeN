#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "gifdec.h"
#include "typen_gif.h"

#define MAX_GIF_DIMENSION          4096U
#define MAX_GIF_WORK_BYTES         (20U * 1024U * 1024U)
#define GIF_MIN_DELAY_MS           16U
#define GIF_PATH_MAX               512
#define GIF_PSRAM_RESERVE_BYTES    (6U * 1024U * 1024U)

static const char *TAG = "typen_gif";

typedef struct {
    int x;
    int y;
    int w;
    int h;
} source_rect_t;

struct typen_gif_player {
    gd_GIF *gif;
    char path[GIF_PATH_MAX];

    uint8_t *file_data;
    size_t file_size;

    /*
     * Persistent native-resolution RGB565 canvas.
     * gifdec continues to own its indexed frame + RGB888 logical canvas.
     * We only update changed source rectangles here, then let PPA scale this
     * canvas into the 800x800 playback framebuffer.
     */
    uint16_t *source_rgb565;
    size_t source_bytes;

    uint16_t palette565[256];

    ppa_client_handle_t ppa;

    int target_w;
    int target_h;

    int crop_x;
    int crop_y;
    int crop_w;
    int crop_h;

    float scale_x;
    float scale_y;

    bool have_frame;
};

static inline uint16_t rgb565(
    uint8_t r,
    uint8_t g,
    uint8_t b)
{
    return (uint16_t)(
        ((uint16_t)(r & 0xF8U) << 8) |
        ((uint16_t)(g & 0xFCU) << 3) |
        ((uint16_t)b >> 3)
    );
}

static void fill_rect(
    uint16_t *fb,
    int fb_w,
    int fb_h,
    int x,
    int y,
    int w,
    int h,
    uint16_t color)
{
    if (!fb || w <= 0 || h <= 0) {
        return;
    }

    if (x < 0) {
        w += x;
        x = 0;
    }

    if (y < 0) {
        h += y;
        y = 0;
    }

    if (x + w > fb_w) {
        w = fb_w - x;
    }

    if (y + h > fb_h) {
        h = fb_h - y;
    }

    if (w <= 0 || h <= 0) {
        return;
    }

    for (int yy = 0; yy < h; ++yy) {
        uint16_t *row =
            fb + (y + yy) * fb_w + x;

        for (int xx = 0; xx < w; ++xx) {
            row[xx] = color;
        }
    }
}

static void calc_fit(
    int src_w,
    int src_h,
    int box_w,
    int box_h,
    int *draw_w,
    int *draw_h,
    float *scale)
{
    const float sx =
        (float)box_w / (float)src_w;

    const float sy =
        (float)box_h / (float)src_h;

    const float s =
        sx < sy ? sx : sy;

    int dw =
        (int)((float)src_w * s);

    int dh =
        (int)((float)src_h * s);

    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    if (dw > box_w) dw = box_w;
    if (dh > box_h) dh = box_h;

    *draw_w = dw;
    *draw_h = dh;
    *scale = s;
}

static esp_err_t validate_gif(
    gd_GIF *gif)
{
    if (!gif) {
        return ESP_ERR_INVALID_ARG;
    }

    if (gif->width == 0 ||
        gif->height == 0 ||
        gif->width > MAX_GIF_DIMENSION ||
        gif->height > MAX_GIF_DIMENSION) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t pixels =
        (size_t)gif->width *
        (size_t)gif->height;

    if (pixels > SIZE_MAX / 4U ||
        pixels * 4U > MAX_GIF_WORK_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t load_file_to_psram(
    typen_gif_player_t *player)
{
    FILE *file =
        fopen(
            player->path,
            "rb"
        );

    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    const long file_len =
        ftell(file);

    if (file_len <= 0) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    rewind(file);

    const size_t bytes =
        (size_t)file_len;

    const size_t free_psram =
        heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM
        );

    if (bytes + GIF_PSRAM_RESERVE_BYTES >
        free_psram) {
        ESP_LOGE(
            TAG,
            "GIF cache too large: %u bytes, PSRAM free=%u",
            (unsigned)bytes,
            (unsigned)free_psram
        );

        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *data =
        (uint8_t *)heap_caps_malloc(
            bytes,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (!data) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    const size_t got =
        fread(
            data,
            1,
            bytes,
            file
        );

    fclose(file);

    if (got != bytes) {
        heap_caps_free(data);
        return ESP_FAIL;
    }

    player->file_data = data;
    player->file_size = bytes;

    ESP_LOGI(
        TAG,
        "GIF cached in PSRAM: %u bytes",
        (unsigned)bytes
    );

    return ESP_OK;
}

static esp_err_t open_memory_decoder(
    typen_gif_player_t *player)
{
    player->gif =
        gd_open_gif_mem(
            player->file_data,
            player->file_size
        );

    if (!player->gif) {
        return ESP_FAIL;
    }

    return validate_gif(
        player->gif
    );
}

static void build_active_palette(
    typen_gif_player_t *player)
{
    gd_Palette *palette =
        player->gif->palette;

    int count =
        palette ? palette->size : 0;

    if (count > 256) {
        count = 256;
    }

    for (int i = 0; i < count; ++i) {
        const uint8_t *c =
            &palette->colors[i * 3];

        player->palette565[i] =
            rgb565(
                c[0],
                c[1],
                c[2]
            );
    }
}

static uint16_t background565(
    typen_gif_player_t *player)
{
    gd_GIF *gif =
        player->gif;

    const uint8_t *c =
        &gif->gct.colors[
            gif->bgindex * 3
        ];

    return rgb565(
        c[0],
        c[1],
        c[2]
    );
}

static void fill_source_background(
    typen_gif_player_t *player)
{
    const size_t pixels =
        (size_t)player->gif->width *
        player->gif->height;

    const uint16_t bg =
        background565(player);

    for (size_t i = 0;
         i < pixels;
         ++i) {
        player->source_rgb565[i] = bg;
    }
}

static source_rect_t clamp_source_rect(
    gd_GIF *gif,
    source_rect_t rect)
{
    if (rect.x < 0) {
        rect.w += rect.x;
        rect.x = 0;
    }

    if (rect.y < 0) {
        rect.h += rect.y;
        rect.y = 0;
    }

    if (rect.x + rect.w > gif->width) {
        rect.w =
            gif->width - rect.x;
    }

    if (rect.y + rect.h > gif->height) {
        rect.h =
            gif->height - rect.y;
    }

    if (rect.w < 0) rect.w = 0;
    if (rect.h < 0) rect.h = 0;

    return rect;
}

/*
 * After gd_get_frame(), gif->canvas contains the logical image after disposal
 * of the PREVIOUS frame and before the current frame is merged.
 *
 * We use this only when the previous frame's disposal actually changed pixels.
 */
static void update_rect_from_canvas(
    typen_gif_player_t *player,
    source_rect_t rect)
{
    gd_GIF *gif =
        player->gif;

    rect =
        clamp_source_rect(
            gif,
            rect
        );

    for (int y = 0;
         y < rect.h;
         ++y) {
        const int sy =
            rect.y + y;

        uint16_t *dst =
            player->source_rgb565 +
            sy * gif->width +
            rect.x;

        const uint8_t *src =
            gif->canvas +
            ((size_t)sy *
             gif->width +
             rect.x) * 3U;

        for (int x = 0;
             x < rect.w;
             ++x) {
            dst[x] =
                rgb565(
                    src[x * 3 + 0],
                    src[x * 3 + 1],
                    src[x * 3 + 2]
                );
        }
    }
}

/*
 * Apply only the opaque pixels from the current indexed GIF frame.
 *
 * Transparent pixels are intentionally skipped. source_rgb565 already holds
 * the correct underlying logical canvas state.
 */
static void update_current_frame(
    typen_gif_player_t *player)
{
    gd_GIF *gif =
        player->gif;

    source_rect_t rect = {
        .x = gif->fx,
        .y = gif->fy,
        .w = gif->fw,
        .h = gif->fh,
    };

    rect =
        clamp_source_rect(
            gif,
            rect
        );

    build_active_palette(
        player
    );

    const bool transparent =
        gif->gce.transparency != 0;

    const uint8_t tindex =
        gif->gce.tindex;

    for (int y = 0;
         y < rect.h;
         ++y) {
        const int sy =
            rect.y + y;

        uint16_t *dst =
            player->source_rgb565 +
            sy * gif->width +
            rect.x;

        const uint8_t *src =
            gif->frame +
            sy * gif->width +
            rect.x;

        if (!transparent) {
            for (int x = 0;
                 x < rect.w;
                 ++x) {
                dst[x] =
                    player->palette565[
                        src[x]
                    ];
            }
        } else {
            for (int x = 0;
                 x < rect.w;
                 ++x) {
                const uint8_t index =
                    src[x];

                if (index != tindex) {
                    dst[x] =
                        player->palette565[
                            index
                        ];
                }
            }
        }
    }
}

static esp_err_t prepare_target_geometry(
    typen_gif_player_t *player,
    int target_w,
    int target_h)
{
    if (player->target_w == target_w &&
        player->target_h == target_h &&
        player->crop_w > 0 &&
        player->crop_h > 0) {
        return ESP_OK;
    }

    const int src_w =
        player->gif->width;

    const int src_h =
        player->gif->height;

    int crop_x = 0;
    int crop_y = 0;
    int crop_w = src_w;
    int crop_h = src_h;

    /*
     * First crop the logical GIF canvas to the display aspect ratio.
     */
    if ((int64_t)src_w * target_h >
        (int64_t)src_h * target_w) {
        crop_w =
            (int)(((int64_t)src_h *
                   target_w) /
                  target_h);

        if (crop_w < 1) {
            crop_w = 1;
        }

        crop_x =
            (src_w - crop_w) / 2;
    } else if ((int64_t)src_h * target_w >
               (int64_t)src_w * target_h) {
        crop_h =
            (int)(((int64_t)src_w *
                   target_h) /
                  target_w);

        if (crop_h < 1) {
            crop_h = 1;
        }

        crop_y =
            (src_h - crop_h) / 2;
    }

    /*
     * The ESP32-P4 PPA truncates scale values to 1/16 steps.
     *
     * TypeN's playback target is square (800x800), so search for the largest
     * centered square crop that lands on exactly target_w pixels after that
     * quantization. This prevents the old "computed as centered, rendered
     * smaller" mismatch that could leave GIFs visibly undersized/off-center.
     */
    if (target_w == target_h) {
        const int available =
            crop_w < crop_h
                ? crop_w
                : crop_h;

        int best_crop = 0;
        int best_step = 0;

        for (int step = 1;
             step <= 255;
             ++step) {
            const int candidate =
                (target_w * 16 +
                 step - 1) /
                step;

            if (candidate <= 0 ||
                candidate > available) {
                continue;
            }

            const int output =
                (candidate * step) /
                16;

            if (output != target_w) {
                continue;
            }

            if (candidate > best_crop) {
                best_crop = candidate;
                best_step = step;
            }
        }

        if (best_crop > 0 &&
            best_step > 0) {
            crop_w = best_crop;
            crop_h = best_crop;

            crop_x =
                (src_w - crop_w) / 2;

            crop_y =
                (src_h - crop_h) / 2;

            player->scale_x =
                (float)best_step / 16.0f;

            player->scale_y =
                player->scale_x;
        } else {
            /*
             * Very small/edge-case sources: use the closest safe quantized
             * scale and center the source crop. This still preserves cover
             * semantics as closely as the PPA can represent.
             */
            float desired =
                (float)target_w /
                (float)available;

            int step =
                (int)(desired * 16.0f);

            if ((float)step / 16.0f <
                desired) {
                ++step;
            }

            if (step < 1) step = 1;
            if (step > 255) step = 255;

            const float q =
                (float)step / 16.0f;

            int safe_crop =
                (int)((float)target_w / q);

            if (safe_crop < 1) safe_crop = 1;
            if (safe_crop > available) safe_crop = available;

            crop_w = safe_crop;
            crop_h = safe_crop;

            crop_x =
                (src_w - crop_w) / 2;

            crop_y =
                (src_h - crop_h) / 2;

            player->scale_x = q;
            player->scale_y = q;
        }
    } else {
        /*
         * Generic non-square fallback. TypeN currently uses 800x800, but keep
         * this path valid if the playback target changes later.
         */
        float sx =
            (float)target_w /
            (float)crop_w;

        float sy =
            (float)target_h /
            (float)crop_h;

        int step_x =
            (int)(sx * 16.0f);

        int step_y =
            (int)(sy * 16.0f);

        if ((float)step_x / 16.0f < sx) ++step_x;
        if ((float)step_y / 16.0f < sy) ++step_y;

        if (step_x < 1) step_x = 1;
        if (step_y < 1) step_y = 1;
        if (step_x > 255) step_x = 255;
        if (step_y > 255) step_y = 255;

        player->scale_x =
            (float)step_x / 16.0f;

        player->scale_y =
            (float)step_y / 16.0f;
    }

    player->crop_x = crop_x;
    player->crop_y = crop_y;
    player->crop_w = crop_w;
    player->crop_h = crop_h;

    player->target_w = target_w;
    player->target_h = target_h;

    ESP_LOGI(
        TAG,
        "PPA GIF cover: %ux%u crop=%dx%d@%d,%d scale=%.4f,%.4f -> %dx%d",
        (unsigned)src_w,
        (unsigned)src_h,
        player->crop_w,
        player->crop_h,
        player->crop_x,
        player->crop_y,
        (double)player->scale_x,
        (double)player->scale_y,
        target_w,
        target_h
    );

    return ESP_OK;
}

static esp_err_t ppa_scale_to_target(
    typen_gif_player_t *player,
    uint16_t *target,
    int target_w,
    int target_h)
{
    esp_err_t err =
        prepare_target_geometry(
            player,
            target_w,
            target_h
        );

    if (err != ESP_OK) {
        return err;
    }

    const size_t target_bytes =
        (size_t)target_w *
        target_h *
        sizeof(uint16_t);

    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = player->source_rgb565,
            .pic_w = player->gif->width,
            .pic_h = player->gif->height,
            .block_w = player->crop_w,
            .block_h = player->crop_h,
            .block_offset_x = player->crop_x,
            .block_offset_y = player->crop_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = target,
            .buffer_size = target_bytes,
            .pic_w = target_w,
            .pic_h = target_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle =
            PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = player->scale_x,
        .scale_y = player->scale_y,
        .mirror_x = false,
        .mirror_y = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    return ppa_do_scale_rotate_mirror(
        player->ppa,
        &cfg
    );
}

esp_err_t typen_gif_player_open(
    const char *path,
    typen_gif_player_t **out_player)
{
    if (!path ||
        !out_player ||
        path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    *out_player = NULL;

    if (strlen(path) >= GIF_PATH_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * Keep the hot control/palette state in internal SRAM.
     */
    typen_gif_player_t *player =
        (typen_gif_player_t *)
        heap_caps_calloc(
            1,
            sizeof(*player),
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        );

    if (!player) {
        return ESP_ERR_NO_MEM;
    }

    strcpy(
        player->path,
        path
    );

    esp_err_t err =
        load_file_to_psram(
            player
        );

    if (err == ESP_OK) {
        err =
            open_memory_decoder(
                player
            );
    }

    if (err == ESP_OK) {
        const size_t pixels =
            (size_t)player->gif->width *
            player->gif->height;

        player->source_bytes =
            pixels *
            sizeof(uint16_t);

        /*
         * 128-byte alignment matches the large L2 cache line configuration
         * used by TypeN and is friendly to PPA/DMA2D.
         */
        player->source_rgb565 =
            (uint16_t *)
            heap_caps_aligned_calloc(
                128,
                1,
                player->source_bytes,
                MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
            );

        if (!player->source_rgb565) {
            err = ESP_ERR_NO_MEM;
        }
    }

    if (err == ESP_OK) {
        ppa_client_config_t ppa_cfg = {
            .oper_type =
                PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };

        err =
            ppa_register_client(
                &ppa_cfg,
                &player->ppa
            );
    }

    if (err != ESP_OK) {
        typen_gif_player_close(
            player
        );
        return err;
    }

    fill_source_background(
        player
    );

    ESP_LOGI(
        TAG,
        "GIF PPA player ready: %s (%ux%u), source RGB565=%u bytes",
        path,
        (unsigned)player->gif->width,
        (unsigned)player->gif->height,
        (unsigned)player->source_bytes
    );

    *out_player = player;
    return ESP_OK;
}

esp_err_t typen_gif_player_next_frame(
    typen_gif_player_t *player,
    uint16_t *framebuffer,
    int framebuffer_width,
    int framebuffer_height,
    uint16_t background,
    uint32_t *delay_ms,
    int *dirty_y_start,
    int *dirty_y_end)
{
    if (!player ||
        !player->gif ||
        !player->source_rgb565 ||
        !framebuffer ||
        framebuffer_width <= 0 ||
        framebuffer_height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    source_rect_t previous_rect = {
        .x = player->gif->fx,
        .y = player->gif->fy,
        .w = player->gif->fw,
        .h = player->gif->fh,
    };

    const uint8_t previous_disposal =
        player->gif->gce.disposal;

    const bool had_previous =
        player->have_frame;

    int frame_result =
        gd_get_frame(
            player->gif
        );

    bool loop_reset = false;

    if (frame_result == 0) {
        gd_rewind(
            player->gif
        );

        fill_source_background(
            player
        );

        player->have_frame = false;
        loop_reset = true;

        frame_result =
            gd_get_frame(
                player->gif
            );
    }

    if (frame_result != 1) {
        return ESP_FAIL;
    }

    /*
     * gd_get_frame() has already applied the PREVIOUS disposal to gif->canvas.
     * Mirror only the area that changed because of that disposal.
     */
    if (!loop_reset &&
        had_previous &&
        (previous_disposal == 2 ||
         previous_disposal == 3)) {
        update_rect_from_canvas(
            player,
            previous_rect
        );
    }

    /*
     * Then overlay the current frame's opaque pixels onto our native RGB565
     * logical canvas.
     */
    update_current_frame(
        player
    );

    esp_err_t err =
        ppa_scale_to_target(
            player,
            framebuffer,
            framebuffer_width,
            framebuffer_height
        );

    if (err != ESP_OK) {
        return err;
    }

    player->have_frame = true;

    uint32_t ms =
        (uint32_t)player->gif->gce.delay *
        10U;

    if (ms < GIF_MIN_DELAY_MS) {
        ms = GIF_MIN_DELAY_MS;
    }

    if (delay_ms) {
        *delay_ms = ms;
    }

    /*
     * PPA produces the complete scaled image each frame.
     */
    if (dirty_y_start) {
        *dirty_y_start = 0;
    }

    if (dirty_y_end) {
        *dirty_y_end =
            framebuffer_height;
    }

    (void)background;

    return ESP_OK;
}

void typen_gif_player_close(
    typen_gif_player_t *player)
{
    if (!player) {
        return;
    }

    if (player->ppa) {
        ppa_unregister_client(
            player->ppa
        );

        player->ppa = NULL;
    }

    if (player->gif) {
        gd_close_gif(
            player->gif
        );

        player->gif = NULL;
    }

    if (player->source_rgb565) {
        heap_caps_free(
            player->source_rgb565
        );

        player->source_rgb565 = NULL;
    }

    if (player->file_data) {
        heap_caps_free(
            player->file_data
        );

        player->file_data = NULL;
    }

    heap_caps_free(
        player
    );
}

/* -------------------------------------------------------------------------- */
/* Browser preview                                                             */
/* -------------------------------------------------------------------------- */

esp_err_t typen_gif_draw_preview(
    const char *path,
    uint16_t *framebuffer,
    int framebuffer_width,
    int framebuffer_height,
    int x,
    int y,
    int width,
    int height,
    uint16_t background)
{
    if (!path ||
        !framebuffer ||
        framebuffer_width <= 0 ||
        framebuffer_height <= 0 ||
        width <= 0 ||
        height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gd_GIF *gif =
        gd_open_gif(path);

    if (!gif) {
        return ESP_FAIL;
    }

    esp_err_t err =
        validate_gif(gif);

    if (err != ESP_OK) {
        gd_close_gif(gif);
        return err;
    }

    if (gd_get_frame(gif) != 1) {
        gd_close_gif(gif);
        return ESP_FAIL;
    }

    const size_t rgb_bytes =
        (size_t)gif->width *
        gif->height *
        3U;

    uint8_t *rgb =
        (uint8_t *)heap_caps_malloc(
            rgb_bytes,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (!rgb) {
        gd_close_gif(gif);
        return ESP_ERR_NO_MEM;
    }

    gd_render_frame(
        gif,
        rgb
    );

    fill_rect(
        framebuffer,
        framebuffer_width,
        framebuffer_height,
        x,
        y,
        width,
        height,
        background
    );

    int crop_x = 0;
    int crop_y = 0;
    int crop_w = gif->width;
    int crop_h = gif->height;

    /*
     * CENTER + COVER for thumbnail previews.
     */
    if ((int64_t)gif->width * height >
        (int64_t)gif->height * width) {
        crop_w =
            (int)(((int64_t)gif->height *
                   width) /
                  height);

        if (crop_w < 1) crop_w = 1;

        crop_x =
            (gif->width - crop_w) / 2;
    } else if ((int64_t)gif->height * width >
               (int64_t)gif->width * height) {
        crop_h =
            (int)(((int64_t)gif->width *
                   height) /
                  width);

        if (crop_h < 1) crop_h = 1;

        crop_y =
            (gif->height - crop_h) / 2;
    }

    const int draw_w =
        width;

    const int draw_h =
        height;

    const int ox =
        x;

    const int oy =
        y;

    for (int dy = 0;
         dy < draw_h;
         ++dy) {
        const int sy =
            crop_y +
            (int)(((int64_t)dy *
                   crop_h) /
                  draw_h);

        uint16_t *dst =
            framebuffer +
            (oy + dy) *
            framebuffer_width +
            ox;

        const uint8_t *src_row =
            rgb +
            (size_t)sy *
            gif->width *
            3U;

        for (int dx = 0;
             dx < draw_w;
             ++dx) {
            const int sx =
                crop_x +
                (int)(((int64_t)dx *
                       crop_w) /
                      draw_w);

            const uint8_t *c =
                src_row +
                sx * 3;

            dst[dx] =
                rgb565(
                    c[0],
                    c[1],
                    c[2]
                );
        }
    }

    heap_caps_free(rgb);
    gd_close_gif(gif);

    return ESP_OK;
}
