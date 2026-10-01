/*
 * gifdec - small C99 GIF decoder
 * Public domain.
 * Upstream: https://github.com/lecram/gifdec
 */

#include "gifdec.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "esp_heap_caps.h"

#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define MAX(A, B) ((A) > (B) ? (A) : (B))

typedef struct Entry {
    uint16_t length;
    uint16_t prefix;
    uint8_t  suffix;
} Entry;

typedef struct Table {
    int bulk;
    int nentries;
    Entry *entries;
} Table;

static ssize_t gd_read(gd_GIF *gif, void *buffer, size_t length)
{
    if (!gif || !buffer) {
        return -1;
    }

    if (gif->from_memory) {
        if (gif->data_pos >= gif->data_size) {
            return 0;
        }

        size_t available =
            gif->data_size - gif->data_pos;

        if (length > available) {
            length = available;
        }

        memcpy(
            buffer,
            gif->data + gif->data_pos,
            length
        );

        gif->data_pos += length;
        return (ssize_t)length;
    }

    return read(
        gif->fd,
        buffer,
        length
    );
}

static off_t gd_seek(
    gd_GIF *gif,
    off_t offset,
    int whence)
{
    if (!gif) {
        return (off_t)-1;
    }

    if (!gif->from_memory) {
        return lseek(
        gif->fd,
        offset,
        whence
    );
    }

    int64_t base = 0;

    switch (whence) {
        case SEEK_SET:
            base = 0;
            break;

        case SEEK_CUR:
            base = (int64_t)gif->data_pos;
            break;

        case SEEK_END:
            base = (int64_t)gif->data_size;
            break;

        default:
            return (off_t)-1;
    }

    int64_t pos =
        base + (int64_t)offset;

    if (pos < 0) {
        pos = 0;
    }

    if ((uint64_t)pos >
        (uint64_t)gif->data_size) {
        pos = (int64_t)gif->data_size;
    }

    gif->data_pos =
        (size_t)pos;

    return (off_t)pos;
}

static uint16_t read_num(gd_GIF *gif)
{
    uint8_t bytes[2];

    if (gd_read(
            gif,
            bytes,
            2) != 2) {
        return 0;
    }

    return (uint16_t)(
        bytes[0] +
        (((uint16_t)bytes[1]) << 8)
    );
}

static gd_GIF *open_source(gd_GIF *gif)
{
    uint8_t sigver[3];
    uint16_t width, height, depth;
    uint8_t fdsz, bgidx, aspect;
    int i;
    uint8_t *bgcolor;
    int gct_sz;

    if (gd_read(gif, sigver, 3) != 3 ||
        memcmp(sigver, "GIF", 3) != 0) {
        return NULL;
    }

    if (gd_read(gif, sigver, 3) != 3 ||
        (memcmp(sigver, "89a", 3) != 0 &&
         memcmp(sigver, "87a", 3) != 0)) {
        return NULL;
    }

    width = read_num(gif);
    height = read_num(gif);

    if (width == 0 || height == 0) {
        return NULL;
    }

    if (gd_read(gif, &fdsz, 1) != 1) {
        return NULL;
    }

    if (!(fdsz & 0x80)) {
        return NULL;
    }

    depth = ((fdsz >> 4) & 7) + 1;
    gct_sz = 1 << ((fdsz & 0x07) + 1);

    if (gd_read(gif, &bgidx, 1) != 1 ||
        gd_read(gif, &aspect, 1) != 1) {
        return NULL;
    }

    (void)aspect;

    gif->width = width;
    gif->height = height;
    gif->depth = depth;
    gif->gct.size = gct_sz;

    if (gd_read(
            gif,
            gif->gct.colors,
            3 * gif->gct.size) !=
        3 * gif->gct.size) {
        return NULL;
    }

    gif->palette = &gif->gct;
    gif->bgindex = bgidx;

    if ((size_t)width >
            SIZE_MAX / (size_t)height ||
        (size_t)width *
            (size_t)height >
            SIZE_MAX / 4U) {
        return NULL;
    }

    /*
     * gifdec stores:
     *   width*height bytes indexed current frame
     *   width*height*3 bytes persistent RGB canvas
     *
     * Those are large, long-lived buffers and belong in PSRAM.
     */
    gif->frame =
        (uint8_t *)heap_caps_calloc(
            4U,
            (size_t)width *
                (size_t)height,
            MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
        );

    if (!gif->frame) {
        return NULL;
    }

    gif->canvas =
        &gif->frame[
            (size_t)width *
            (size_t)height
        ];

    if (gif->bgindex) {
        memset(
            gif->frame,
            gif->bgindex,
            (size_t)gif->width *
                (size_t)gif->height
        );
    }

    bgcolor =
        &gif->palette->colors[
            gif->bgindex * 3
        ];

    if (bgcolor[0] ||
        bgcolor[1] ||
        bgcolor[2]) {
        for (i = 0;
             i < gif->width *
                 gif->height;
             ++i) {
            memcpy(
                &gif->canvas[i * 3],
                bgcolor,
                3
            );
        }
    }

    gif->anim_start =
        gd_seek(
            gif,
            0,
            SEEK_CUR
        );

    return gif;
}

gd_GIF *gd_open_gif(const char *fname)
{
    if (!fname) {
        return NULL;
    }

    int fd =
        open(
            fname,
            O_RDONLY
        );

    if (fd == -1) {
        return NULL;
    }

    gd_GIF *gif =
        (gd_GIF *)heap_caps_calloc(
            1,
            sizeof(*gif),
            MALLOC_CAP_INTERNAL |
                MALLOC_CAP_8BIT
        );

    if (!gif) {
        close(fd);
        return NULL;
    }

    gif->fd = fd;
    gif->from_memory = 0;

    if (!open_source(gif)) {
        gd_close_gif(gif);
        return NULL;
    }

    return gif;
}

gd_GIF *gd_open_gif_mem(
    const uint8_t *data,
    size_t size)
{
    if (!data || size < 16U) {
        return NULL;
    }

    gd_GIF *gif =
        (gd_GIF *)heap_caps_calloc(
            1,
            sizeof(*gif),
            MALLOC_CAP_INTERNAL |
                MALLOC_CAP_8BIT
        );

    if (!gif) {
        return NULL;
    }

    gif->fd = -1;
    gif->from_memory = 1;
    gif->data = data;
    gif->data_size = size;
    gif->data_pos = 0;

    if (!open_source(gif)) {
        gd_close_gif(gif);
        return NULL;
    }

    return gif;
}

static void discard_sub_blocks(gd_GIF *gif)
{
    uint8_t size = 0;

    do {
        if (gd_read(gif, &size, 1) != 1) {
            return;
        }

        gd_seek(gif, size, SEEK_CUR);
    } while (size);
}

static void read_plain_text_ext(gd_GIF *gif)
{
    if (gif->plain_text) {
        uint16_t tx, ty, tw, th;
        uint8_t cw, ch, fg, bg;
        off_t sub_block;

        gd_seek(gif, 1, SEEK_CUR);
        tx = read_num(gif);
        ty = read_num(gif);
        tw = read_num(gif);
        th = read_num(gif);
        gd_read(gif, &cw, 1);
        gd_read(gif, &ch, 1);
        gd_read(gif, &fg, 1);
        gd_read(gif, &bg, 1);

        sub_block =
            gd_seek(gif, 0, SEEK_CUR);

        gif->plain_text(
            gif,
            tx, ty,
            tw, th,
            cw, ch,
            fg, bg
        );

        gd_seek(gif,
            sub_block,
            SEEK_SET
        );
    } else {
        gd_seek(gif, 13, SEEK_CUR);
    }

    discard_sub_blocks(gif);
}

static void read_graphic_control_ext(gd_GIF *gif)
{
    uint8_t rdit;

    gd_seek(gif, 1, SEEK_CUR);
    gd_read(gif, &rdit, 1);

    gif->gce.disposal =
        (rdit >> 2) & 3;

    gif->gce.input =
        rdit & 2;

    gif->gce.transparency =
        rdit & 1;

    gif->gce.delay =
        read_num(gif);

    gd_read(gif,
        &gif->gce.tindex,
        1
    );

    gd_seek(gif, 1, SEEK_CUR);
}

static void read_comment_ext(gd_GIF *gif)
{
    if (gif->comment) {
        off_t sub_block =
            gd_seek(gif, 0, SEEK_CUR);

        gif->comment(gif);

        gd_seek(gif,
            sub_block,
            SEEK_SET
        );
    }

    discard_sub_blocks(gif);
}

static void read_application_ext(gd_GIF *gif)
{
    char app_id[8];
    char app_auth_code[3];

    gd_seek(gif, 1, SEEK_CUR);

    gd_read(gif, app_id, 8);
    gd_read(gif, app_auth_code, 3);

    if (!strncmp(
            app_id,
            "NETSCAPE",
            sizeof(app_id))) {
        gd_seek(gif, 2, SEEK_CUR);

        gif->loop_count =
            read_num(gif);

        gd_seek(gif, 1, SEEK_CUR);
    } else if (gif->application) {
        off_t sub_block =
            gd_seek(gif, 0, SEEK_CUR);

        gif->application(
            gif,
            app_id,
            app_auth_code
        );

        gd_seek(gif,
            sub_block,
            SEEK_SET
        );

        discard_sub_blocks(gif);
    } else {
        discard_sub_blocks(gif);
    }
}

static void read_ext(gd_GIF *gif)
{
    uint8_t label;

    if (gd_read(gif, &label, 1) != 1) {
        return;
    }

    switch (label) {
        case 0x01:
            read_plain_text_ext(gif);
            break;

        case 0xF9:
            read_graphic_control_ext(gif);
            break;

        case 0xFE:
            read_comment_ext(gif);
            break;

        case 0xFF:
            read_application_ext(gif);
            break;

        default:
            discard_sub_blocks(gif);
            break;
    }
}

static Table *new_table(int key_size)
{
    int key;

    /*
     * GIF LZW dictionaries top out at 4096 entries. Allocate the whole table
     * once, internal-RAM first, instead of repeatedly reallocating it in
     * PSRAM. This is a small (~24 KiB) hot structure and materially improves
     * decode locality.
     */
    const int bulk = 0x1000;

    Table *table =
        (Table *)heap_caps_malloc_prefer(
            sizeof(*table) +
                sizeof(Entry) * bulk,
            2,
            MALLOC_CAP_INTERNAL |
                MALLOC_CAP_8BIT,
            MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
        );

    if (!table) {
        return NULL;
    }

    table->bulk = bulk;
    table->nentries =
        (1 << key_size) + 2;

    table->entries =
        (Entry *)&table[1];

    for (key = 0;
         key < (1 << key_size);
         ++key) {
        table->entries[key] =
            (Entry){
                1,
                0xFFF,
                (uint8_t)key
            };
    }

    return table;
}

static int add_entry(
    Table **tablep,
    uint16_t length,
    uint16_t prefix,
    uint8_t suffix)
{
    Table *table = *tablep;

    if (table->nentries >= table->bulk) {
        return -1;
    }

    table->entries[table->nentries] =
        (Entry){length, prefix, suffix};

    table->nentries++;

    if ((table->nentries &
         (table->nentries - 1)) == 0) {
        return 1;
    }

    return 0;
}

static uint16_t get_key(
    gd_GIF *gif,
    int key_size,
    uint8_t *sub_len,
    uint8_t *shift,
    uint8_t *byte)
{
    int bits_read;
    int rpad;
    int frag_size;
    uint16_t key = 0;

    for (bits_read = 0;
         bits_read < key_size;
         bits_read += frag_size) {

        rpad =
            (*shift + bits_read) % 8;

        if (rpad == 0) {
            if (*sub_len == 0) {
                if (gd_read(gif,
                        sub_len,
                        1) != 1) {
                    return 0x1000;
                }

                if (*sub_len == 0) {
                    return 0x1000;
                }
            }

            if (gd_read(gif,
                    byte,
                    1) != 1) {
                return 0x1000;
            }

            (*sub_len)--;
        }

        frag_size =
            MIN(
                key_size - bits_read,
                8 - rpad
            );

        key |=
            ((uint16_t)((*byte) >> rpad))
            << bits_read;
    }

    key &=
        (1 << key_size) - 1;

    *shift =
        (*shift + key_size) % 8;

    return key;
}

static int interlaced_line_index(
    int h,
    int y)
{
    int p;

    p = (h - 1) / 8 + 1;

    if (y < p) {
        return y * 8;
    }

    y -= p;
    p = (h - 5) / 8 + 1;

    if (y < p) {
        return y * 8 + 4;
    }

    y -= p;
    p = (h - 3) / 4 + 1;

    if (y < p) {
        return y * 4 + 2;
    }

    y -= p;

    return y * 2 + 1;
}

static int read_image_data(
    gd_GIF *gif,
    int interlace)
{
    uint8_t sub_len, shift, byte;
    int init_key_size;
    int key_size;
    int table_is_full;
    int frm_off;
    int frm_size;
    int str_len = 0;
    int i, p, x, y;
    uint16_t key, clear, stop;
    int ret;
    Table *table;
    Entry entry = {0};
    off_t start, end;

    if (gd_read(gif, &byte, 1) != 1) {
        return -1;
    }

    key_size = (int)byte;

    if (key_size < 2 || key_size > 8) {
        return -1;
    }

    start =
        gd_seek(gif, 0, SEEK_CUR);

    discard_sub_blocks(gif);

    end =
        gd_seek(gif, 0, SEEK_CUR);

    gd_seek(gif,
        start,
        SEEK_SET
    );

    clear = 1 << key_size;
    stop = clear + 1;

    table = new_table(key_size);

    if (!table) {
        return -1;
    }

    key_size++;
    init_key_size = key_size;
    sub_len = shift = 0;

    key = get_key(
        gif,
        key_size,
        &sub_len,
        &shift,
        &byte
    );

    frm_off = 0;
    ret = 0;
    table_is_full = 0;

    frm_size =
        gif->fw * gif->fh;

    while (frm_off < frm_size) {
        if (key == clear) {
            key_size = init_key_size;

            table->nentries =
                (1 << (key_size - 1)) + 2;

            table_is_full = 0;
        } else if (!table_is_full) {
            ret = add_entry(
                &table,
                (uint16_t)(str_len + 1),
                key,
                entry.suffix
            );

            if (ret == -1) {
                heap_caps_free(table);
                return -1;
            }

            if (table->nentries == 0x1000) {
                ret = 0;
                table_is_full = 1;
            }
        }

        key = get_key(
            gif,
            key_size,
            &sub_len,
            &shift,
            &byte
        );

        if (key == clear) {
            continue;
        }

        if (key == stop ||
            key == 0x1000) {
            break;
        }

        if (ret == 1) {
            key_size++;
        }

        if (key >= table->nentries) {
            heap_caps_free(table);
            return -1;
        }

        entry = table->entries[key];
        str_len = entry.length;

        for (i = 0; i < str_len; ++i) {
            p =
                frm_off +
                entry.length -
                1;

            x = p % gif->fw;
            y = p / gif->fw;

            if (interlace) {
                y = interlaced_line_index(
                    (int)gif->fh,
                    y
                );
            }

            gif->frame[
                (gif->fy + y) *
                gif->width +
                gif->fx +
                x
            ] = entry.suffix;

            if (entry.prefix == 0xFFF) {
                break;
            }

            if (entry.prefix >=
                table->nentries) {
                heap_caps_free(table);
                return -1;
            }

            entry =
                table->entries[
                    entry.prefix
                ];
        }

        frm_off += str_len;

        if (key <
                table->nentries - 1 &&
            !table_is_full) {
            table->entries[
                table->nentries - 1
            ].suffix =
                entry.suffix;
        }
    }

    heap_caps_free(table);

    if (key == stop) {
        gd_read(gif, &sub_len, 1);
    }

    gd_seek(gif,
        end,
        SEEK_SET
    );

    return 0;
}

static int read_image(gd_GIF *gif)
{
    uint8_t fisrz;
    int interlace;

    gif->fx = read_num(gif);
    gif->fy = read_num(gif);

    if (gif->fx >= gif->width ||
        gif->fy >= gif->height) {
        return -1;
    }

    gif->fw = read_num(gif);
    gif->fh = read_num(gif);

    gif->fw =
        MIN(
            gif->fw,
            gif->width - gif->fx
        );

    gif->fh =
        MIN(
            gif->fh,
            gif->height - gif->fy
        );

    if (gd_read(gif, &fisrz, 1) != 1) {
        return -1;
    }

    interlace = fisrz & 0x40;

    if (fisrz & 0x80) {
        gif->lct.size =
            1 << ((fisrz & 0x07) + 1);

        if (gd_read(gif,
                gif->lct.colors,
                3 * gif->lct.size) !=
            3 * gif->lct.size) {
            return -1;
        }

        gif->palette =
            &gif->lct;
    } else {
        gif->palette =
            &gif->gct;
    }

    return read_image_data(
        gif,
        interlace
    );
}

static void render_frame_rect(
    gd_GIF *gif,
    uint8_t *buffer)
{
    int i, j, k;
    uint8_t index;
    uint8_t *color;

    i =
        gif->fy *
        gif->width +
        gif->fx;

    for (j = 0;
         j < gif->fh;
         ++j) {
        for (k = 0;
             k < gif->fw;
             ++k) {
            index =
                gif->frame[
                    (gif->fy + j) *
                    gif->width +
                    gif->fx +
                    k
                ];

            color =
                &gif->palette->colors[
                    index * 3
                ];

            if (!gif->gce.transparency ||
                index != gif->gce.tindex) {
                memcpy(
                    &buffer[(i + k) * 3],
                    color,
                    3
                );
            }
        }

        i += gif->width;
    }
}

static void dispose(gd_GIF *gif)
{
    int i, j, k;
    uint8_t *bgcolor;

    switch (gif->gce.disposal) {
        case 2:
            bgcolor =
                &gif->palette->colors[
                    gif->bgindex * 3
                ];

            i =
                gif->fy *
                gif->width +
                gif->fx;

            for (j = 0;
                 j < gif->fh;
                 ++j) {
                for (k = 0;
                     k < gif->fw;
                     ++k) {
                    memcpy(
                        &gif->canvas[
                            (i + k) * 3
                        ],
                        bgcolor,
                        3
                    );
                }

                i += gif->width;
            }
            break;

        case 3:
            break;

        default:
            render_frame_rect(
                gif,
                gif->canvas
            );
            break;
    }
}

int gd_get_frame(gd_GIF *gif)
{
    char sep;

    dispose(gif);

    if (gd_read(gif,
            &sep,
            1) != 1) {
        return -1;
    }

    while (sep != ',') {
        if (sep == ';') {
            return 0;
        }

        if (sep == '!') {
            read_ext(gif);
        } else {
            return -1;
        }

        if (gd_read(gif,
                &sep,
                1) != 1) {
            return -1;
        }
    }

    if (read_image(gif) == -1) {
        return -1;
    }

    return 1;
}

void gd_render_frame(
    gd_GIF *gif,
    uint8_t *buffer)
{
    memcpy(
        buffer,
        gif->canvas,
        (size_t)gif->width *
        (size_t)gif->height *
        3U
    );

    render_frame_rect(
        gif,
        buffer
    );
}

int gd_is_bgcolor(
    gd_GIF *gif,
    uint8_t color[3])
{
    return !memcmp(
        &gif->palette->colors[
            gif->bgindex * 3
        ],
        color,
        3
    );
}

void gd_rewind(gd_GIF *gif)
{
    if (!gif) {
        return;
    }

    const size_t pixels =
        (size_t)gif->width *
        (size_t)gif->height;

    if (gif->frame) {
        memset(
            gif->frame,
            gif->bgindex,
            pixels
        );
    }

    if (gif->canvas) {
        const uint8_t *bg =
            &gif->gct.colors[
                gif->bgindex * 3
            ];

        for (size_t i = 0;
             i < pixels;
             ++i) {
            gif->canvas[i * 3 + 0] = bg[0];
            gif->canvas[i * 3 + 1] = bg[1];
            gif->canvas[i * 3 + 2] = bg[2];
        }
    }

    memset(
        &gif->gce,
        0,
        sizeof(gif->gce)
    );

    gif->palette = &gif->gct;
    gif->fx = 0;
    gif->fy = 0;
    gif->fw = 0;
    gif->fh = 0;

    gd_seek(
        gif,
        gif->anim_start,
        SEEK_SET
    );
}

void gd_close_gif(gd_GIF *gif)
{
    if (!gif) {
        return;
    }

    if (!gif->from_memory &&
        gif->fd >= 0) {
        close(gif->fd);
    }

    if (gif->frame) {
        heap_caps_free(
            gif->frame
        );
    }

    heap_caps_free(gif);
}
