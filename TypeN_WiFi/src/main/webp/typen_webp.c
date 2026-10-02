#include "typen_webp.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

/*
 * SimpleWebP is intentionally vendored as a header rather than added as an
 * ESP-IDF managed component. That keeps the existing TypeN dependency graph
 * unchanged. See fetch_simplewebp.ps1 in this patch set.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define SIMPLEWEBP_IMPLEMENTATION
#include "simplewebp.h"
#pragma GCC diagnostic pop

#define WEBP_RIFF_HEADER_BYTES 12U
#define WEBP_CHUNK_HEADER_BYTES 8U
#define WEBP_VP8X_ANIMATION_FLAG 0x02U

static const char *TAG = "typen_webp";

static uint32_t read_le32(
    const uint8_t *p)
{
    return
        (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static bool seek_forward(
    FILE *file,
    uint64_t bytes)
{
    if (!file ||
        bytes > (uint64_t)LONG_MAX) {
        return false;
    }

    return fseek(
        file,
        (long)bytes,
        SEEK_CUR
    ) == 0;
}

static void *webp_psram_alloc(
    void *userdata,
    size_t size)
{
    (void)userdata;

    if (size == 0U) {
        size = 1U;
    }

    return heap_caps_malloc(
        size,
        MALLOC_CAP_SPIRAM |
        MALLOC_CAP_8BIT
    );
}

static void webp_psram_free(
    void *userdata,
    void *mem)
{
    (void)userdata;

    if (mem) {
        heap_caps_free(mem);
    }
}

static esp_err_t webp_error_to_esp(
    simplewebp_error err)
{
    switch (err) {
        case SIMPLEWEBP_NO_ERROR:
            return ESP_OK;

        case SIMPLEWEBP_ALLOC_ERROR:
            return ESP_ERR_NO_MEM;

        case SIMPLEWEBP_UNSUPPORTED_ERROR:
        case SIMPLEWEBP_IS_LOSSLESS_ERROR:
            return ESP_ERR_NOT_SUPPORTED;

        case SIMPLEWEBP_NOT_WEBP_ERROR:
        case SIMPLEWEBP_CORRUPT_ERROR:
            return ESP_ERR_INVALID_ARG;

        case SIMPLEWEBP_IO_ERROR:
        default:
            return ESP_FAIL;
    }
}

static esp_err_t webp_check_static(
    const char *path,
    bool *animated)
{
    if (!path || !animated) {
        return ESP_ERR_INVALID_ARG;
    }

    *animated = false;

    FILE *file =
        fopen(
            path,
            "rb"
        );

    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t header[WEBP_RIFF_HEADER_BYTES];

    if (fread(
            header,
            1,
            sizeof(header),
            file) != sizeof(header)) {
        fclose(file);
        return ESP_FAIL;
    }

    if (memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WEBP", 4) != 0) {
        fclose(file);
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t riff_end =
        UINT64_C(8) +
        (uint64_t)read_le32(
            header + 4
        );

    uint64_t position =
        WEBP_RIFF_HEADER_BYTES;

    while (position + WEBP_CHUNK_HEADER_BYTES <=
           riff_end) {
        uint8_t chunk_header[WEBP_CHUNK_HEADER_BYTES];

        if (fread(
                chunk_header,
                1,
                sizeof(chunk_header),
                file) != sizeof(chunk_header)) {
            break;
        }

        position +=
            WEBP_CHUNK_HEADER_BYTES;

        const uint32_t chunk_size =
            read_le32(
                chunk_header + 4
            );

        if (memcmp(
                chunk_header,
                "ANIM",
                4) == 0 ||
            memcmp(
                chunk_header,
                "ANMF",
                4) == 0) {
            *animated = true;
            fclose(file);
            return ESP_OK;
        }

        if (memcmp(
                chunk_header,
                "VP8X",
                4) == 0 &&
            chunk_size >= 1U) {
            uint8_t flags = 0;

            if (fread(
                    &flags,
                    1,
                    1,
                    file) != 1U) {
                fclose(file);
                return ESP_FAIL;
            }

            if ((flags & WEBP_VP8X_ANIMATION_FLAG) != 0U) {
                *animated = true;
                fclose(file);
                return ESP_OK;
            }

            const uint64_t rest =
                (uint64_t)chunk_size - 1U;

            if (!seek_forward(
                    file,
                    rest)) {
                fclose(file);
                return ESP_FAIL;
            }
        } else {
            if (!seek_forward(
                    file,
                    (uint64_t)chunk_size)) {
                fclose(file);
                return ESP_FAIL;
            }
        }

        position +=
            (uint64_t)chunk_size;

        if ((chunk_size & 1U) != 0U) {
            if (!seek_forward(
                    file,
                    1U)) {
                fclose(file);
                return ESP_FAIL;
            }

            ++position;
        }
    }

    fclose(file);
    return ESP_OK;
}

static uint8_t clip_u8(
    int value)
{
    if (value < 0) {
        return 0U;
    }

    if (value > 255) {
        return 255U;
    }

    return (uint8_t)value;
}

static uint16_t rgb565(
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

static void rgb565_to_rgb888(
    uint16_t pixel,
    uint8_t *r,
    uint8_t *g,
    uint8_t *b)
{
    const uint8_t r5 =
        (uint8_t)((pixel >> 11) & 0x1FU);

    const uint8_t g6 =
        (uint8_t)((pixel >> 5) & 0x3FU);

    const uint8_t b5 =
        (uint8_t)(pixel & 0x1FU);

    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static uint16_t blend_rgb565(
    uint8_t r,
    uint8_t g,
    uint8_t b,
    uint8_t a,
    uint16_t background)
{
    if (a >= 255U) {
        return rgb565(r, g, b);
    }

    if (a == 0U) {
        return background;
    }

    uint8_t br = 0;
    uint8_t bg = 0;
    uint8_t bb = 0;

    rgb565_to_rgb888(
        background,
        &br,
        &bg,
        &bb
    );

    const unsigned inv =
        255U - a;

    return rgb565(
        (uint8_t)(
            ((unsigned)r * a +
             (unsigned)br * inv +
             127U) /
            255U
        ),
        (uint8_t)(
            ((unsigned)g * a +
             (unsigned)bg * inv +
             127U) /
            255U
        ),
        (uint8_t)(
            ((unsigned)b * a +
             (unsigned)bb * inv +
             127U) /
            255U
        )
    );
}

static void compute_cover_crop(
    int source_w,
    int source_h,
    int box_w,
    int box_h,
    int *crop_x,
    int *crop_y,
    int *crop_w,
    int *crop_h)
{
    *crop_x = 0;
    *crop_y = 0;
    *crop_w = source_w;
    *crop_h = source_h;

    if ((int64_t)source_w * box_h >
        (int64_t)source_h * box_w) {
        *crop_w =
            (int)(((int64_t)source_h * box_w) /
                  box_h);

        if (*crop_w < 1) {
            *crop_w = 1;
        }

        *crop_x =
            (source_w - *crop_w) / 2;
    } else if ((int64_t)source_h * box_w >
               (int64_t)source_w * box_h) {
        *crop_h =
            (int)(((int64_t)source_w * box_h) /
                  box_w);

        if (*crop_h < 1) {
            *crop_h = 1;
        }

        *crop_y =
            (source_h - *crop_h) / 2;
    }
}

static void yuv_to_rgb(
    uint8_t y,
    uint8_t u,
    uint8_t v,
    uint8_t *r,
    uint8_t *g,
    uint8_t *b)
{
    int c = (int)y - 16;
    const int d = (int)u - 128;
    const int e = (int)v - 128;

    if (c < 0) {
        c = 0;
    }

    *r = clip_u8(
        (298 * c + 409 * e + 128) >> 8
    );

    *g = clip_u8(
        (298 * c - 100 * d - 208 * e + 128) >> 8
    );

    *b = clip_u8(
        (298 * c + 516 * d + 128) >> 8
    );
}

static esp_err_t render_lossy(
    simplewebp *image,
    size_t width,
    size_t height,
    uint16_t *dst,
    int dst_width,
    int box_x,
    int box_y,
    int box_w,
    int box_h,
    uint16_t background)
{
    if (width > SIZE_MAX / height) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t pixels =
        width * height;

    const size_t uv_width =
        (width + 1U) / 2U;

    const size_t uv_height =
        (height + 1U) / 2U;

    if (uv_width > SIZE_MAX / uv_height) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t uv_pixels =
        uv_width * uv_height;

    if (pixels > (SIZE_MAX - uv_pixels * 2U) / 2U) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t allocation =
        pixels * 2U +
        uv_pixels * 2U;

    uint8_t *planes =
        (uint8_t *)heap_caps_malloc(
            allocation,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (!planes) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *y_plane = planes;
    uint8_t *a_plane = y_plane + pixels;
    uint8_t *u_plane = a_plane + pixels;
    uint8_t *v_plane = u_plane + uv_pixels;

    simplewebp_error sw_err =
        simplewebp_decode_yuva(
            image,
            y_plane,
            u_plane,
            v_plane,
            a_plane,
            NULL
        );

    if (sw_err != SIMPLEWEBP_NO_ERROR) {
        heap_caps_free(planes);
        return webp_error_to_esp(sw_err);
    }

    int crop_x = 0;
    int crop_y = 0;
    int crop_w = 0;
    int crop_h = 0;

    compute_cover_crop(
        (int)width,
        (int)height,
        box_w,
        box_h,
        &crop_x,
        &crop_y,
        &crop_w,
        &crop_h
    );

    for (int dy = 0;
         dy < box_h;
         ++dy) {
        const int sy =
            crop_y +
            (int)(((int64_t)dy * crop_h) /
                  box_h);

        uint16_t *dst_row =
            dst +
            (size_t)(box_y + dy) *
            (size_t)dst_width +
            (size_t)box_x;

        for (int dx = 0;
             dx < box_w;
             ++dx) {
            const int sx =
                crop_x +
                (int)(((int64_t)dx * crop_w) /
                      box_w);

            const size_t y_index =
                (size_t)sy * width +
                (size_t)sx;

            const size_t uv_index =
                ((size_t)sy / 2U) *
                    uv_width +
                ((size_t)sx / 2U);

            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;

            yuv_to_rgb(
                y_plane[y_index],
                u_plane[uv_index],
                v_plane[uv_index],
                &r,
                &g,
                &b
            );

            dst_row[dx] =
                blend_rgb565(
                    r,
                    g,
                    b,
                    a_plane[y_index],
                    background
                );
        }

        if ((dy & 15) == 15) {
            taskYIELD();
        }
    }

    heap_caps_free(planes);
    return ESP_OK;
}

static esp_err_t render_lossless(
    simplewebp *image,
    size_t width,
    size_t height,
    uint16_t *dst,
    int dst_width,
    int box_x,
    int box_y,
    int box_w,
    int box_h,
    uint16_t background)
{
    if (width > SIZE_MAX / height) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t pixels =
        width * height;

    if (pixels > SIZE_MAX / 4U) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t rgba_bytes =
        pixels * 4U;

    uint8_t *rgba =
        (uint8_t *)heap_caps_malloc(
            rgba_bytes,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (!rgba) {
        return ESP_ERR_NO_MEM;
    }

    const simplewebp_error sw_err =
        simplewebp_decode(
            image,
            rgba,
            NULL
        );

    if (sw_err != SIMPLEWEBP_NO_ERROR) {
        heap_caps_free(rgba);
        return webp_error_to_esp(sw_err);
    }

    int crop_x = 0;
    int crop_y = 0;
    int crop_w = 0;
    int crop_h = 0;

    compute_cover_crop(
        (int)width,
        (int)height,
        box_w,
        box_h,
        &crop_x,
        &crop_y,
        &crop_w,
        &crop_h
    );

    for (int dy = 0;
         dy < box_h;
         ++dy) {
        const int sy =
            crop_y +
            (int)(((int64_t)dy * crop_h) /
                  box_h);

        uint16_t *dst_row =
            dst +
            (size_t)(box_y + dy) *
            (size_t)dst_width +
            (size_t)box_x;

        for (int dx = 0;
             dx < box_w;
             ++dx) {
            const int sx =
                crop_x +
                (int)(((int64_t)dx * crop_w) /
                      box_w);

            const size_t index =
                ((size_t)sy * width +
                 (size_t)sx) * 4U;

            dst_row[dx] =
                blend_rgb565(
                    rgba[index + 0U],
                    rgba[index + 1U],
                    rgba[index + 2U],
                    rgba[index + 3U],
                    background
                );
        }

        if ((dy & 15) == 15) {
            taskYIELD();
        }
    }

    heap_caps_free(rgba);
    return ESP_OK;
}

esp_err_t typen_webp_draw_preview(
    const char *path,
    uint16_t *dst,
    int dst_width,
    int dst_height,
    int box_x,
    int box_y,
    int box_w,
    int box_h,
    uint16_t background)
{
    if (!path ||
        !dst ||
        dst_width <= 0 ||
        dst_height <= 0 ||
        box_x < 0 ||
        box_y < 0 ||
        box_w <= 0 ||
        box_h <= 0 ||
        box_x > dst_width - box_w ||
        box_y > dst_height - box_h) {
        return ESP_ERR_INVALID_ARG;
    }

    bool animated = false;

    esp_err_t err =
        webp_check_static(
            path,
            &animated
        );

    if (err != ESP_OK) {
        return err;
    }

    if (animated) {
        ESP_LOGW(
            TAG,
            "Animated WebP is not supported: %s",
            path
        );
        return ESP_ERR_NOT_SUPPORTED;
    }

    const simplewebp_allocator allocator = {
        .alloc = webp_psram_alloc,
        .free = webp_psram_free,
        .userdata = NULL,
    };

    simplewebp *image = NULL;

    const simplewebp_error load_err =
        simplewebp_load_from_filename(
            path,
            &allocator,
            &image
        );

    if (load_err != SIMPLEWEBP_NO_ERROR ||
        !image) {
        ESP_LOGW(
            TAG,
            "WebP load failed for %s: %s",
            path,
            simplewebp_get_error_text(load_err)
        );

        return webp_error_to_esp(
            load_err
        );
    }

    size_t width = 0;
    size_t height = 0;

    simplewebp_get_dimensions(
        image,
        &width,
        &height
    );

    if (width == 0U ||
        height == 0U ||
        width > INT_MAX ||
        height > INT_MAX) {
        simplewebp_unload(image);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(
        TAG,
        "Decode static WebP: %s %ux%u %s",
        path,
        (unsigned)width,
        (unsigned)height,
        simplewebp_is_lossless(image)
            ? "lossless"
            : "lossy"
    );

    if (simplewebp_is_lossless(image)) {
        err =
            render_lossless(
                image,
                width,
                height,
                dst,
                dst_width,
                box_x,
                box_y,
                box_w,
                box_h,
                background
            );
    } else {
        err =
            render_lossy(
                image,
                width,
                height,
                dst,
                dst_width,
                box_x,
                box_y,
                box_w,
                box_h,
                background
            );
    }

    simplewebp_unload(image);
    return err;
}
