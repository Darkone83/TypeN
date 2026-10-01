#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "png.h"

#include "typen_png.h"

/*
 * PNG preview policy:
 *
 * - Never expand the full source image to RGBA.
 * - Non-interlaced PNGs use ONE source row plus the small destination preview.
 * - Interlaced PNGs retain only source rows that are actually sampled.
 * - Yield periodically while libpng inflates large images so the rest of the
 *   ESP32-P4 remains schedulable.
 *
 * This is intentionally a low-memory / low-pressure thumbnail path.
 */
#define MAX_PNG_DIMENSION          16384U
#define PNG_YIELD_EVERY_ROWS       24U
#define MAX_INTERLACE_SAMPLE_BYTES (12U * 1024U * 1024U)

static const char *TAG = "typen_png";


static png_voidp typen_png_malloc(
    png_structp png_ptr,
    png_alloc_size_t size)
{
    (void)png_ptr;

    return heap_caps_malloc_prefer(
        (size_t)size,
        2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );
}

static void typen_png_free(
    png_structp png_ptr,
    png_voidp ptr)
{
    (void)png_ptr;
    heap_caps_free(ptr);
}

typedef struct {
    FILE *file;
    png_structp png;
    png_infop info;

    uint8_t *row;
    uint8_t *sampled_rows;
    int32_t *sample_map;

    png_uint_32 source_w;
    png_uint_32 source_h;
    png_size_t rowbytes;

    int draw_w;
    int draw_h;
    int offset_x;
    int offset_y;

    int crop_x;
    int crop_y;
    int crop_w;
    int crop_h;

    uint16_t *framebuffer;
    int framebuffer_width;
    int framebuffer_height;

    uint16_t background;
} png_preview_ctx_t;

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

    for (int yy = 0; yy < h; ++yy) {
        const int py = y + yy;

        if ((unsigned)py >= (unsigned)fb_h) {
            continue;
        }

        for (int xx = 0; xx < w; ++xx) {
            const int px = x + xx;

            if ((unsigned)px >= (unsigned)fb_w) {
                continue;
            }

            fb[py * fb_w + px] = color;
        }
    }
}

static void calc_cover_crop(
    int src_w,
    int src_h,
    int box_w,
    int box_h,
    int *crop_x,
    int *crop_y,
    int *crop_w,
    int *crop_h)
{
    int cx = 0;
    int cy = 0;
    int cw = src_w;
    int ch = src_h;

    if ((int64_t)src_w * box_h >
        (int64_t)src_h * box_w) {
        cw =
            (int)(((int64_t)src_h *
                   box_w) /
                  box_h);

        if (cw < 1) {
            cw = 1;
        }

        cx =
            (src_w - cw) / 2;
    } else if ((int64_t)src_h * box_w >
               (int64_t)src_w * box_h) {
        ch =
            (int)(((int64_t)src_w *
                   box_h) /
                  box_w);

        if (ch < 1) {
            ch = 1;
        }

        cy =
            (src_h - ch) / 2;
    }

    *crop_x = cx;
    *crop_y = cy;
    *crop_w = cw;
    *crop_h = ch;
}

static void background_rgb888(
    uint16_t c,
    uint8_t *r,
    uint8_t *g,
    uint8_t *b)
{
    *r = (uint8_t)(
        ((c >> 11) & 0x1FU) * 255U / 31U
    );

    *g = (uint8_t)(
        ((c >> 5) & 0x3FU) * 255U / 63U
    );

    *b = (uint8_t)(
        (c & 0x1FU) * 255U / 31U
    );
}

static void scale_rgba_row_to_destination(
    const png_preview_ctx_t *ctx,
    const uint8_t *source_row,
    int destination_y)
{
    if (!ctx || !source_row) {
        return;
    }

    const int py =
        ctx->offset_y + destination_y;

    if ((unsigned)py >=
        (unsigned)ctx->framebuffer_height) {
        return;
    }

    uint8_t bg_r, bg_g, bg_b;

    background_rgb888(
        ctx->background,
        &bg_r,
        &bg_g,
        &bg_b
    );

    for (int dx = 0;
         dx < ctx->draw_w;
         ++dx) {
        const png_uint_32 sx =
            (png_uint_32)(
                ctx->crop_x +
                (int)(((uint64_t)dx *
                       (uint32_t)ctx->crop_w) /
                      (uint32_t)ctx->draw_w)
            );

        const uint8_t *p =
            source_row + (size_t)sx * 4U;

        const uint32_t a = p[3];
        const uint32_t inv = 255U - a;

        const uint8_t r =
            (uint8_t)(
                (p[0] * a +
                 bg_r * inv +
                 127U) /
                255U
            );

        const uint8_t g =
            (uint8_t)(
                (p[1] * a +
                 bg_g * inv +
                 127U) /
                255U
            );

        const uint8_t b =
            (uint8_t)(
                (p[2] * a +
                 bg_b * inv +
                 127U) /
                255U
            );

        const int px =
            ctx->offset_x + dx;

        if ((unsigned)px >=
            (unsigned)ctx->framebuffer_width) {
            continue;
        }

        ctx->framebuffer[
            py * ctx->framebuffer_width + px
        ] = rgb565(r, g, b);
    }
}

static void cleanup_ctx(
    png_preview_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->sample_map) {
        heap_caps_free(ctx->sample_map);
        ctx->sample_map = NULL;
    }

    if (ctx->sampled_rows) {
        heap_caps_free(ctx->sampled_rows);
        ctx->sampled_rows = NULL;
    }

    if (ctx->row) {
        heap_caps_free(ctx->row);
        ctx->row = NULL;
    }

    if (ctx->png || ctx->info) {
        png_destroy_read_struct(
            ctx->png ? &ctx->png : NULL,
            ctx->info ? &ctx->info : NULL,
            NULL
        );
    }

    if (ctx->file) {
        fclose(ctx->file);
        ctx->file = NULL;
    }

    heap_caps_free(ctx);
}

static esp_err_t normalize_png(
    png_preview_ctx_t *ctx,
    int *passes_out,
    int *interlace_out)
{
    png_read_info(
        ctx->png,
        ctx->info
    );

    ctx->source_w =
        png_get_image_width(
            ctx->png,
            ctx->info
        );

    ctx->source_h =
        png_get_image_height(
            ctx->png,
            ctx->info
        );

    int bit_depth =
        png_get_bit_depth(
            ctx->png,
            ctx->info
        );

    int color_type =
        png_get_color_type(
            ctx->png,
            ctx->info
        );

    const int interlace_type =
        png_get_interlace_type(
            ctx->png,
            ctx->info
        );

    if (ctx->source_w == 0 ||
        ctx->source_h == 0 ||
        ctx->source_w > MAX_PNG_DIMENSION ||
        ctx->source_h > MAX_PNG_DIMENSION) {
        ESP_LOGW(
            TAG,
            "PNG dimensions unsupported: %ux%u",
            (unsigned)ctx->source_w,
            (unsigned)ctx->source_h
        );

        return ESP_ERR_INVALID_SIZE;
    }

    if (bit_depth == 16) {
        png_set_strip_16(ctx->png);
    }

    if (color_type ==
        PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(ctx->png);
    }

    if (color_type ==
            PNG_COLOR_TYPE_GRAY &&
        bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(
            ctx->png
        );
    }

    const bool has_trns =
        png_get_valid(
            ctx->png,
            ctx->info,
            PNG_INFO_tRNS
        ) != 0;

    if (has_trns) {
        png_set_tRNS_to_alpha(ctx->png);
    }

    if (color_type ==
            PNG_COLOR_TYPE_GRAY ||
        color_type ==
            PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(ctx->png);
    }

    if (!(color_type &
          PNG_COLOR_MASK_ALPHA) &&
        !has_trns) {
        png_set_add_alpha(
            ctx->png,
            0xFF,
            PNG_FILLER_AFTER
        );
    }

    int passes = 1;

    if (interlace_type !=
        PNG_INTERLACE_NONE) {
        passes =
            png_set_interlace_handling(
                ctx->png
            );

        if (passes < 1) {
            passes = 1;
        }
    }

    png_read_update_info(
        ctx->png,
        ctx->info
    );

    const int channels =
        png_get_channels(
            ctx->png,
            ctx->info
        );

    ctx->rowbytes =
        png_get_rowbytes(
            ctx->png,
            ctx->info
        );

    if (channels != 4 ||
        ctx->rowbytes <
            (png_size_t)ctx->source_w *
            4U) {
        return ESP_FAIL;
    }

    *passes_out = passes;
    *interlace_out = interlace_type;

    return ESP_OK;
}

static esp_err_t decode_non_interlaced(
    png_preview_ctx_t *ctx)
{
    ctx->row =
        (uint8_t *)heap_caps_malloc(
            ctx->rowbytes,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (!ctx->row) {
        return ESP_ERR_NO_MEM;
    }

    int next_dy = 0;

    for (png_uint_32 sy = 0;
         sy < ctx->source_h;
         ++sy) {
        png_read_row(
            ctx->png,
            ctx->row,
            NULL
        );

        while (next_dy <
               ctx->draw_h) {
            const png_uint_32 wanted_sy =
                (png_uint_32)(
                    ctx->crop_y +
                    (int)(((uint64_t)next_dy *
                           (uint32_t)ctx->crop_h) /
                          (uint32_t)ctx->draw_h)
                );

            if (wanted_sy != sy) {
                break;
            }

            scale_rgba_row_to_destination(
                ctx,
                ctx->row,
                next_dy
            );

            ++next_dy;
        }

        if ((sy %
             PNG_YIELD_EVERY_ROWS) == 0U) {
            vTaskDelay(1);
        }
    }

    png_read_end(
        ctx->png,
        ctx->info
    );

    return ESP_OK;
}

static esp_err_t decode_interlaced(
    png_preview_ctx_t *ctx,
    int passes)
{
    /*
     * Adam7 requires selected rows to persist between passes. Keep only rows
     * actually used by the destination thumbnail; all other source rows share
     * one scratch buffer.
     */
    ctx->row =
        (uint8_t *)heap_caps_calloc(
            1,
            ctx->rowbytes,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    ctx->sample_map =
        (int32_t *)heap_caps_malloc(
            (size_t)ctx->source_h *
            sizeof(int32_t),
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (!ctx->row ||
        !ctx->sample_map) {
        return ESP_ERR_NO_MEM;
    }

    for (png_uint_32 sy = 0;
         sy < ctx->source_h;
         ++sy) {
        ctx->sample_map[sy] = -1;
    }

    int unique_rows = 0;

    for (int dy = 0;
         dy < ctx->draw_h;
         ++dy) {
        png_uint_32 sy =
            (png_uint_32)(
                ctx->crop_y +
                (int)(((uint64_t)dy *
                       (uint32_t)ctx->crop_h) /
                      (uint32_t)ctx->draw_h)
            );

        if (sy >= ctx->source_h) {
            sy = ctx->source_h - 1U;
        }

        if (ctx->sample_map[sy] < 0) {
            ctx->sample_map[sy] =
                unique_rows++;
        }
    }

    if (unique_rows <= 0) {
        return ESP_FAIL;
    }

    const size_t sample_bytes =
        (size_t)unique_rows *
        (size_t)ctx->rowbytes;

    if (sample_bytes >
        MAX_INTERLACE_SAMPLE_BYTES) {
        ESP_LOGW(
            TAG,
            "Interlaced PNG preview would need %u bytes",
            (unsigned)sample_bytes
        );

        return ESP_ERR_NO_MEM;
    }

    ctx->sampled_rows =
        (uint8_t *)heap_caps_calloc(
            1,
            sample_bytes,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (!ctx->sampled_rows) {
        return ESP_ERR_NO_MEM;
    }

    for (int pass = 0;
         pass < passes;
         ++pass) {
        for (png_uint_32 sy = 0;
             sy < ctx->source_h;
             ++sy) {
            uint8_t *target =
                ctx->row;

            if (ctx->sample_map[sy] >= 0) {
                target =
                    ctx->sampled_rows +
                    (size_t)ctx->sample_map[sy] *
                    (size_t)ctx->rowbytes;
            } else {
                memset(
                    ctx->row,
                    0,
                    ctx->rowbytes
                );
            }

            png_read_row(
                ctx->png,
                target,
                NULL
            );

            if ((sy %
                 PNG_YIELD_EVERY_ROWS) == 0U) {
                vTaskDelay(1);
            }
        }
    }

    png_read_end(
        ctx->png,
        ctx->info
    );

    for (int dy = 0;
         dy < ctx->draw_h;
         ++dy) {
        png_uint_32 sy =
            (png_uint_32)(
                ctx->crop_y +
                (int)(((uint64_t)dy *
                       (uint32_t)ctx->crop_h) /
                      (uint32_t)ctx->draw_h)
            );

        if (sy >= ctx->source_h) {
            sy = ctx->source_h - 1U;
        }

        const int32_t slot =
            ctx->sample_map[sy];

        if (slot < 0) {
            continue;
        }

        const uint8_t *row =
            ctx->sampled_rows +
            (size_t)slot *
            (size_t)ctx->rowbytes;

        scale_rgba_row_to_destination(
            ctx,
            row,
            dy
        );
    }

    return ESP_OK;
}

esp_err_t typen_png_draw_preview(
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

    png_preview_ctx_t *ctx =
        (png_preview_ctx_t *)heap_caps_calloc(
            1,
            sizeof(*ctx),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }

    ctx->framebuffer =
        framebuffer;

    ctx->framebuffer_width =
        framebuffer_width;

    ctx->framebuffer_height =
        framebuffer_height;

    ctx->background =
        background;

    ctx->file =
        fopen(path, "rb");

    if (!ctx->file) {
        cleanup_ctx(ctx);
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * Force libpng/zlib working allocations into PSRAM first. This is
     * deliberate even if CONFIG_LIBPNG_PREFER_PSRAM is already enabled:
     * the decoder remains self-contained and predictable for very large
     * stress-test PNGs.
     */
    ctx->png =
        png_create_read_struct_2(
            PNG_LIBPNG_VER_STRING,
            NULL,
            NULL,
            NULL,
            NULL,
            typen_png_malloc,
            typen_png_free
        );

    if (!ctx->png) {
        cleanup_ctx(ctx);
        return ESP_ERR_NO_MEM;
    }

    ctx->info =
        png_create_info_struct(
            ctx->png
        );

    if (!ctx->info) {
        cleanup_ctx(ctx);
        return ESP_ERR_NO_MEM;
    }

    /*
     * ctx itself lives on the heap. Its cleanup pointers therefore remain
     * valid after libpng's longjmp; we do not rely on modified automatic
     * locals surviving setjmp/longjmp.
     */
    if (setjmp(png_jmpbuf(ctx->png))) {
        ESP_LOGW(
            TAG,
            "libpng rejected/aborted %s",
            path
        );

        cleanup_ctx(ctx);
        return ESP_FAIL;
    }

    png_set_user_limits(
        ctx->png,
        MAX_PNG_DIMENSION,
        MAX_PNG_DIMENSION
    );

    png_init_io(
        ctx->png,
        ctx->file
    );

    int passes = 1;
    int interlace_type =
        PNG_INTERLACE_NONE;

    esp_err_t err =
        normalize_png(
            ctx,
            &passes,
            &interlace_type
        );

    if (err != ESP_OK) {
        cleanup_ctx(ctx);
        return err;
    }

    /*
     * CENTER + COVER:
     * destination always fills the requested box. The source is cropped in
     * the middle to the box aspect ratio before row sampling.
     */
    calc_cover_crop(
        (int)ctx->source_w,
        (int)ctx->source_h,
        width,
        height,
        &ctx->crop_x,
        &ctx->crop_y,
        &ctx->crop_w,
        &ctx->crop_h
    );

    ctx->draw_w =
        width;

    ctx->draw_h =
        height;

    ctx->offset_x =
        x;

    ctx->offset_y =
        y;

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

    if (interlace_type ==
        PNG_INTERLACE_NONE) {
        err =
            decode_non_interlaced(
                ctx
            );
    } else {
        err =
            decode_interlaced(
                ctx,
                passes
            );
    }

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "PNG cover %ux%u crop=%dx%d@%d,%d -> %dx%d%s",
            (unsigned)ctx->source_w,
            (unsigned)ctx->source_h,
            ctx->crop_w,
            ctx->crop_h,
            ctx->crop_x,
            ctx->crop_y,
            ctx->draw_w,
            ctx->draw_h,
            interlace_type !=
                PNG_INTERLACE_NONE
                ? " Adam7"
                : ""
        );
    }

    cleanup_ctx(ctx);
    return err;
}
