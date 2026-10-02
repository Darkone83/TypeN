#include "typen_bmp.h"

#include <limits.h>
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

#define BMP_FILE_HEADER_SIZE        14U
#define BMP_INFO_HEADER_MIN_SIZE    40U
#define BMP_INFO_HEADER_READ_MAX   124U

#define BMP_BI_RGB                  0U
#define BMP_BI_BITFIELDS            3U
#define BMP_BI_ALPHABITFIELDS       6U

static const char *TAG = "typen_bmp";

typedef struct {
    int width;
    int height;
    bool top_down;
    uint16_t bpp;
    uint32_t compression;
    uint32_t pixel_offset;
    size_t row_bytes;

    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t alpha_mask;

    uint32_t palette_count;
    uint32_t *palette;
} bmp_info_t;

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(
        (uint16_t)p[0] |
        ((uint16_t)p[1] << 8)
    );
}

static uint32_t read_le32(const uint8_t *p)
{
    return
        (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static int32_t read_le32s(const uint8_t *p)
{
    return (int32_t)read_le32(p);
}

static bool file_read_exact(
    FILE *file,
    void *dst,
    size_t size)
{
    return file &&
           dst &&
           fread(dst, 1, size, file) == size;
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

static uint8_t mask_channel(
    uint32_t value,
    uint32_t mask,
    uint8_t fallback)
{
    if (mask == 0U) {
        return fallback;
    }

    unsigned shift = 0;

    while (((mask >> shift) & 1U) == 0U &&
           shift < 31U) {
        ++shift;
    }

    uint32_t normalized_mask =
        mask >> shift;

    unsigned bits = 0;

    while ((normalized_mask & 1U) != 0U) {
        ++bits;
        normalized_mask >>= 1;
    }

    if (bits == 0U) {
        return fallback;
    }

    const uint32_t raw =
        (value & mask) >> shift;

    const uint32_t maximum =
        bits >= 32U
            ? UINT32_MAX
            : ((UINT32_C(1) << bits) - 1U);

    if (maximum == 0U) {
        return fallback;
    }

    return (uint8_t)(
        (raw * 255U + maximum / 2U) /
        maximum
    );
}

static uint16_t blend_to_rgb565(
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

    const uint8_t out_r =
        (uint8_t)(
            ((unsigned)r * a +
             (unsigned)br * inv +
             127U) /
            255U
        );

    const uint8_t out_g =
        (uint8_t)(
            ((unsigned)g * a +
             (unsigned)bg * inv +
             127U) /
            255U
        );

    const uint8_t out_b =
        (uint8_t)(
            ((unsigned)b * a +
             (unsigned)bb * inv +
             127U) /
            255U
        );

    return rgb565(
        out_r,
        out_g,
        out_b
    );
}

static void bmp_info_free(
    bmp_info_t *info)
{
    if (!info) {
        return;
    }

    if (info->palette) {
        heap_caps_free(
            info->palette
        );

        info->palette = NULL;
    }
}

static esp_err_t bmp_read_info(
    FILE *file,
    bmp_info_t *info)
{
    if (!file || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(
        info,
        0,
        sizeof(*info)
    );

    uint8_t file_header[BMP_FILE_HEADER_SIZE];

    if (!file_read_exact(
            file,
            file_header,
            sizeof(file_header))) {
        return ESP_FAIL;
    }

    if (file_header[0] != 'B' ||
        file_header[1] != 'M') {
        return ESP_ERR_INVALID_ARG;
    }

    info->pixel_offset =
        read_le32(
            &file_header[10]
        );

    uint8_t dib_size_bytes[4];

    if (!file_read_exact(
            file,
            dib_size_bytes,
            sizeof(dib_size_bytes))) {
        return ESP_FAIL;
    }

    const uint32_t dib_size =
        read_le32(
            dib_size_bytes
        );

    if (dib_size < BMP_INFO_HEADER_MIN_SIZE) {
        ESP_LOGW(
            TAG,
            "Unsupported BMP DIB header: %u bytes",
            (unsigned)dib_size
        );
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t dib[BMP_INFO_HEADER_READ_MAX];
    memset(dib, 0, sizeof(dib));
    memcpy(dib, dib_size_bytes, sizeof(dib_size_bytes));

    const size_t copy_bytes =
        dib_size > sizeof(dib)
            ? sizeof(dib) - 4U
            : (size_t)dib_size - 4U;

    if (!file_read_exact(
            file,
            dib + 4,
            copy_bytes)) {
        return ESP_FAIL;
    }

    const int32_t width =
        read_le32s(
            &dib[4]
        );

    const int32_t raw_height =
        read_le32s(
            &dib[8]
        );

    const uint16_t planes =
        read_le16(
            &dib[12]
        );

    const uint16_t bpp =
        read_le16(
            &dib[14]
        );

    const uint32_t compression =
        read_le32(
            &dib[16]
        );

    const uint32_t colors_used =
        read_le32(
            &dib[32]
        );

    if (width <= 0 ||
        raw_height == 0 ||
        raw_height == INT32_MIN ||
        planes != 1U) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (bpp) {
        case 1:
        case 4:
        case 8:
        case 16:
        case 24:
        case 32:
            break;

        default:
            ESP_LOGW(
                TAG,
                "Unsupported BMP depth: %u bpp",
                (unsigned)bpp
            );
            return ESP_ERR_NOT_SUPPORTED;
    }

    if (compression != BMP_BI_RGB &&
        compression != BMP_BI_BITFIELDS &&
        compression != BMP_BI_ALPHABITFIELDS) {
        ESP_LOGW(
            TAG,
            "Unsupported BMP compression: %u",
            (unsigned)compression
        );
        return ESP_ERR_NOT_SUPPORTED;
    }

    if ((compression == BMP_BI_BITFIELDS ||
         compression == BMP_BI_ALPHABITFIELDS) &&
        bpp != 16U &&
        bpp != 32U) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    info->width = (int)width;
    info->height =
        raw_height < 0
            ? (int)(-raw_height)
            : (int)raw_height;

    info->top_down =
        raw_height < 0;

    info->bpp = bpp;
    info->compression = compression;

    const uint64_t row_bits =
        (uint64_t)(uint32_t)info->width *
        (uint64_t)bpp;

    const uint64_t row_bytes =
        ((row_bits + 31U) / 32U) * 4U;

    if (row_bytes == 0U ||
        row_bytes > SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    info->row_bytes =
        (size_t)row_bytes;

    if (bpp == 16U &&
        compression == BMP_BI_RGB) {
        info->red_mask = UINT32_C(0x7C00);
        info->green_mask = UINT32_C(0x03E0);
        info->blue_mask = UINT32_C(0x001F);
    } else if (bpp == 32U &&
               compression == BMP_BI_RGB) {
        info->red_mask = UINT32_C(0x00FF0000);
        info->green_mask = UINT32_C(0x0000FF00);
        info->blue_mask = UINT32_C(0x000000FF);
        info->alpha_mask = 0U;
    }

    size_t external_mask_bytes = 0U;

    if (compression == BMP_BI_BITFIELDS ||
        compression == BMP_BI_ALPHABITFIELDS) {
        if (dib_size >= 52U) {
            info->red_mask =
                read_le32(&dib[40]);
            info->green_mask =
                read_le32(&dib[44]);
            info->blue_mask =
                read_le32(&dib[48]);

            if (dib_size >= 56U) {
                info->alpha_mask =
                    read_le32(&dib[52]);
            }
        } else {
            const size_t mask_count =
                compression == BMP_BI_ALPHABITFIELDS
                    ? 4U
                    : 3U;

            uint8_t masks[16] = {0};

            if (fseek(
                    file,
                    (long)(BMP_FILE_HEADER_SIZE + dib_size),
                    SEEK_SET) != 0 ||
                !file_read_exact(
                    file,
                    masks,
                    mask_count * 4U)) {
                return ESP_FAIL;
            }

            info->red_mask =
                read_le32(&masks[0]);
            info->green_mask =
                read_le32(&masks[4]);
            info->blue_mask =
                read_le32(&masks[8]);

            if (mask_count == 4U) {
                info->alpha_mask =
                    read_le32(&masks[12]);
            }

            external_mask_bytes =
                mask_count * 4U;
        }

        if (info->red_mask == 0U ||
            info->green_mask == 0U ||
            info->blue_mask == 0U) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (bpp <= 8U) {
        uint32_t palette_count =
            colors_used;

        if (palette_count == 0U) {
            palette_count =
                UINT32_C(1) << bpp;
        }

        const uint32_t maximum_palette =
            UINT32_C(1) << bpp;

        if (palette_count == 0U ||
            palette_count > maximum_palette) {
            return ESP_ERR_INVALID_ARG;
        }

        const uint64_t palette_offset =
            (uint64_t)BMP_FILE_HEADER_SIZE +
            (uint64_t)dib_size +
            (uint64_t)external_mask_bytes;

        const uint64_t palette_bytes =
            (uint64_t)palette_count * 4U;

        if (palette_offset + palette_bytes >
            info->pixel_offset) {
            return ESP_ERR_INVALID_ARG;
        }

        info->palette =
            (uint32_t *)heap_caps_malloc(
                (size_t)palette_bytes,
                MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
            );

        if (!info->palette) {
            return ESP_ERR_NO_MEM;
        }

        if (palette_offset > LONG_MAX ||
            fseek(
                file,
                (long)palette_offset,
                SEEK_SET) != 0) {
            bmp_info_free(info);
            return ESP_FAIL;
        }

        uint8_t entry[4];

        for (uint32_t i = 0;
             i < palette_count;
             ++i) {
            if (!file_read_exact(
                    file,
                    entry,
                    sizeof(entry))) {
                bmp_info_free(info);
                return ESP_FAIL;
            }

            const uint8_t b = entry[0];
            const uint8_t g = entry[1];
            const uint8_t r = entry[2];

            info->palette[i] =
                ((uint32_t)r << 16) |
                ((uint32_t)g << 8) |
                (uint32_t)b;
        }

        info->palette_count =
            palette_count;
    }

    const uint64_t data_end =
        (uint64_t)info->pixel_offset +
        (uint64_t)info->row_bytes *
        (uint64_t)(uint32_t)info->height;

    if (data_end > LONG_MAX) {
        bmp_info_free(info);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static bool bmp_pixel_rgba(
    const bmp_info_t *info,
    const uint8_t *row,
    int x,
    uint8_t *r,
    uint8_t *g,
    uint8_t *b,
    uint8_t *a)
{
    if (!info ||
        !row ||
        x < 0 ||
        x >= info->width ||
        !r || !g || !b || !a) {
        return false;
    }

    *a = 255U;

    if (info->bpp <= 8U) {
        uint32_t index = 0U;

        if (info->bpp == 8U) {
            index = row[x];
        } else if (info->bpp == 4U) {
            const uint8_t packed =
                row[x >> 1];

            index =
                (x & 1)
                    ? (uint32_t)(packed & 0x0FU)
                    : (uint32_t)(packed >> 4);
        } else {
            const uint8_t packed =
                row[x >> 3];

            index =
                (uint32_t)(
                    (packed >> (7 - (x & 7))) &
                    1U
                );
        }

        if (index >= info->palette_count) {
            return false;
        }

        const uint32_t color =
            info->palette[index];

        *r = (uint8_t)((color >> 16) & 0xFFU);
        *g = (uint8_t)((color >> 8) & 0xFFU);
        *b = (uint8_t)(color & 0xFFU);
        return true;
    }

    if (info->bpp == 24U) {
        const uint8_t *p =
            row + (size_t)x * 3U;

        *b = p[0];
        *g = p[1];
        *r = p[2];
        return true;
    }

    uint32_t value = 0U;

    if (info->bpp == 16U) {
        value =
            read_le16(
                row + (size_t)x * 2U
            );
    } else if (info->bpp == 32U) {
        value =
            read_le32(
                row + (size_t)x * 4U
            );
    } else {
        return false;
    }

    *r = mask_channel(
        value,
        info->red_mask,
        0U
    );

    *g = mask_channel(
        value,
        info->green_mask,
        0U
    );

    *b = mask_channel(
        value,
        info->blue_mask,
        0U
    );

    *a = mask_channel(
        value,
        info->alpha_mask,
        255U
    );

    return true;
}

esp_err_t typen_bmp_draw_preview(
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

    FILE *file =
        fopen(
            path,
            "rb"
        );

    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    bmp_info_t info;

    esp_err_t err =
        bmp_read_info(
            file,
            &info
        );

    if (err != ESP_OK) {
        fclose(file);
        return err;
    }

    uint8_t *row =
        (uint8_t *)heap_caps_malloc(
            info.row_bytes,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (!row) {
        bmp_info_free(&info);
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    int crop_x = 0;
    int crop_y = 0;
    int crop_w = info.width;
    int crop_h = info.height;

    if ((int64_t)info.width * box_h >
        (int64_t)info.height * box_w) {
        crop_w =
            (int)(((int64_t)info.height *
                   box_w) /
                  box_h);

        if (crop_w < 1) {
            crop_w = 1;
        }

        crop_x =
            (info.width - crop_w) / 2;
    } else if ((int64_t)info.height * box_w >
               (int64_t)info.width * box_h) {
        crop_h =
            (int)(((int64_t)info.width *
                   box_h) /
                  box_w);

        if (crop_h < 1) {
            crop_h = 1;
        }

        crop_y =
            (info.height - crop_h) / 2;
    }

    int cached_file_row = -1;

    for (int dy = 0;
         dy < box_h;
         ++dy) {
        const int sy =
            crop_y +
            (int)(((int64_t)dy * crop_h) /
                  box_h);

        const int file_row =
            info.top_down
                ? sy
                : (info.height - 1 - sy);

        if (file_row != cached_file_row) {
            const uint64_t offset =
                (uint64_t)info.pixel_offset +
                (uint64_t)(uint32_t)file_row *
                (uint64_t)info.row_bytes;

            if (offset > LONG_MAX ||
                fseek(
                    file,
                    (long)offset,
                    SEEK_SET) != 0 ||
                !file_read_exact(
                    file,
                    row,
                    info.row_bytes)) {
                err = ESP_FAIL;
                break;
            }

            cached_file_row =
                file_row;
        }

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

            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            uint8_t a = 255U;

            if (!bmp_pixel_rgba(
                    &info,
                    row,
                    sx,
                    &r,
                    &g,
                    &b,
                    &a)) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }

            dst_row[dx] =
                blend_to_rgb565(
                    r,
                    g,
                    b,
                    a,
                    background
                );
        }

        if (err != ESP_OK) {
            break;
        }

        if ((dy & 15) == 15) {
            taskYIELD();
        }
    }

    heap_caps_free(row);
    bmp_info_free(&info);
    fclose(file);

    return err;
}
