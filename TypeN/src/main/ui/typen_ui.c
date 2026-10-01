#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "typen_board.h"
#include "typen_media.h"
#include "typen_playback.h"
#include "typen_settings.h"
#include "typen_thumb_cache.h"
#include "typen_ui.h"

#define SAFE_X                  125
#define SAFE_Y                  125
#define SAFE_W                  550
#define SAFE_H                  550

#define TOUCH_POLL_MS            20
#define LONG_PRESS_MS           700
#define TAP_MOVE_LIMIT           24

#define C_BLACK             0x0000
#define C_BG                0x0020
#define C_PANEL             0x0861
#define C_PANEL_2           0x10A2
#define C_PANEL_PRESSED     0x1983
#define C_LINE              0x29A5
#define C_ACCENT            0x3FE8
#define C_ACCENT_DIM        0x1C63
#define C_TEXT              0xF7DE
#define C_TEXT_DIM          0x9CF3
#define C_WARN              0xFCA0

/* About-page brand accents */
#define C_D83_PURPLE        0xB81F
#define C_D83_PURPLE_DIM    0x580F
#define C_D83_PINK          0xF81F
#define C_MNP_ORANGE        0xFD20
#define C_MNP_YELLOW        0xFFE0
#define C_MNP_RED           0xF980

#define MEDIA_PER_PAGE            4
#define MEDIA_CARD_W            218
#define MEDIA_CARD_H            132
#define MEDIA_THUMB_W           196
#define MEDIA_THUMB_H            88

typedef enum {
    PAGE_HOME = 0,
    PAGE_DISPLAY,
    PAGE_MEDIA,
    PAGE_SYSTEM,
    PAGE_ABOUT,
} typen_page_t;

static const char *TAG = "typen_ui";

static uint16_t *s_fb = NULL;
static TaskHandle_t s_touch_task = NULL;

static bool s_menu_visible = false;
static typen_page_t s_page = PAGE_HOME;

static uint8_t s_brightness = TYPEN_DEFAULT_BRIGHTNESS;
static size_t s_media_count = 0;
static size_t s_media_page_index = 0;
static bool s_media_confirm_visible = false;
static size_t s_media_confirm_index = 0;

static char s_status[48] = "STARTING";
static char s_wifi[48] = "GATED";

static void draw_shell(const char *title, const char *subtitle);

/* -------------------------------------------------------------------------- */
/* Tiny direct framebuffer renderer                                            */
/* -------------------------------------------------------------------------- */

static inline void px(int x, int y, uint16_t c)
{
    if ((unsigned)x < TYPEN_LCD_W &&
        (unsigned)y < TYPEN_LCD_H &&
        s_fb != NULL) {
        s_fb[y * TYPEN_LCD_W + x] = c;
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    if (s_fb == NULL || w <= 0 || h <= 0) {
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

    if (x + w > TYPEN_LCD_W) {
        w = TYPEN_LCD_W - x;
    }

    if (y + h > TYPEN_LCD_H) {
        h = TYPEN_LCD_H - y;
    }

    if (w <= 0 || h <= 0) {
        return;
    }

    for (int yy = y; yy < y + h; ++yy) {
        uint16_t *row = &s_fb[yy * TYPEN_LCD_W + x];

        for (int xx = 0; xx < w; ++xx) {
            row[xx] = c;
        }
    }
}

static int corner_inset(int radius, int dy)
{
    int inset = 0;
    const int rr = radius * radius;

    while (inset < radius) {
        int dx = radius - inset - 1;

        if ((dx * dx + dy * dy) <= rr) {
            break;
        }

        ++inset;
    }

    return inset;
}

static void fill_round_rect(
    int x,
    int y,
    int w,
    int h,
    int radius,
    uint16_t color)
{
    if (radius < 1) {
        fill_rect(x, y, w, h, color);
        return;
    }

    if (radius * 2 > w) {
        radius = w / 2;
    }

    if (radius * 2 > h) {
        radius = h / 2;
    }

    for (int yy = 0; yy < h; ++yy) {
        int inset = 0;

        if (yy < radius) {
            int dy = radius - yy - 1;
            inset = corner_inset(radius, dy);
        } else if (yy >= h - radius) {
            int dy = yy - (h - radius);
            inset = corner_inset(radius, dy);
        }

        fill_rect(
            x + inset,
            y + yy,
            w - inset * 2,
            1,
            color
        );
    }
}

static void stroke_round_rect(
    int x,
    int y,
    int w,
    int h,
    int radius,
    int thickness,
    uint16_t color,
    uint16_t inside)
{
    fill_round_rect(x, y, w, h, radius, color);

    if (thickness > 0 &&
        w > thickness * 2 &&
        h > thickness * 2) {
        fill_round_rect(
            x + thickness,
            y + thickness,
            w - thickness * 2,
            h - thickness * 2,
            radius > thickness ? radius - thickness : 1,
            inside
        );
    }
}

static const uint8_t *glyph_rows(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }

    switch (c) {
        case ' ': { static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; return g; }
        case 'A': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}; return g; }
        case 'B': { static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}; return g; }
        case 'C': { static const uint8_t g[7] = {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F}; return g; }
        case 'D': { static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}; return g; }
        case 'E': { static const uint8_t g[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}; return g; }
        case 'F': { static const uint8_t g[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}; return g; }
        case 'G': { static const uint8_t g[7] = {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0E}; return g; }
        case 'H': { static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}; return g; }
        case 'I': { static const uint8_t g[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}; return g; }
        case 'J': { static const uint8_t g[7] = {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C}; return g; }
        case 'K': { static const uint8_t g[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}; return g; }
        case 'L': { static const uint8_t g[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}; return g; }
        case 'M': { static const uint8_t g[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}; return g; }
        case 'N': { static const uint8_t g[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}; return g; }
        case 'O': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}; return g; }
        case 'P': { static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}; return g; }
        case 'Q': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}; return g; }
        case 'R': { static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}; return g; }
        case 'S': { static const uint8_t g[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}; return g; }
        case 'T': { static const uint8_t g[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}; return g; }
        case 'U': { static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}; return g; }
        case 'V': { static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}; return g; }
        case 'W': { static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}; return g; }
        case 'X': { static const uint8_t g[7] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}; return g; }
        case 'Y': { static const uint8_t g[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}; return g; }
        case 'Z': { static const uint8_t g[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}; return g; }
        case '0': { static const uint8_t g[7] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}; return g; }
        case '1': { static const uint8_t g[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}; return g; }
        case '2': { static const uint8_t g[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}; return g; }
        case '3': { static const uint8_t g[7] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}; return g; }
        case '4': { static const uint8_t g[7] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}; return g; }
        case '5': { static const uint8_t g[7] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}; return g; }
        case '6': { static const uint8_t g[7] = {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}; return g; }
        case '7': { static const uint8_t g[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}; return g; }
        case '8': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}; return g; }
        case '9': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E}; return g; }
        case '%': { static const uint8_t g[7] = {0x19, 0x1A, 0x04, 0x08, 0x16, 0x06, 0x00}; return g; }
        case '-': { static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}; return g; }
        case '/': { static const uint8_t g[7] = {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}; return g; }
        case '+': { static const uint8_t g[7] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}; return g; }
        case '<': { static const uint8_t g[7] = {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}; return g; }
        case '>': { static const uint8_t g[7] = {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}; return g; }
        case ':': { static const uint8_t g[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}; return g; }
        case '.': { static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06}; return g; }
        case '_': { static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}; return g; }
        default: {
            static const uint8_t q[7] = {
                0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04
            };
            return q;
        }
    }
}

static int text_width(const char *text, int scale)
{
    if (text == NULL || *text == 0) {
        return 0;
    }

    return (int)strlen(text) * 6 * scale - scale;
}

static void draw_char(
    int x,
    int y,
    char c,
    int scale,
    uint16_t color)
{
    const uint8_t *rows = glyph_rows(c);

    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (rows[row] & (1U << (4 - col))) {
                fill_rect(
                    x + col * scale,
                    y + row * scale,
                    scale,
                    scale,
                    color
                );
            }
        }
    }
}

static void draw_text(
    int x,
    int y,
    const char *text,
    int scale,
    uint16_t color)
{
    if (text == NULL) {
        return;
    }

    while (*text) {
        draw_char(x, y, *text, scale, color);
        x += 6 * scale;
        ++text;
    }
}

static void draw_text_center(
    int cx,
    int y,
    const char *text,
    int scale,
    uint16_t color)
{
    draw_text(
        cx - text_width(text, scale) / 2,
        y,
        text,
        scale,
        color
    );
}

static void draw_text_right(
    int right,
    int y,
    const char *text,
    int scale,
    uint16_t color)
{
    draw_text(
        right - text_width(text, scale),
        y,
        text,
        scale,
        color
    );
}

static void clear_screen(void)
{
    fill_rect(0, 0, TYPEN_LCD_W, TYPEN_LCD_H, C_BLACK);
}

static void present(void)
{
    esp_err_t err = typen_board_present();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "present failed: %s", esp_err_to_name(err));
    }
}


static void draw_text_ellipsis(
    int x,
    int y,
    const char *text,
    int scale,
    uint16_t color,
    int max_chars)
{
    if (text == NULL || max_chars < 4) {
        return;
    }

    char buf[64];
    size_t len = strlen(text);
    size_t cap = (size_t)max_chars;

    if (cap >= sizeof(buf)) {
        cap = sizeof(buf) - 1U;
    }

    if (len <= cap) {
        memcpy(buf, text, len + 1U);
    } else {
        size_t keep = cap - 3U;
        memcpy(buf, text, keep);
        buf[keep + 0U] = '.';
        buf[keep + 1U] = '.';
        buf[keep + 2U] = '.';
        buf[keep + 3U] = '\0';
    }

    draw_text(x, y, buf, scale, color);
}

static void draw_thumb_placeholder(
    int x,
    int y,
    int w,
    int h,
    typen_media_type_t type)
{
    fill_round_rect(x, y, w, h, 10, C_BG);

    fill_rect(
        x + 1,
        y + h - 24,
        w - 2,
        23,
        C_PANEL_PRESSED
    );

    const char *label =
        typen_media_type_name(type);

    draw_text_center(
        x + w / 2,
        y + h / 2 - 11,
        label,
        3,
        C_ACCENT
    );

    draw_text_center(
        x + w / 2,
        y + h - 17,
        "PREVIEW LATER",
        1,
        C_TEXT_DIM
    );
}

static void draw_cached_thumb_pixels(
    const uint16_t *pixels,
    int x,
    int y,
    int w,
    int h)
{
    if (!pixels) {
        return;
    }

    fill_rect(x, y, w, h, C_BLACK);

    int draw_w = w;
    int draw_h =
        (int)(((int64_t)TYPEN_THUMB_H * draw_w) /
              TYPEN_THUMB_W);

    if (draw_h > h) {
        draw_h = h;
        draw_w =
            (int)(((int64_t)TYPEN_THUMB_W * draw_h) /
                  TYPEN_THUMB_H);
    }

    if (draw_w < 1) draw_w = 1;
    if (draw_h < 1) draw_h = 1;

    const int offset_x = x + (w - draw_w) / 2;
    const int offset_y = y + (h - draw_h) / 2;

    for (int dy = 0; dy < draw_h; ++dy) {
        const int sy =
            (int)(((int64_t)dy * TYPEN_THUMB_H) /
                  draw_h);

        for (int dx = 0; dx < draw_w; ++dx) {
            const int sx =
                (int)(((int64_t)dx * TYPEN_THUMB_W) /
                      draw_w);

            px(
                offset_x + dx,
                offset_y + dy,
                pixels[
                    sy * TYPEN_THUMB_W + sx
                ]
            );
        }
    }
}

static void draw_media_thumbnail(
    size_t media_index,
    const typen_media_item_t *item,
    int x,
    int y,
    int w,
    int h)
{
    if (item == NULL) {
        return;
    }

    const uint16_t *pixels = NULL;

    const typen_thumb_state_t state =
        typen_thumb_cache_get(
            media_index,
            &pixels
        );

    if (state == TYPEN_THUMB_READY &&
        pixels != NULL) {
        draw_cached_thumb_pixels(
            pixels,
            x,
            y,
            w,
            h
        );
        return;
    }

    if (state == TYPEN_THUMB_LOADING ||
        state == TYPEN_THUMB_MISS) {
        fill_round_rect(
            x,
            y,
            w,
            h,
            10,
            C_BG
        );

        draw_text_center(
            x + w / 2,
            y + h / 2 - 10,
            "LOADING",
            2,
            C_ACCENT
        );

        return;
    }

    draw_thumb_placeholder(
        x,
        y,
        w,
        h,
        item->type
    );
}

static size_t media_page_count(void)
{
    if (s_media_count == 0) {
        return 1;
    }

    return
        (s_media_count +
         MEDIA_PER_PAGE - 1U) /
        MEDIA_PER_PAGE;
}

static void draw_media_card(
    int x,
    int y,
    size_t index)
{
    typen_media_item_t item;

    if (typen_media_get(index, &item) != ESP_OK) {
        return;
    }

    stroke_round_rect(
        x,
        y,
        MEDIA_CARD_W,
        MEDIA_CARD_H,
        14,
        1,
        C_LINE,
        C_PANEL_2
    );

    draw_media_thumbnail(
        index,
        &item,
        x + 11,
        y + 9,
        MEDIA_THUMB_W,
        MEDIA_THUMB_H
    );

    draw_text_ellipsis(
        x + 12,
        y + 106,
        item.name,
        1,
        C_TEXT,
        31
    );
}

static void draw_media_confirmation(void)
{
    typen_media_item_t item;

    if (typen_media_get(
            s_media_confirm_index,
            &item) != ESP_OK) {
        s_media_confirm_visible = false;
        return;
    }

    draw_shell("SET MEDIA?", typen_media_type_name(item.type));

    draw_media_thumbnail(
        s_media_confirm_index,
        &item,
        SAFE_X + 110,
        SAFE_Y + 150,
        330,
        190
    );

    draw_text_ellipsis(
        SAFE_X + 76,
        SAFE_Y + 365,
        item.name,
        2,
        C_TEXT,
        31
    );

    stroke_round_rect(
        SAFE_X + 86,
        SAFE_Y + 430,
        150,
        50,
        14,
        1,
        C_LINE,
        C_PANEL_2
    );

    draw_text_center(
        SAFE_X + 161,
        SAFE_Y + 447,
        "CANCEL",
        2,
        C_TEXT_DIM
    );

    stroke_round_rect(
        SAFE_X + 314,
        SAFE_Y + 430,
        150,
        50,
        14,
        1,
        C_ACCENT_DIM,
        C_PANEL_PRESSED
    );

    draw_text_center(
        SAFE_X + 389,
        SAFE_Y + 447,
        "SET",
        2,
        C_ACCENT
    );

    present();
}

/* -------------------------------------------------------------------------- */
/* Modern TypeN chrome                                                         */
/* -------------------------------------------------------------------------- */

static void draw_shell(const char *title, const char *subtitle)
{
    clear_screen();

    /*
     * 550x550 safe-area surface. This is intentionally quieter and cleaner
     * than Type-D-XL: dark neutral surface, thin accent rail, sparse text.
     */
    fill_round_rect(
        SAFE_X,
        SAFE_Y,
        SAFE_W,
        SAFE_H,
        28,
        C_PANEL
    );

    stroke_round_rect(
        SAFE_X + 1,
        SAFE_Y + 1,
        SAFE_W - 2,
        SAFE_H - 2,
        27,
        2,
        C_LINE,
        C_PANEL
    );

    fill_round_rect(
        SAFE_X + 34,
        SAFE_Y + 34,
        5,
        70,
        2,
        C_ACCENT
    );

    draw_text(
        SAFE_X + 58,
        SAFE_Y + 40,
        title,
        4,
        C_TEXT
    );

    draw_text(
        SAFE_X + 58,
        SAFE_Y + 78,
        subtitle,
        2,
        C_TEXT_DIM
    );

    fill_rect(
        SAFE_X + 34,
        SAFE_Y + 122,
        SAFE_W - 68,
        1,
        C_LINE
    );
}

static void draw_back_button(void)
{
    stroke_round_rect(
        SAFE_X + 34,
        SAFE_Y + SAFE_H - 66,
        114,
        38,
        12,
        1,
        C_LINE,
        C_PANEL_2
    );

    draw_text_center(
        SAFE_X + 91,
        SAFE_Y + SAFE_H - 55,
        "< BACK",
        2,
        C_TEXT_DIM
    );
}

static void draw_menu_row(
    int y,
    const char *title,
    const char *subtitle,
    bool enabled)
{
    const int x = SAFE_X + 48;
    const int w = SAFE_W - 96;
    const int h = 66;

    stroke_round_rect(
        x,
        y,
        w,
        h,
        15,
        1,
        enabled ? C_LINE : C_PANEL_2,
        C_PANEL_2
    );

    fill_round_rect(
        x + 14,
        y + 13,
        4,
        h - 26,
        2,
        enabled ? C_ACCENT_DIM : C_LINE
    );

    draw_text(
        x + 34,
        y + 12,
        title,
        3,
        enabled ? C_TEXT : C_TEXT_DIM
    );

    draw_text(
        x + 34,
        y + 40,
        subtitle,
        1,
        C_TEXT_DIM
    );

    if (enabled) {
        draw_text_right(
            x + w - 20,
            y + 24,
            ">",
            2,
            C_ACCENT
        );
    }
}

static void draw_idle(void)
{
    clear_screen();

    /*
     * Placeholder idle state until media rendering lands.
     * No active menu tree and no periodic redraw.
     */
    stroke_round_rect(
        315,
        300,
        170,
        170,
        85,
        2,
        C_LINE,
        C_BG
    );

    fill_round_rect(
        392,
        324,
        16,
        74,
        8,
        C_ACCENT
    );

    draw_text_center(
        400,
        505,
        "TYPE N",
        4,
        C_TEXT
    );

    draw_text_center(
        400,
        550,
        "HOLD FOR MENU",
        2,
        C_TEXT_DIM
    );

    present();
}

static void draw_home(void)
{
    s_page = PAGE_HOME;

    draw_shell("TYPE N", "CONTROL CENTER");

    draw_menu_row(
        SAFE_Y + 146,
        "DISPLAY",
        "BRIGHTNESS",
        true
    );

    draw_menu_row(
        SAFE_Y + 220,
        "MEDIA",
        "BROWSER NEXT",
        true
    );

    draw_menu_row(
        SAFE_Y + 294,
        "SYSTEM",
        "STORAGE + WI-FI",
        true
    );

    draw_menu_row(
        SAFE_Y + 368,
        "ABOUT",
        "CREDITS + INFO",
        true
    );

    stroke_round_rect(
        SAFE_X + 196,
        SAFE_Y + 462,
        158,
        44,
        14,
        1,
        C_LINE,
        C_PANEL_2
    );

    draw_text_center(
        400,
        SAFE_Y + 476,
        "CLOSE",
        2,
        C_TEXT_DIM
    );

    present();
}

static void draw_display_page(void)
{
    s_page = PAGE_DISPLAY;

    draw_shell("DISPLAY", "BRIGHTNESS");

    char value[16];
    snprintf(value, sizeof(value), "%u%%", s_brightness);

    draw_text_center(
        400,
        SAFE_Y + 190,
        value,
        6,
        C_TEXT
    );

    const int track_x = SAFE_X + 92;
    const int track_y = SAFE_Y + 300;
    const int track_w = SAFE_W - 184;
    const int track_h = 16;

    fill_round_rect(
        track_x,
        track_y,
        track_w,
        track_h,
        8,
        C_LINE
    );

    int fill_w =
        (track_w * (int)s_brightness) / 100;

    if (fill_w < 12) {
        fill_w = 12;
    }

    fill_round_rect(
        track_x,
        track_y,
        fill_w,
        track_h,
        8,
        C_ACCENT
    );

    int knob_x = track_x + fill_w - 10;

    fill_round_rect(
        knob_x,
        track_y - 8,
        20,
        32,
        10,
        C_TEXT
    );

    draw_text_center(
        400,
        SAFE_Y + 350,
        "TAP BAR TO ADJUST",
        2,
        C_TEXT_DIM
    );

    draw_back_button();

    present();
}

static void draw_media_page(void)
{
    s_page = PAGE_MEDIA;
    s_media_confirm_visible = false;

    const size_t pages = media_page_count();

    if (s_media_page_index >= pages) {
        s_media_page_index = pages - 1U;
    }

    draw_shell("MEDIA", "SD LIBRARY");

    if (s_media_count == 0) {
        draw_text_center(
            400,
            SAFE_Y + 248,
            "NO MEDIA FOUND",
            3,
            C_TEXT
        );

        draw_text_center(
            400,
            SAFE_Y + 292,
            "SCANNED ENTIRE SD CARD",
            2,
            C_TEXT_DIM
        );

        draw_back_button();
        present();
        return;
    }

    const int left_x = SAFE_X + 48;
    const int right_x =
        left_x + MEDIA_CARD_W + 16;

    const int top_y = SAFE_Y + 142;
    const int bottom_y =
        top_y + MEDIA_CARD_H + 14;

    const size_t first =
        s_media_page_index * MEDIA_PER_PAGE;

    const int card_x[MEDIA_PER_PAGE] = {
        left_x,
        right_x,
        left_x,
        right_x
    };

    const int card_y[MEDIA_PER_PAGE] = {
        top_y,
        top_y,
        bottom_y,
        bottom_y
    };

    for (size_t slot = 0;
         slot < MEDIA_PER_PAGE;
         ++slot) {
        const size_t index = first + slot;

        if (index >= s_media_count) {
            break;
        }

        draw_media_card(
            card_x[slot],
            card_y[slot],
            index
        );
    }

    draw_back_button();

    if (s_media_page_index > 0) {
        stroke_round_rect(
            SAFE_X + 188,
            SAFE_Y + SAFE_H - 66,
            58,
            38,
            12,
            1,
            C_LINE,
            C_PANEL_2
        );

        draw_text_center(
            SAFE_X + 217,
            SAFE_Y + SAFE_H - 55,
            "<",
            2,
            C_TEXT
        );
    }

    if (s_media_page_index + 1U < pages) {
        stroke_round_rect(
            SAFE_X + SAFE_W - 92,
            SAFE_Y + SAFE_H - 66,
            58,
            38,
            12,
            1,
            C_LINE,
            C_PANEL_2
        );

        draw_text_center(
            SAFE_X + SAFE_W - 63,
            SAFE_Y + SAFE_H - 55,
            ">",
            2,
            C_TEXT
        );
    }

    char page_text[32];
    snprintf(
        page_text,
        sizeof(page_text),
        "%u/%u",
        (unsigned)(s_media_page_index + 1U),
        (unsigned)pages
    );

    draw_text_center(
        400,
        SAFE_Y + SAFE_H - 55,
        page_text,
        2,
        C_TEXT_DIM
    );

    present();
}

static void draw_system_page(void)
{
    s_page = PAGE_SYSTEM;

    draw_shell("SYSTEM", "LOCAL STATUS");

    draw_text(
        SAFE_X + 64,
        SAFE_Y + 166,
        "STORAGE",
        2,
        C_TEXT_DIM
    );

    draw_text_right(
        SAFE_X + SAFE_W - 64,
        SAFE_Y + 166,
        strcmp(s_status, "READY") == 0 ? "READY" : s_status,
        2,
        strcmp(s_status, "READY") == 0 ? C_ACCENT : C_TEXT
    );

    fill_rect(
        SAFE_X + 64,
        SAFE_Y + 198,
        SAFE_W - 128,
        1,
        C_LINE
    );

    draw_text(
        SAFE_X + 64,
        SAFE_Y + 224,
        "MEDIA",
        2,
        C_TEXT_DIM
    );

    char count[24];
    snprintf(
        count,
        sizeof(count),
        "%u FILES",
        (unsigned)s_media_count
    );

    draw_text_right(
        SAFE_X + SAFE_W - 64,
        SAFE_Y + 224,
        count,
        2,
        C_TEXT
    );

    fill_rect(
        SAFE_X + 64,
        SAFE_Y + 256,
        SAFE_W - 128,
        1,
        C_LINE
    );

    draw_text(
        SAFE_X + 64,
        SAFE_Y + 282,
        "WI-FI",
        2,
        C_TEXT_DIM
    );

    draw_text_right(
        SAFE_X + SAFE_W - 64,
        SAFE_Y + 282,
        s_wifi,
        2,
        C_WARN
    );

    draw_back_button();

    present();
}


static void draw_brand_header_darkone83(
    int x,
    int y,
    int w,
    int h)
{
    /*
     * Compact on-device rendition of the Darkone83 EOS-era visual language:
     * black field, purple/pink shard rails, angular wordmark treatment.
     */
    stroke_round_rect(
        x, y, w, h,
        14,
        1,
        C_D83_PURPLE_DIM,
        C_BLACK
    );

    /* fractured / shard-style top and bottom rails */
    for (int i = 0; i < 7; ++i) {
        const int sx =
            x + 14 + i * ((w - 28) / 7);

        fill_rect(
            sx,
            y + 8 + (i & 1 ? 3 : 0),
            20,
            3,
            (i & 1) ? C_D83_PINK : C_D83_PURPLE
        );

        fill_rect(
            sx + 8,
            y + h - 12 - (i & 1 ? 2 : 0),
            18,
            3,
            (i & 1) ? C_D83_PURPLE : C_D83_PINK
        );
    }

    draw_text_center(
        x + w / 2,
        y + 21,
        "DARKONE83",
        4,
        C_D83_PURPLE
    );

    /* subtle hot-pink offset gives the mark a sharper/glitched edge */
    draw_text_center(
        x + w / 2 + 2,
        y + 23,
        "DARKONE83",
        4,
        C_D83_PINK
    );

    draw_text_center(
        x + w / 2,
        y + 58,
        "CODED BY",
        1,
        C_TEXT_DIM
    );
}

static void draw_brand_header_macho_nacho(
    int x,
    int y,
    int w,
    int h)
{
    /*
     * Special-thanks header using Macho Nacho Productions' warm orange /
     * yellow visual treatment without introducing another runtime image
     * decoder path into the UI.
     */
    stroke_round_rect(
        x, y, w, h,
        14,
        1,
        C_MNP_ORANGE,
        C_BLACK
    );

    fill_round_rect(
        x + 12,
        y + 11,
        6,
        h - 22,
        3,
        C_MNP_ORANGE
    );

    fill_round_rect(
        x + w - 18,
        y + 11,
        6,
        h - 22,
        3,
        C_MNP_RED
    );

    draw_text_center(
        x + w / 2,
        y + 17,
        "MACHO NACHO",
        3,
        C_MNP_YELLOW
    );

    draw_text_center(
        x + w / 2,
        y + 45,
        "PRODUCTIONS",
        2,
        C_MNP_ORANGE
    );

    draw_text_center(
        x + w / 2,
        y + 67,
        "SPECIAL THANKS",
        1,
        C_TEXT_DIM
    );
}

static void draw_about_page(void)
{
    s_page = PAGE_ABOUT;

    draw_shell("ABOUT", "TYPE N");

    /*
     * Identity block kept compact so the credits are the visual focus.
     */
    draw_text_center(
        400,
        SAFE_Y + 148,
        "TYPE N",
        4,
        C_TEXT
    );

    draw_text_center(
        400,
        SAFE_Y + 184,
        "ROUND MEDIA DISPLAY",
        2,
        C_TEXT_DIM
    );

    fill_rect(
        SAFE_X + 72,
        SAFE_Y + 214,
        SAFE_W - 144,
        1,
        C_LINE
    );

    draw_brand_header_darkone83(
        SAFE_X + 78,
        SAFE_Y + 234,
        SAFE_W - 156,
        80
    );

    draw_brand_header_macho_nacho(
        SAFE_X + 78,
        SAFE_Y + 330,
        SAFE_W - 156,
        86
    );

    /*
     * Technical identity is secondary on this page.
     */
    draw_text_center(
        400,
        SAFE_Y + 438,
        "ESP32-P4 / ESP-IDF 5.5.1",
        1,
        C_TEXT_DIM
    );

    draw_text_center(
        400,
        SAFE_Y + 456,
        "800 X 800 / RGB565",
        1,
        C_TEXT_DIM
    );

    draw_back_button();

    present();
}

/* -------------------------------------------------------------------------- */
/* Touch interaction                                                           */
/* -------------------------------------------------------------------------- */

static bool point_in(
    int x,
    int y,
    int rx,
    int ry,
    int rw,
    int rh)
{
    return x >= rx &&
           y >= ry &&
           x < rx + rw &&
           y < ry + rh;
}

static void close_menu(void)
{
    s_menu_visible = false;
    s_page = PAGE_HOME;
    s_media_confirm_visible = false;

    typen_thumb_cache_set_paused(true);

    if (typen_playback_has_frame()) {
        esp_err_t err =
            typen_playback_restore();

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "playback restore failed: %s",
                esp_err_to_name(err)
            );
            draw_idle();
        }
    } else {
        draw_idle();
    }

    typen_playback_pause(false);
}

static void handle_home_tap(int x, int y)
{
    const int row_x = SAFE_X + 48;
    const int row_w = SAFE_W - 96;
    const int row_h = 66;

    if (point_in(
            x, y,
            row_x, SAFE_Y + 146,
            row_w, row_h)) {
        draw_display_page();
        return;
    }

    if (point_in(
            x, y,
            row_x, SAFE_Y + 220,
            row_w, row_h)) {
        s_media_page_index = 0;
        s_media_confirm_visible = false;
        draw_media_page();
        return;
    }

    if (point_in(
            x, y,
            row_x, SAFE_Y + 294,
            row_w, row_h)) {
        draw_system_page();
        return;
    }

    if (point_in(
            x, y,
            row_x, SAFE_Y + 368,
            row_w, row_h)) {
        draw_about_page();
        return;
    }

    if (point_in(
            x, y,
            SAFE_X + 196,
            SAFE_Y + 462,
            158,
            44)) {
        close_menu();
    }
}

static void handle_subpage_tap(int x, int y)
{
    if (s_page == PAGE_MEDIA &&
        s_media_confirm_visible) {

        if (point_in(
                x, y,
                SAFE_X + 86,
                SAFE_Y + 430,
                150,
                50)) {
            s_media_confirm_visible = false;
            draw_media_page();
            return;
        }

        if (point_in(
                x, y,
                SAFE_X + 314,
                SAFE_Y + 430,
                150,
                50)) {
            typen_media_item_t item;

            esp_err_t err =
                typen_media_get(
                    s_media_confirm_index,
                    &item
                );

            if (err == ESP_OK) {
                err =
                    typen_settings_set_selected_media(
                        item.path
                    );
            }

            if (err == ESP_OK) {
                err =
                    typen_playback_set(
                        &item
                    );
            }

            if (err == ESP_OK) {
                ESP_LOGI(
                    TAG,
                    "Selected for playback: %s",
                    item.path
                );

                close_menu();
            } else {
                ESP_LOGE(
                    TAG,
                    "Unable to set media: %s",
                    esp_err_to_name(err)
                );

                s_media_confirm_visible = false;
                draw_media_page();
            }

            return;
        }

        return;
    }

    if (point_in(
            x, y,
            SAFE_X + 34,
            SAFE_Y + SAFE_H - 66,
            114,
            38)) {
        draw_home();
        return;
    }

    if (s_page == PAGE_MEDIA) {
        const size_t pages = media_page_count();

        if (s_media_page_index > 0 &&
            point_in(
                x, y,
                SAFE_X + 188,
                SAFE_Y + SAFE_H - 66,
                58,
                38)) {
            --s_media_page_index;
            draw_media_page();
            return;
        }

        if (s_media_page_index + 1U < pages &&
            point_in(
                x, y,
                SAFE_X + SAFE_W - 92,
                SAFE_Y + SAFE_H - 66,
                58,
                38)) {
            ++s_media_page_index;
            draw_media_page();
            return;
        }

        const int left_x = SAFE_X + 48;
        const int right_x =
            left_x + MEDIA_CARD_W + 16;

        const int top_y = SAFE_Y + 142;
        const int bottom_y =
            top_y + MEDIA_CARD_H + 14;

        const int card_x[MEDIA_PER_PAGE] = {
            left_x,
            right_x,
            left_x,
            right_x
        };

        const int card_y[MEDIA_PER_PAGE] = {
            top_y,
            top_y,
            bottom_y,
            bottom_y
        };

        const size_t first =
            s_media_page_index * MEDIA_PER_PAGE;

        for (size_t slot = 0;
             slot < MEDIA_PER_PAGE;
             ++slot) {
            const size_t index = first + slot;

            if (index >= s_media_count) {
                break;
            }

            if (point_in(
                    x, y,
                    card_x[slot],
                    card_y[slot],
                    MEDIA_CARD_W,
                    MEDIA_CARD_H)) {
                s_media_confirm_index = index;
                s_media_confirm_visible = true;
                draw_media_confirmation();
                return;
            }
        }

        return;
    }

    if (s_page == PAGE_DISPLAY) {
        const int track_x = SAFE_X + 92;
        const int track_y = SAFE_Y + 270;
        const int track_w = SAFE_W - 184;
        const int track_h = 90;

        if (point_in(
                x, y,
                track_x,
                track_y,
                track_w,
                track_h)) {
            int percent =
                ((x - track_x) * 100) / track_w;

            if (percent < 5) {
                percent = 5;
            }

            if (percent > 100) {
                percent = 100;
            }

            esp_err_t brightness_err =
                typen_ui_set_brightness(
                    (uint8_t)percent
                );

            if (brightness_err != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "brightness update failed: %s",
                    esp_err_to_name(brightness_err)
                );
            }
        }
    }
}

static void handle_menu_tap(int x, int y)
{
    if (s_page == PAGE_HOME) {
        handle_home_tap(x, y);
    } else {
        handle_subpage_tap(x, y);
    }
}

static void touch_task(void *arg)
{
    (void)arg;

    bool was_down = false;
    bool long_fired = false;

    uint16_t down_x = 0;
    uint16_t down_y = 0;
    uint16_t last_x = 0;
    uint16_t last_y = 0;

    TickType_t down_tick = 0;

    for (;;) {
        uint16_t x = 0;
        uint16_t y = 0;
        bool down = false;

        esp_err_t err =
            typen_board_touch_sample(&x, &y, &down);

        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
            continue;
        }

        if (down) {
            last_x = x;
            last_y = y;

            if (!was_down) {
                was_down = true;
                long_fired = false;
                down_x = x;
                down_y = y;
                down_tick = xTaskGetTickCount();
            } else if (!s_menu_visible &&
                       !long_fired &&
                       (xTaskGetTickCount() - down_tick) >=
                           pdMS_TO_TICKS(LONG_PRESS_MS)) {
                long_fired = true;

                /*
                 * Drain any playback framebuffer update before drawing UI.
                 * GIF timing also freezes while the menu is visible.
                 */
                typen_playback_pause(true);
                typen_thumb_cache_set_paused(false);

                s_menu_visible = true;
                draw_home();
                ESP_LOGI(TAG, "Menu opened by long press");
            }
        } else if (was_down) {
            int dx = (int)last_x - (int)down_x;
            int dy = (int)last_y - (int)down_y;

            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;

            if (s_menu_visible &&
                !long_fired &&
                dx <= TAP_MOVE_LIMIT &&
                dy <= TAP_MOVE_LIMIT) {
                handle_menu_tap(last_x, last_y);
            }

            was_down = false;
            long_fired = false;
        }

        if (typen_thumb_cache_take_dirty()) {
            if (s_menu_visible &&
                s_page == PAGE_MEDIA) {
                if (s_media_confirm_visible) {
                    draw_media_confirmation();
                } else {
                    draw_media_page();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

esp_err_t typen_ui_init(void)
{
    s_fb = typen_board_framebuffer();

    if (s_fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t saved_brightness =
        TYPEN_DEFAULT_BRIGHTNESS;

    esp_err_t brightness_err =
        typen_settings_get_brightness(
            &saved_brightness
        );

    if (brightness_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "brightness restore failed: %s; using %u%%",
            esp_err_to_name(brightness_err),
            (unsigned)TYPEN_DEFAULT_BRIGHTNESS
        );

        saved_brightness =
            TYPEN_DEFAULT_BRIGHTNESS;
    }

    if (saved_brightness < TYPEN_BRIGHTNESS_MIN) {
        saved_brightness = TYPEN_BRIGHTNESS_MIN;
    }

    if (saved_brightness > TYPEN_BRIGHTNESS_MAX) {
        saved_brightness = TYPEN_BRIGHTNESS_MAX;
    }

    s_brightness =
        saved_brightness;

    brightness_err =
        typen_board_set_brightness(
            s_brightness
        );

    if (brightness_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "unable to apply restored brightness: %s",
            esp_err_to_name(brightness_err)
        );
    }

    esp_err_t cache_err = typen_thumb_cache_init();

    if (cache_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "thumbnail cache init failed: %s",
            esp_err_to_name(cache_err)
        );
        return cache_err;
    }

    typen_thumb_cache_set_paused(true);

    /*
     * One direct draw at startup, then no redraw until the user long-presses.
     */
    draw_idle();

    if (s_touch_task == NULL) {
        BaseType_t ok = xTaskCreate(
            touch_task,
            "typen_touch_ui",
            4096,
            NULL,
            5,
            &s_touch_task
        );

        if (ok != pdPASS) {
            s_touch_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(
        TAG,
        "Direct framebuffer UI ready; long press opens menu"
    );

    return ESP_OK;
}

void typen_ui_set_status(const char *text)
{
    if (text == NULL) {
        return;
    }

    snprintf(s_status, sizeof(s_status), "%s", text);

    if (s_menu_visible && s_page == PAGE_SYSTEM) {
        draw_system_page();
    }
}

void typen_ui_set_wifi_status(const char *text)
{
    if (text == NULL) {
        return;
    }

    snprintf(s_wifi, sizeof(s_wifi), "%s", text);

    if (s_menu_visible && s_page == PAGE_SYSTEM) {
        draw_system_page();
    }
}

void typen_ui_set_media_count(size_t count)
{
    s_media_count = count;
    typen_thumb_cache_reset();

    const size_t pages = media_page_count();

    if (s_media_page_index >= pages) {
        s_media_page_index = pages - 1U;
    }

    if (s_menu_visible &&
        (s_page == PAGE_MEDIA ||
         s_page == PAGE_SYSTEM)) {
        if (s_page == PAGE_MEDIA) {
            draw_media_page();
        } else {
            draw_system_page();
        }
    }
}

esp_err_t typen_ui_set_brightness(
    uint8_t percent)
{
    if (percent < TYPEN_BRIGHTNESS_MIN) {
        percent = TYPEN_BRIGHTNESS_MIN;
    }

    if (percent > TYPEN_BRIGHTNESS_MAX) {
        percent = TYPEN_BRIGHTNESS_MAX;
    }

    esp_err_t err =
        typen_board_set_brightness(
            percent
        );

    if (err != ESP_OK) {
        return err;
    }

    s_brightness =
        percent;

    if (s_menu_visible &&
        s_page == PAGE_DISPLAY) {
        draw_display_page();
    }

    err =
        typen_settings_set_brightness(
            percent
        );

    return err;
}

uint8_t typen_ui_get_brightness(void)
{
    return s_brightness;
}

esp_err_t typen_ui_show_media(
    const typen_media_item_t *item)
{
    if (item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        typen_settings_set_selected_media(
            item->path
        );

    if (err != ESP_OK) {
        return err;
    }

    return typen_playback_set(item);
}
