#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "driver/jpeg_decode.h"

#include "typen_jpeg.h"

#define MAX_JPEG_FILE_BYTES (16U * 1024U * 1024U)
#define MAX_JPEG_DIMENSION  4096U

static const char *TAG = "typen_jpeg";
static jpeg_decoder_handle_t s_decoder = NULL;

static uint32_t align16(uint32_t value)
{
    return (value + 15U) & ~15U;
}


static uint16_t read_u16(
    const uint8_t *p,
    bool little_endian)
{
    if (little_endian) {
        return (uint16_t)(
            (uint16_t)p[0] |
            ((uint16_t)p[1] << 8)
        );
    }

    return (uint16_t)(
        ((uint16_t)p[0] << 8) |
        (uint16_t)p[1]
    );
}

static uint32_t read_u32(
    const uint8_t *p,
    bool little_endian)
{
    if (little_endian) {
        return
            (uint32_t)p[0] |
            ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) |
            ((uint32_t)p[3] << 24);
    }

    return
        ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) |
        (uint32_t)p[3];
}

static uint8_t parse_exif_orientation(
    const uint8_t *data,
    size_t length)
{
    if (!data ||
        length < 4U ||
        data[0] != 0xFFU ||
        data[1] != 0xD8U) {
        return 1U;
    }

    size_t pos = 2U;

    while (pos + 4U <= length) {
        if (data[pos] != 0xFFU) {
            ++pos;
            continue;
        }

        while (pos < length &&
               data[pos] == 0xFFU) {
            ++pos;
        }

        if (pos >= length) {
            break;
        }

        const uint8_t marker =
            data[pos++];

        if (marker == 0xD8U ||
            marker == 0xD9U ||
            (marker >= 0xD0U &&
             marker <= 0xD7U) ||
            marker == 0x01U) {
            continue;
        }

        if (marker == 0xDAU) {
            break;
        }

        if (pos + 2U > length) {
            break;
        }

        const uint16_t segment_length =
            (uint16_t)(
                ((uint16_t)data[pos] << 8) |
                data[pos + 1U]
            );

        if (segment_length < 2U) {
            break;
        }

        const size_t payload_length =
            (size_t)segment_length - 2U;

        pos += 2U;

        if (payload_length >
            length - pos) {
            break;
        }

        if (marker == 0xE1U &&
            payload_length >= 14U &&
            memcmp(
                data + pos,
                "Exif\0\0",
                6U) == 0) {
            const uint8_t *tiff =
                data + pos + 6U;

            const size_t tiff_length =
                payload_length - 6U;

            if (tiff_length < 8U) {
                return 1U;
            }

            bool little_endian = false;

            if (tiff[0] == 'I' &&
                tiff[1] == 'I') {
                little_endian = true;
            } else if (tiff[0] == 'M' &&
                       tiff[1] == 'M') {
                little_endian = false;
            } else {
                return 1U;
            }

            if (read_u16(
                    tiff + 2U,
                    little_endian) != 0x002AU) {
                return 1U;
            }

            const uint32_t ifd0_offset =
                read_u32(
                    tiff + 4U,
                    little_endian
                );

            if (ifd0_offset >
                tiff_length - 2U) {
                return 1U;
            }

            const uint8_t *ifd =
                tiff + ifd0_offset;

            const size_t ifd_remaining =
                tiff_length - ifd0_offset;

            if (ifd_remaining < 2U) {
                return 1U;
            }

            const uint16_t entry_count =
                read_u16(
                    ifd,
                    little_endian
                );

            const uint8_t *entry =
                ifd + 2U;

            size_t remaining =
                ifd_remaining - 2U;

            for (uint16_t i = 0;
                 i < entry_count;
                 ++i) {
                if (remaining < 12U) {
                    break;
                }

                const uint16_t tag =
                    read_u16(
                        entry,
                        little_endian
                    );

                const uint16_t type =
                    read_u16(
                        entry + 2U,
                        little_endian
                    );

                const uint32_t count =
                    read_u32(
                        entry + 4U,
                        little_endian
                    );

                if (tag == 0x0112U &&
                    type == 3U &&
                    count >= 1U) {
                    const uint16_t orientation =
                        read_u16(
                            entry + 8U,
                            little_endian
                        );

                    if (orientation >= 1U &&
                        orientation <= 8U) {
                        return (uint8_t)orientation;
                    }

                    return 1U;
                }

                entry += 12U;
                remaining -= 12U;
            }

            return 1U;
        }

        pos += payload_length;
    }

    return 1U;
}

esp_err_t typen_jpeg_init(void)
{
    if (s_decoder != NULL) {
        return ESP_OK;
    }

    jpeg_decode_engine_cfg_t cfg = {
        .intr_priority = 0,
        .timeout_ms = 250,
    };

    return jpeg_new_decoder_engine(
        &cfg,
        &s_decoder
    );
}

esp_err_t typen_jpeg_decode_file(
    const char *path,
    typen_jpeg_frame_t *out)
{
    if (path == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    ESP_RETURN_ON_ERROR(
        typen_jpeg_init(),
        TAG,
        "JPEG engine init failed"
    );

    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    long file_len = ftell(file);

    if (file_len <= 0 ||
        (size_t)file_len > MAX_JPEG_FILE_BYTES) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    rewind(file);

    jpeg_decode_memory_alloc_cfg_t input_cfg = {
        .buffer_direction =
            JPEG_DEC_ALLOC_INPUT_BUFFER,
    };

    size_t input_alloc = 0;

    uint8_t *input = jpeg_alloc_decoder_mem(
        (size_t)file_len,
        &input_cfg,
        &input_alloc
    );

    if (input == NULL ||
        input_alloc < (size_t)file_len) {
        if (input != NULL) {
            free(input);
        }

        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    const size_t read_len =
        fread(input, 1, (size_t)file_len, file);

    fclose(file);

    if (read_len != (size_t)file_len) {
        free(input);
        return ESP_FAIL;
    }

    const uint8_t orientation =
        parse_exif_orientation(
            input,
            (size_t)file_len
        );

    jpeg_decode_picture_info_t info;

    esp_err_t err = jpeg_decoder_get_info(
        input,
        (uint32_t)file_len,
        &info
    );

    if (err != ESP_OK) {
        free(input);
        return err;
    }

    if (info.width == 0 ||
        info.height == 0 ||
        info.width > MAX_JPEG_DIMENSION ||
        info.height > MAX_JPEG_DIMENSION) {
        free(input);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t padded_w = align16(info.width);
    const uint32_t padded_h = align16(info.height);

    const size_t requested_out =
        (size_t)padded_w *
        (size_t)padded_h *
        2U;

    jpeg_decode_memory_alloc_cfg_t output_cfg = {
        .buffer_direction =
            JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };

    size_t output_alloc = 0;

    uint8_t *pixels = jpeg_alloc_decoder_mem(
        requested_out,
        &output_cfg,
        &output_alloc
    );

    if (pixels == NULL ||
        output_alloc < requested_out) {
        if (pixels != NULL) {
            free(pixels);
        }

        free(input);
        return ESP_ERR_NO_MEM;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format =
            JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order =
            JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std =
            JPEG_YUV_RGB_CONV_STD_BT601,
    };

    uint32_t output_size = 0;

    err = jpeg_decoder_process(
        s_decoder,
        &decode_cfg,
        input,
        (uint32_t)file_len,
        pixels,
        (uint32_t)output_alloc,
        &output_size
    );

    free(input);

    if (err != ESP_OK) {
        free(pixels);
        return err;
    }

    (void)output_size;

    out->pixels = pixels;
    out->pixels_allocated = output_alloc;
    out->width = (uint16_t)info.width;
    out->height = (uint16_t)info.height;
    out->stride_pixels = (uint16_t)padded_w;
    out->orientation = orientation;

    ESP_LOGI(
        TAG,
        "Decoded %s: %ux%u RGB565 exif-orientation=%u",
        path,
        (unsigned)out->width,
        (unsigned)out->height,
        (unsigned)out->orientation
    );

    return ESP_OK;
}

void typen_jpeg_free(
    typen_jpeg_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }

    if (frame->pixels != NULL) {
        free(frame->pixels);
    }

    memset(frame, 0, sizeof(*frame));
}
