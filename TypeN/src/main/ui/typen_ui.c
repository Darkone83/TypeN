#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_vfs_fat.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "typen_board.h"
#include "typen_media.h"
#include "typen_playback.h"
#include "typen_settings.h"
#include "typen_storage.h"
#include "typen_thumb_cache.h"
#include "typen_ui.h"
#include "typen_ui_misc.h"
#include "typen_update.h"

#define SAFE_X                  125
#define SAFE_Y                  125
#define SAFE_W                  550
#define SAFE_H                  550

#define TOUCH_POLL_MS            20
#define LONG_PRESS_MS           700
#define MISC_LONG_PRESS_MS     2000
#define TAP_MOVE_LIMIT           24

/*
 * UI animation timing.
 *
 * The panel runs at ~65 Hz, so each page-flipped animation frame is held
 * slightly longer than one scan. This avoids modifying a framebuffer while
 * DSI is still scanning it.
 */
#define UI_ANIM_FRAMES           10
#define UI_ANIM_FRAME_MS         18
#define UI_ANIM_SETTLE_MS        18
#define UI_ANIM_PRESS_MS         55

#define STARTUP_FLOURISH_FRAMES  18
#define STARTUP_RING_STEPS       56

#define UI_FB_PIXELS     ((size_t)TYPEN_LCD_W * (size_t)TYPEN_LCD_H)
#define UI_FB_BYTES     (UI_FB_PIXELS * sizeof(uint16_t))

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

#define MEDIA_DELETE_W            42
#define MEDIA_DELETE_H            42
#define MEDIA_DELETE_MARGIN        4

typedef enum {
    PAGE_HOME = 0,
    PAGE_DISPLAY,
    PAGE_MEDIA,
    PAGE_SYSTEM,
    PAGE_DIAGNOSTICS,
    PAGE_ABOUT,
} typen_page_t;

typedef enum {
    MEDIA_CONFIRM_SET = 0,
    MEDIA_CONFIRM_DELETE,
} media_confirm_mode_t;

static const char *TAG = "typen_ui";

static uint16_t *s_fb = NULL;
static TaskHandle_t s_touch_task = NULL;
static SemaphoreHandle_t s_misc_control_lock = NULL;

static bool s_menu_visible = false;
static typen_page_t s_page = PAGE_HOME;

typedef struct {
    bool active;
    uint16_t *ui_fb;
    uint16_t *alt_fb;
    uint16_t *alt_backup;
    uint16_t *from_frame;
    uint16_t *to_frame;
    uint16_t *displayed_fb;
} typen_ui_anim_ctx_t;

static typen_ui_anim_ctx_t s_anim = {0};
static bool s_ui_animating = false;
static bool s_present_suppressed = false;
static int s_home_pressed_y = -1;

static uint8_t s_brightness = TYPEN_DEFAULT_BRIGHTNESS;
static size_t s_media_count = 0;
static size_t s_media_page_index = 0;
static size_t s_media_pressed_index = (size_t)-1;
static bool s_media_confirm_visible = false;
static size_t s_media_confirm_index = 0;
static media_confirm_mode_t s_media_confirm_mode =
    MEDIA_CONFIRM_SET;
static bool s_factory_reset_confirm_visible = false;

static char s_status[48] = "STARTING";
static char s_wifi[48] = "STARTING";

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
    if (s_present_suppressed) {
        return;
    }

    if (!s_anim.active) {
        esp_err_t err =
            typen_board_present();

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "present failed: %s",
                esp_err_to_name(err)
            );
        }

        return;
    }

    /*
     * During a menu session s_fb always points at the framebuffer that is
     * NOT currently being scanned. Present it, wait a full scan interval,
     * then hand drawing to the other native framebuffer.
     */
    uint16_t *submitted =
        s_fb;

    esp_err_t err =
        typen_board_present_buffer(
            submitted
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "animated present failed: %s",
            esp_err_to_name(err)
        );
        return;
    }

    s_anim.displayed_fb =
        submitted;

    vTaskDelay(
        pdMS_TO_TICKS(
            UI_ANIM_SETTLE_MS
        )
    );

    s_fb =
        submitted == s_anim.ui_fb
            ? s_anim.alt_fb
            : s_anim.ui_fb;
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
    size_t index,
    const char *active_path)
{
    typen_media_item_t item;

    if (typen_media_get(index, &item) != ESP_OK) {
        return;
    }

    const bool active =
        active_path != NULL &&
        active_path[0] != '\0' &&
        strcmp(active_path, item.path) == 0;

    const bool pressed =
        s_media_pressed_index == index;

    const uint16_t border_color =
        active
            ? C_ACCENT
            : (pressed ? C_TEXT_DIM : C_LINE);

    const uint16_t fill_color =
        pressed
            ? C_PANEL_PRESSED
            : C_PANEL_2;

    stroke_round_rect(
        x,
        y,
        MEDIA_CARD_W,
        MEDIA_CARD_H,
        14,
        active ? 3 : 1,
        border_color,
        fill_color
    );

    if (active) {
        fill_round_rect(
            x + 5,
            y + 18,
            4,
            MEDIA_CARD_H - 36,
            2,
            C_ACCENT
        );
    }

    draw_media_thumbnail(
        index,
        &item,
        x + 11,
        y + 9,
        MEDIA_THUMB_W,
        MEDIA_THUMB_H
    );

    if (active) {
        fill_round_rect(
            x + 16,
            y + 14,
            58,
            20,
            8,
            C_ACCENT_DIM
        );

        draw_text_center(
            x + 45,
            y + 20,
            "ACTIVE",
            1,
            C_ACCENT
        );
    }

    const int delete_x =
        x + 11 +
        MEDIA_THUMB_W -
        MEDIA_DELETE_W -
        MEDIA_DELETE_MARGIN;

    const int delete_y =
        y + 9 +
        MEDIA_DELETE_MARGIN;

    stroke_round_rect(
        delete_x,
        delete_y,
        MEDIA_DELETE_W,
        MEDIA_DELETE_H,
        10,
        1,
        C_WARN,
        C_PANEL_PRESSED
    );

    draw_text_center(
        delete_x + MEDIA_DELETE_W / 2,
        delete_y + 14,
        "X",
        2,
        C_WARN
    );

    draw_text_ellipsis(
        x + 12,
        y + 106,
        item.name,
        1,
        active ? C_ACCENT : C_TEXT,
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

    const bool deleting =
        s_media_confirm_mode ==
        MEDIA_CONFIRM_DELETE;

    draw_shell(
        deleting ? "DELETE MEDIA?" : "ACTIVATE MEDIA?",
        typen_media_type_name(item.type)
    );

    draw_media_thumbnail(
        s_media_confirm_index,
        &item,
        SAFE_X + 110,
        SAFE_Y + 146,
        330,
        188
    );

    draw_text_ellipsis(
        SAFE_X + 76,
        SAFE_Y + 352,
        item.name,
        2,
        C_TEXT,
        31
    );

    draw_text_center(
        400,
        SAFE_Y + 390,
        deleting
            ? "PERMANENTLY REMOVE THIS FILE"
            : "DISPLAY THIS FILE ON TYPE N",
        1,
        deleting ? C_WARN : C_TEXT_DIM
    );

    const int action_y =
        SAFE_Y + 424;

    const int action_w = 164;
    const int action_h = 54;

    stroke_round_rect(
        SAFE_X + 72,
        action_y,
        action_w,
        action_h,
        15,
        1,
        C_LINE,
        C_PANEL_2
    );

    draw_text_center(
        SAFE_X + 72 + action_w / 2,
        action_y + 18,
        "CANCEL",
        2,
        C_TEXT_DIM
    );

    stroke_round_rect(
        SAFE_X + SAFE_W - 72 - action_w,
        action_y,
        action_w,
        action_h,
        15,
        2,
        deleting ? C_WARN : C_ACCENT_DIM,
        C_PANEL_PRESSED
    );

    draw_text_center(
        SAFE_X + SAFE_W - 72 - action_w / 2,
        action_y + 18,
        deleting ? "DELETE" : "ACTIVATE",
        2,
        deleting ? C_WARN : C_ACCENT
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
        SAFE_Y + SAFE_H - 70,
        122,
        44,
        13,
        1,
        C_LINE,
        C_PANEL_2
    );

    draw_text_center(
        SAFE_X + 95,
        SAFE_Y + SAFE_H - 57,
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

    const bool pressed =
        enabled &&
        s_home_pressed_y == y;

    stroke_round_rect(
        x,
        y,
        w,
        h,
        15,
        pressed ? 2 : 1,
        pressed
            ? C_ACCENT
            : (enabled ? C_LINE : C_PANEL_2),
        pressed
            ? C_PANEL_PRESSED
            : C_PANEL_2
    );

    fill_round_rect(
        x + 14,
        y + 13,
        pressed ? 6 : 4,
        h - 26,
        2,
        pressed
            ? C_ACCENT
            : (enabled ? C_ACCENT_DIM : C_LINE)
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


typedef struct {
    int x;
    int y;
    float z;
    bool valid;
} startup_flourish_point_t;

static void startup_flourish_line(
    int x0,
    int y0,
    int x1,
    int y1,
    uint16_t color)
{
    int dx =
        x1 > x0
            ? x1 - x0
            : x0 - x1;

    const int sx =
        x0 < x1
            ? 1
            : -1;

    int dy =
        y1 > y0
            ? y0 - y1
            : y1 - y0;

    const int sy =
        y0 < y1
            ? 1
            : -1;

    int err =
        dx + dy;

    for (;;) {
        px(
            x0,
            y0,
            color
        );

        if (x0 == x1 &&
            y0 == y1) {
            break;
        }

        const int e2 =
            err * 2;

        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }

        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void startup_flourish_glow_line(
    int x0,
    int y0,
    int x1,
    int y1,
    uint16_t halo,
    uint16_t core)
{
    startup_flourish_line(
        x0 - 1,
        y0,
        x1 - 1,
        y1,
        halo
    );

    startup_flourish_line(
        x0 + 1,
        y0,
        x1 + 1,
        y1,
        halo
    );

    startup_flourish_line(
        x0,
        y0 - 1,
        x1,
        y1 - 1,
        halo
    );

    startup_flourish_line(
        x0,
        y0 + 1,
        x1,
        y1 + 1,
        halo
    );

    startup_flourish_line(
        x0,
        y0,
        x1,
        y1,
        core
    );
}

static startup_flourish_point_t startup_flourish_project(
    float x,
    float y,
    float z,
    float angle_y,
    float angle_x,
    float scale)
{
    const float cy =
        cosf(angle_y);

    const float sy =
        sinf(angle_y);

    const float cx =
        cosf(angle_x);

    const float sx =
        sinf(angle_x);

    const float xr =
        x * cy +
        z * sy;

    const float zr =
        -x * sy +
        z * cy;

    const float yr =
        y * cx -
        zr * sx;

    const float z2 =
        y * sx +
        zr * cx;

    const float camera =
        680.0f;

    const float denom =
        camera - z2;

    startup_flourish_point_t p = {
        .x = 0,
        .y = 0,
        .z = z2,
        .valid = false,
    };

    if (denom < 180.0f) {
        return p;
    }

    const float perspective =
        (520.0f / denom) *
        scale;

    p.x =
        400 +
        (int)(xr * perspective);

    p.y =
        392 +
        (int)(yr * perspective);

    p.valid =
        p.x >= 32 &&
        p.x < TYPEN_LCD_W - 32 &&
        p.y >= 32 &&
        p.y < TYPEN_LCD_H - 32;

    return p;
}

static void startup_flourish_draw_orbit(
    float radius_x,
    float radius_y,
    float tilt,
    float spin,
    float scale,
    int strength_q8)
{
    startup_flourish_point_t prev = {
        .valid = false
    };

    const uint16_t halo =
        strength_q8 > 150
            ? C_D83_PURPLE_DIM
            : C_LINE;

    const uint16_t front =
        strength_q8 > 196
            ? C_D83_PINK
            : C_D83_PURPLE;

    for (int step = 0;
         step <= STARTUP_RING_STEPS;
         ++step) {
        const float a =
            ((float)step /
             (float)STARTUP_RING_STEPS) *
            6.28318531f;

        /*
         * Ellipse in local X/Z plane, with a small vertical component.
         * The projection supplies the 3D feel; this is intentionally only
         * decorative and never alters the TypeN logo geometry.
         */
        const float x =
            cosf(a) *
            radius_x;

        const float y =
            sinf(a * 2.0f) *
            radius_y *
            0.12f;

        const float z =
            sinf(a) *
            radius_y;

        startup_flourish_point_t p =
            startup_flourish_project(
                x,
                y,
                z,
                spin,
                tilt,
                scale
            );

        if (prev.valid &&
            p.valid) {
            const float depth =
                (prev.z + p.z) *
                0.5f;

            const uint16_t core =
                depth > 0.0f
                    ? front
                    : C_D83_PURPLE_DIM;

            startup_flourish_glow_line(
                prev.x,
                prev.y,
                p.x,
                p.y,
                halo,
                core
            );
        }

        prev = p;
    }
}

static void startup_flourish_draw_original_logo(
    bool show_hint)
{
    /*
     * IMPORTANT:
     * This is the existing TypeN identity from draw_idle().
     * Do not alter dimensions, placement, or wordmark treatment here.
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

    if (show_hint) {
        draw_text_center(
            400,
            550,
            "HOLD FOR MENU",
            2,
            C_TEXT_DIM
        );
    }
}

static void startup_flourish_scene(
    int progress_q8,
    bool settled)
{
    if (progress_q8 < 0) {
        progress_q8 = 0;
    }

    if (progress_q8 > 256) {
        progress_q8 = 256;
    }

    clear_screen();

    const float t =
        (float)progress_q8 /
        256.0f;

    /*
     * Flourish starts large and airy, then tightens around the fixed logo.
     * The logo itself never rotates, scales, extrudes, or changes shape.
     */
    const float orbit_scale =
        1.28f -
        0.20f * t;

    const float spin =
        -1.05f +
        1.62f * t;

    const float settle =
        sinf(t * 3.14159265f);

    startup_flourish_draw_orbit(
        238.0f,
        188.0f,
        -0.48f +
            0.12f * settle,
        spin,
        orbit_scale,
        progress_q8
    );

    startup_flourish_draw_orbit(
        214.0f,
        164.0f,
        0.44f -
            0.10f * settle,
        -spin * 0.72f +
            0.35f,
        orbit_scale * 0.94f,
        progress_q8
    );

    startup_flourish_draw_orbit(
        196.0f,
        150.0f,
        0.06f,
        spin * 0.46f -
            0.25f,
        orbit_scale * 0.90f,
        progress_q8
    );

    /*
     * A few depth points skim the orbit. They are intentionally sparse.
     */
    for (int i = 0;
         i < 4;
         ++i) {
        const float a =
            spin +
            i * 1.57079633f;

        startup_flourish_point_t p =
            startup_flourish_project(
                cosf(a) * 235.0f,
                -28.0f +
                    i * 18.0f,
                sinf(a) * 188.0f,
                0.0f,
                -0.24f,
                orbit_scale
            );

        if (p.valid) {
            const int r =
                p.z > 0.0f
                    ? 4
                    : 2;

            fill_round_rect(
                p.x - r,
                p.y - r,
                r * 2 + 1,
                r * 2 + 1,
                r,
                p.z > 0.0f
                    ? C_D83_PINK
                    : C_D83_PURPLE_DIM
            );
        }
    }

    /*
     * Original logo is always the visual anchor and is rendered last so none
     * of the decorative 3D lines can cut through it.
     */
    startup_flourish_draw_original_logo(
        settled
    );
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
        "SD LIBRARY",
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
        SAFE_X + 190,
        SAFE_Y + 458,
        170,
        44,
        14,
        1,
        C_LINE,
        C_PANEL_2
    );

    draw_text_center(
        400,
        SAFE_Y + 471,
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

    draw_text(
        track_x,
        SAFE_Y + 338,
        "5%",
        1,
        C_TEXT_DIM
    );

    draw_text_right(
        track_x + track_w,
        SAFE_Y + 338,
        "100%",
        1,
        C_TEXT_DIM
    );

    draw_text_center(
        400,
        SAFE_Y + 366,
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
    s_media_confirm_mode = MEDIA_CONFIRM_SET;

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

    char active_path[TYPEN_SETTINGS_PATH_MAX] = {0};

    if (typen_settings_get_selected_media(
            active_path,
            sizeof(active_path)) != ESP_OK) {
        active_path[0] = '\0';
    }

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
            index,
            active_path
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

static const char *ui_reset_reason_name(
    esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:
            return "POWER ON";
        case ESP_RST_EXT:
            return "EXTERNAL";
        case ESP_RST_SW:
            return "SOFTWARE";
        case ESP_RST_PANIC:
            return "PANIC";
        case ESP_RST_INT_WDT:
            return "INT WDT";
        case ESP_RST_TASK_WDT:
            return "TASK WDT";
        case ESP_RST_WDT:
            return "WATCHDOG";
        case ESP_RST_DEEPSLEEP:
            return "DEEP SLEEP";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT";
        case ESP_RST_SDIO:
            return "SDIO";
        case ESP_RST_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

static void format_bytes_short(
    uint64_t bytes,
    char *out,
    size_t out_size)
{
    if (!out ||
        out_size == 0) {
        return;
    }

    const uint64_t gib =
        UINT64_C(1024) *
        UINT64_C(1024) *
        UINT64_C(1024);

    const uint64_t mib =
        UINT64_C(1024) *
        UINT64_C(1024);

    const uint64_t kib =
        UINT64_C(1024);

    if (bytes >= gib) {
        const uint64_t tenths =
            (bytes * UINT64_C(10)) /
            gib;

        snprintf(
            out,
            out_size,
            "%llu.%llu GB",
            (unsigned long long)(
                tenths / UINT64_C(10)
            ),
            (unsigned long long)(
                tenths % UINT64_C(10)
            )
        );
    } else if (bytes >= mib) {
        const uint64_t tenths =
            (bytes * UINT64_C(10)) /
            mib;

        snprintf(
            out,
            out_size,
            "%llu.%llu MB",
            (unsigned long long)(
                tenths / UINT64_C(10)
            ),
            (unsigned long long)(
                tenths % UINT64_C(10)
            )
        );
    } else if (bytes >= kib) {
        snprintf(
            out,
            out_size,
            "%llu KB",
            (unsigned long long)(
                bytes /
                kib
            )
        );
    } else {
        snprintf(
            out,
            out_size,
            "%llu B",
            (unsigned long long)bytes
        );
    }
}

static void format_uptime_short(
    uint64_t uptime_ms,
    char *out,
    size_t out_size)
{
    if (!out ||
        out_size == 0) {
        return;
    }

    uint64_t seconds =
        uptime_ms /
        UINT64_C(1000);

    const uint64_t days =
        seconds /
        UINT64_C(86400);

    seconds %=
        UINT64_C(86400);

    const uint64_t hours =
        seconds /
        UINT64_C(3600);

    seconds %=
        UINT64_C(3600);

    const uint64_t minutes =
        seconds /
        UINT64_C(60);

    seconds %=
        UINT64_C(60);

    if (days > 0) {
        snprintf(
            out,
            out_size,
            "%lluD %lluH",
            (unsigned long long)days,
            (unsigned long long)hours
        );
    } else if (hours > 0) {
        snprintf(
            out,
            out_size,
            "%lluH %lluM",
            (unsigned long long)hours,
            (unsigned long long)minutes
        );
    } else if (minutes > 0) {
        snprintf(
            out,
            out_size,
            "%lluM %lluS",
            (unsigned long long)minutes,
            (unsigned long long)seconds
        );
    } else {
        snprintf(
            out,
            out_size,
            "%lluS",
            (unsigned long long)seconds
        );
    }
}

static void draw_system_page(void)
{
    s_page = PAGE_SYSTEM;

    draw_shell("SYSTEM", "DEVICE STATUS");

    const int x = SAFE_X + 56;
    const int w = SAFE_W - 112;
    const int h = 72;

    const bool storage_problem =
        strstr(s_status, "fail") != NULL ||
        strstr(s_status, "Fail") != NULL ||
        strstr(s_status, "unavailable") != NULL ||
        strstr(s_status, "Unavailable") != NULL;

    const bool wifi_ready =
        strchr(s_wifi, '.') != NULL;

    char media_value[24];

    snprintf(
        media_value,
        sizeof(media_value),
        "%u FILE%s",
        (unsigned)s_media_count,
        s_media_count == 1 ? "" : "S"
    );

    const int row_y[3] = {
        SAFE_Y + 148,
        SAFE_Y + 230,
        SAFE_Y + 312
    };

    const char *labels[3] = {
        "STORAGE",
        "MEDIA",
        "WI-FI"
    };

    const char *values[3] = {
        s_status,
        media_value,
        s_wifi
    };

    const uint16_t value_colors[3] = {
        storage_problem ? C_WARN : C_ACCENT,
        C_TEXT,
        wifi_ready ? C_ACCENT : C_WARN
    };

    for (int i = 0; i < 3; ++i) {
        stroke_round_rect(
            x,
            row_y[i],
            w,
            h,
            16,
            1,
            C_LINE,
            C_PANEL_2
        );

        fill_round_rect(
            x + 12,
            row_y[i] + 14,
            4,
            h - 28,
            2,
            value_colors[i]
        );

        draw_text(
            x + 32,
            row_y[i] + 12,
            labels[i],
            1,
            C_TEXT_DIM
        );

        draw_text_right(
            x + w - 22,
            row_y[i] + 31,
            values[i],
            2,
            value_colors[i]
        );
    }

    const int action_x =
        SAFE_X + 56;

    const int action_y =
        SAFE_Y + 397;

    const int action_gap = 16;

    const int action_w =
        (SAFE_W - 112 - action_gap) / 2;

    const int action_h = 54;

    stroke_round_rect(
        action_x,
        action_y,
        action_w,
        action_h,
        15,
        1,
        C_ACCENT,
        C_PANEL_2
    );

    draw_text_center(
        action_x + action_w / 2,
        action_y + 18,
        "DIAGNOSTICS",
        2,
        C_ACCENT
    );

    const int reset_x =
        action_x +
        action_w +
        action_gap;

    stroke_round_rect(
        reset_x,
        action_y,
        action_w,
        action_h,
        15,
        1,
        C_WARN,
        C_PANEL_2
    );

    draw_text_center(
        reset_x + action_w / 2,
        action_y + 18,
        "FACTORY RESET",
        2,
        C_WARN
    );

    draw_back_button();

    present();
}

static void draw_diagnostics_page(void)
{
    s_page = PAGE_DIAGNOSTICS;

    draw_shell(
        "DIAGNOSTICS",
        "LIVE DEVICE STATUS"
    );

    char uptime[24] = {0};
    char sd_free[24] = "UNAVAILABLE";
    char heap_free[24] = {0};
    char psram_free[24] = {0};

    format_uptime_short(
        (uint64_t)(
            esp_timer_get_time() /
            1000
        ),
        uptime,
        sizeof(uptime)
    );

    uint64_t sd_total_bytes = 0;
    uint64_t sd_free_bytes = 0;

    if (esp_vfs_fat_info(
            TYPEN_SD_ROOT,
            &sd_total_bytes,
            &sd_free_bytes) == ESP_OK) {
        format_bytes_short(
            sd_free_bytes,
            sd_free,
            sizeof(sd_free)
        );
    }

    format_bytes_short(
        (uint64_t)heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        ),
        heap_free,
        sizeof(heap_free)
    );

    format_bytes_short(
        (uint64_t)heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        ),
        psram_free,
        sizeof(psram_free)
    );

    typen_update_status_t update_status;
    memset(
        &update_status,
        0,
        sizeof(update_status)
    );

    typen_update_get_status(
        &update_status
    );

    char ota_value[48];

    if (update_status.running_partition[0]) {
        snprintf(
            ota_value,
            sizeof(ota_value),
            "%s / %s",
            update_status.running_partition,
            update_status.image_state[0]
                ? update_status.image_state
                : "UNKNOWN"
        );
    } else {
        snprintf(
            ota_value,
            sizeof(ota_value),
            "%s",
            "UNKNOWN"
        );
    }

    const char *labels[6] = {
        "UPTIME",
        "RESET",
        "SD FREE",
        "HEAP FREE",
        "PSRAM FREE",
        "OTA"
    };

    const char *values[6] = {
        uptime,
        ui_reset_reason_name(
            esp_reset_reason()
        ),
        sd_free,
        heap_free,
        psram_free,
        ota_value
    };

    const int card_gap = 16;
    const int card_w =
        (SAFE_W - 88 - card_gap) / 2;

    const int card_h = 72;
    const int left_x = SAFE_X + 44;
    const int right_x =
        left_x + card_w + card_gap;

    const int row_y[3] = {
        SAFE_Y + 148,
        SAFE_Y + 230,
        SAFE_Y + 312
    };

    for (int i = 0; i < 6; ++i) {
        const int col =
            i & 1;

        const int row =
            i / 2;

        const int x =
            col
                ? right_x
                : left_x;

        stroke_round_rect(
            x,
            row_y[row],
            card_w,
            card_h,
            16,
            1,
            C_LINE,
            C_PANEL_2
        );

        fill_round_rect(
            x + 12,
            row_y[row] + 14,
            4,
            card_h - 28,
            2,
            i == 1
                ? C_TEXT_DIM
                : C_ACCENT
        );

        draw_text(
            x + 30,
            row_y[row] + 12,
            labels[i],
            1,
            C_TEXT_DIM
        );

        draw_text_right(
            x + card_w - 18,
            row_y[row] + 34,
            values[i],
            1,
            i == 1
                ? C_TEXT
                : C_ACCENT
        );
    }

    draw_text_center(
        400,
        SAFE_Y + 410,
        "WEBUI HAS FULL SUPPORT DETAILS",
        1,
        C_TEXT_DIM
    );

    draw_back_button();

    present();
}

static void draw_factory_reset_confirmation(void)
{
    draw_shell(
        "FACTORY RESET?",
        "SETTINGS ONLY"
    );

    draw_text_center(
        400,
        SAFE_Y + 176,
        "CLEARS TYPE N SETTINGS",
        2,
        C_TEXT
    );

    draw_text_center(
        400,
        SAFE_Y + 216,
        "ADMIN PIN + ACTIVE MEDIA",
        2,
        C_WARN
    );

    draw_text_center(
        400,
        SAFE_Y + 258,
        "BRIGHTNESS + CUSTOM PATHS",
        1,
        C_TEXT_DIM
    );

    draw_text_center(
        400,
        SAFE_Y + 286,
        "SD MEDIA AND FIRMWARE ARE KEPT",
        1,
        C_TEXT_DIM
    );

    const int action_y =
        SAFE_Y + 424;

    const int action_w = 164;
    const int action_h = 54;

    stroke_round_rect(
        SAFE_X + 72,
        action_y,
        action_w,
        action_h,
        15,
        1,
        C_LINE,
        C_PANEL_2
    );

    draw_text_center(
        SAFE_X + 72 + action_w / 2,
        action_y + 18,
        "CANCEL",
        2,
        C_TEXT_DIM
    );

    stroke_round_rect(
        SAFE_X + SAFE_W - 72 - action_w,
        action_y,
        action_w,
        action_h,
        15,
        2,
        C_WARN,
        C_PANEL_PRESSED
    );

    draw_text_center(
        SAFE_X + SAFE_W - 72 - action_w / 2,
        action_y + 18,
        "RESET",
        2,
        C_WARN
    );

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

    typen_update_status_t update_status;
    memset(
        &update_status,
        0,
        sizeof(update_status)
    );

    typen_update_get_status(
        &update_status
    );

    const char *version =
        update_status.running_version[0]
            ? update_status.running_version
            : "UNKNOWN";

    const char *variant =
        update_status.running_variant[0]
            ? update_status.running_variant
            : "TYPE N";

    char identity_line[48];

    if (update_status.running_firmware_abi > 0) {
        snprintf(
            identity_line,
            sizeof(identity_line),
            "%s / ABI %u",
            variant,
            (unsigned)update_status.running_firmware_abi
        );
    } else {
        snprintf(
            identity_line,
            sizeof(identity_line),
            "%s",
            variant
        );
    }

    draw_text_center(
        400,
        SAFE_Y + 142,
        "TYPE N",
        3,
        C_TEXT
    );

    draw_text_center(
        400,
        SAFE_Y + 174,
        version,
        3,
        C_ACCENT
    );

    draw_text_center(
        400,
        SAFE_Y + 204,
        identity_line,
        1,
        C_TEXT_DIM
    );

    draw_brand_header_darkone83(
        SAFE_X + 78,
        SAFE_Y + 228,
        SAFE_W - 156,
        74
    );

    draw_brand_header_macho_nacho(
        SAFE_X + 78,
        SAFE_Y + 316,
        SAFE_W - 156,
        78
    );

    char slot_line[64];

    snprintf(
        slot_line,
        sizeof(slot_line),
        "SLOT %s / %s",
        update_status.running_partition[0]
            ? update_status.running_partition
            : "UNKNOWN",
        update_status.image_state[0]
            ? update_status.image_state
            : "UNKNOWN"
    );

    draw_text_center(
        400,
        SAFE_Y + 414,
        slot_line,
        1,
        C_TEXT_DIM
    );

    char idf_line[64];

    snprintf(
        idf_line,
        sizeof(idf_line),
        "ESP-IDF %s",
        esp_get_idf_version()
    );

    draw_text_center(
        400,
        SAFE_Y + 434,
        idf_line,
        1,
        C_TEXT_DIM
    );

    draw_back_button();

    present();
}


/* -------------------------------------------------------------------------- */
/* Smooth page-flipped device animations                                      */
/* -------------------------------------------------------------------------- */

typedef void (*ui_draw_fn_t)(void);

typedef enum {
    UI_PUSH_LEFT = 0,
    UI_PUSH_RIGHT,
    UI_PUSH_UP,
    UI_PUSH_DOWN,
} ui_push_direction_t;

static uint16_t *ui_anim_alloc_frame(void)
{
    return (uint16_t *)
        heap_caps_malloc(
            UI_FB_BYTES,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );
}

static void ui_anim_free_frame(
    uint16_t **frame)
{
    if (frame && *frame) {
        heap_caps_free(*frame);
        *frame = NULL;
    }
}

static int ui_anim_ease_q8(
    int frame,
    int frame_count)
{
    if (frame <= 0) {
        return 0;
    }

    if (frame >= frame_count) {
        return 256;
    }

    /*
     * Cubic smoothstep:
     *   3t^2 - 2t^3
     *
     * Gives visible travel without the abrupt stop of a linear wipe.
     */
    const int t =
        (frame * 256) /
        frame_count;

    const int t2 =
        (t * t) >> 8;

    const int t3 =
        (t2 * t) >> 8;

    return
        3 * t2 -
        2 * t3;
}

static const uint16_t *ui_anim_playback_source(void)
{
    const uint16_t *frame =
        typen_playback_current_framebuffer();

    if (!frame) {
        return NULL;
    }

    /*
     * If playback's current frame is the borrowed alternate framebuffer,
     * use its preserved copy instead of the animation pixels now living there.
     */
    if (s_anim.active &&
        frame == s_anim.alt_fb &&
        s_anim.alt_backup) {
        return s_anim.alt_backup;
    }

    return frame;
}

static bool ui_anim_session_begin(void)
{
    if (s_anim.active) {
        return true;
    }

    uint16_t *ui_fb =
        typen_board_framebuffer();

    uint16_t *alt_fb =
        typen_board_playback_buffer(0);

    if (!ui_fb ||
        !alt_fb) {
        return false;
    }

    uint16_t *backup =
        ui_anim_alloc_frame();

    uint16_t *from_frame =
        ui_anim_alloc_frame();

    uint16_t *to_frame =
        ui_anim_alloc_frame();

    if (!backup ||
        !from_frame ||
        !to_frame) {
        ui_anim_free_frame(&backup);
        ui_anim_free_frame(&from_frame);
        ui_anim_free_frame(&to_frame);

        ESP_LOGW(
            TAG,
            "UI animation buffers unavailable; using static transitions"
        );

        return false;
    }

    memcpy(
        backup,
        alt_fb,
        UI_FB_BYTES
    );

    const uint16_t *visible =
        typen_playback_has_frame()
            ? typen_playback_current_framebuffer()
            : ui_fb;

    if (!visible) {
        visible = ui_fb;
    }

    memcpy(
        from_frame,
        visible,
        UI_FB_BYTES
    );

    s_anim.active = true;
    s_anim.ui_fb = ui_fb;
    s_anim.alt_fb = alt_fb;
    s_anim.alt_backup = backup;
    s_anim.from_frame = from_frame;
    s_anim.to_frame = to_frame;
    s_anim.displayed_fb =
        (uint16_t *)visible;

    /*
     * Draw the first UI animation frame into a native framebuffer that is not
     * currently visible.
     */
    s_fb =
        visible == ui_fb
            ? alt_fb
            : ui_fb;

    return true;
}

static void ui_anim_force_ui_scanout(void)
{
    if (!s_anim.active ||
        !s_anim.ui_fb) {
        return;
    }

    if (s_anim.displayed_fb ==
        s_anim.ui_fb) {
        return;
    }

    /*
     * Copy the current displayed picture to FB0 so switching scanout doesn't
     * visually jump before the borrowed playback buffer is restored.
     */
    if (s_anim.displayed_fb) {
        memcpy(
            s_anim.ui_fb,
            s_anim.displayed_fb,
            UI_FB_BYTES
        );
    }

    esp_err_t err =
        typen_board_present_buffer(
            s_anim.ui_fb
        );

    if (err == ESP_OK) {
        s_anim.displayed_fb =
            s_anim.ui_fb;

        vTaskDelay(
            pdMS_TO_TICKS(
                UI_ANIM_SETTLE_MS
            )
        );
    } else {
        ESP_LOGW(
            TAG,
            "Unable to return animation scanout to UI framebuffer: %s",
            esp_err_to_name(err)
        );
    }
}

static void ui_anim_session_end(void)
{
    if (!s_anim.active) {
        s_fb =
            typen_board_framebuffer();
        return;
    }

    ui_anim_force_ui_scanout();

    if (s_anim.alt_fb &&
        s_anim.alt_backup) {
        memcpy(
            s_anim.alt_fb,
            s_anim.alt_backup,
            UI_FB_BYTES
        );
    }

    ui_anim_free_frame(
        &s_anim.alt_backup
    );

    ui_anim_free_frame(
        &s_anim.from_frame
    );

    ui_anim_free_frame(
        &s_anim.to_frame
    );

    s_fb =
        s_anim.ui_fb
            ? s_anim.ui_fb
            : typen_board_framebuffer();

    memset(
        &s_anim,
        0,
        sizeof(s_anim)
    );
}

static void ui_anim_render_to(
    uint16_t *target,
    ui_draw_fn_t draw_fn)
{
    if (!target ||
        !draw_fn) {
        return;
    }

    uint16_t *saved_fb =
        s_fb;

    const bool saved_suppressed =
        s_present_suppressed;

    s_fb = target;
    s_present_suppressed = true;

    draw_fn();

    s_present_suppressed =
        saved_suppressed;

    s_fb =
        saved_fb;
}

static void ui_anim_snapshot_visible(
    uint16_t *target)
{
    if (!target) {
        return;
    }

    const uint16_t *source =
        s_anim.active
            ? s_anim.displayed_fb
            : typen_board_framebuffer();

    if (!source) {
        memset(
            target,
            0,
            UI_FB_BYTES
        );
        return;
    }

    if (s_anim.active &&
        source == s_anim.alt_fb &&
        !s_menu_visible &&
        s_anim.alt_backup) {
        source =
            s_anim.alt_backup;
    }

    memcpy(
        target,
        source,
        UI_FB_BYTES
    );
}

static inline uint16_t ui_blend565(
    uint16_t a,
    uint16_t b,
    unsigned alpha)
{
    const unsigned inv =
        256U - alpha;

    const unsigned ar =
        (a >> 11) & 0x1FU;
    const unsigned ag =
        (a >> 5) & 0x3FU;
    const unsigned ab =
        a & 0x1FU;

    const unsigned br =
        (b >> 11) & 0x1FU;
    const unsigned bg =
        (b >> 5) & 0x3FU;
    const unsigned bb =
        b & 0x1FU;

    const unsigned r =
        (ar * inv + br * alpha) >> 8;

    const unsigned g =
        (ag * inv + bg * alpha) >> 8;

    const unsigned bl =
        (ab * inv + bb * alpha) >> 8;

    return (uint16_t)(
        (r << 11) |
        (g << 5) |
        bl
    );
}

static void ui_anim_compose_crossfade(
    uint16_t *out,
    const uint16_t *from,
    const uint16_t *to,
    unsigned alpha)
{
    if (!out ||
        !from ||
        !to) {
        return;
    }

    for (size_t i = 0;
         i < UI_FB_PIXELS;
         ++i) {
        out[i] =
            ui_blend565(
                from[i],
                to[i],
                alpha
            );
    }
}

static void ui_anim_copy_row_segment(
    uint16_t *dst,
    int dst_x,
    const uint16_t *src,
    int src_x,
    int y,
    int width)
{
    if (!dst ||
        !src ||
        width <= 0) {
        return;
    }

    memcpy(
        &dst[
            (size_t)y * TYPEN_LCD_W +
            (size_t)dst_x
        ],
        &src[
            (size_t)y * TYPEN_LCD_W +
            (size_t)src_x
        ],
        (size_t)width *
            sizeof(uint16_t)
    );
}

static void ui_anim_compose_push(
    uint16_t *out,
    const uint16_t *from,
    const uint16_t *to,
    int progress_q8,
    ui_push_direction_t direction)
{
    if (!out ||
        !from ||
        !to) {
        return;
    }

    /*
     * Outside the round UI panel nothing needs to move.
     */
    memcpy(
        out,
        from,
        UI_FB_BYTES
    );

    const int x0 = SAFE_X;
    const int y0 = SAFE_Y;
    const int w = SAFE_W;
    const int h = SAFE_H;

    if (direction == UI_PUSH_LEFT ||
        direction == UI_PUSH_RIGHT) {
        const int shift =
            (w * progress_q8) / 256;

        const int old_w =
            w - shift;

        const int new_w =
            shift;

        for (int y = y0;
             y < y0 + h;
             ++y) {
            memset(
                &out[
                    (size_t)y * TYPEN_LCD_W +
                    (size_t)x0
                ],
                0,
                (size_t)w *
                    sizeof(uint16_t)
            );

            if (direction ==
                UI_PUSH_LEFT) {
                if (old_w > 0) {
                    ui_anim_copy_row_segment(
                        out,
                        x0,
                        from,
                        x0 + shift,
                        y,
                        old_w
                    );
                }

                if (new_w > 0) {
                    ui_anim_copy_row_segment(
                        out,
                        x0 + old_w,
                        to,
                        x0,
                        y,
                        new_w
                    );
                }

                if (shift > 2 &&
                    old_w > 1) {
                    out[
                        (size_t)y *
                            TYPEN_LCD_W +
                        (size_t)(x0 + old_w - 1)
                    ] = C_ACCENT_DIM;
                }
            } else {
                if (old_w > 0) {
                    ui_anim_copy_row_segment(
                        out,
                        x0 + shift,
                        from,
                        x0,
                        y,
                        old_w
                    );
                }

                if (new_w > 0) {
                    ui_anim_copy_row_segment(
                        out,
                        x0,
                        to,
                        x0 + old_w,
                        y,
                        new_w
                    );
                }

                if (shift > 2 &&
                    shift < w - 1) {
                    out[
                        (size_t)y *
                            TYPEN_LCD_W +
                        (size_t)(x0 + shift)
                    ] = C_ACCENT_DIM;
                }
            }
        }
    } else {
        const int shift =
            (h * progress_q8) / 256;

        const int old_h =
            h - shift;

        const int new_h =
            shift;

        for (int row = 0;
             row < h;
             ++row) {
            uint16_t *dst =
                &out[
                    (size_t)(y0 + row) *
                        TYPEN_LCD_W +
                    (size_t)x0
                ];

            memset(
                dst,
                0,
                (size_t)w *
                    sizeof(uint16_t)
            );
        }

        if (direction ==
            UI_PUSH_UP) {
            for (int row = 0;
                 row < old_h;
                 ++row) {
                memcpy(
                    &out[
                        (size_t)(y0 + row) *
                            TYPEN_LCD_W +
                        (size_t)x0
                    ],
                    &from[
                        (size_t)(y0 + shift + row) *
                            TYPEN_LCD_W +
                        (size_t)x0
                    ],
                    (size_t)w *
                        sizeof(uint16_t)
                );
            }

            for (int row = 0;
                 row < new_h;
                 ++row) {
                memcpy(
                    &out[
                        (size_t)(y0 + old_h + row) *
                            TYPEN_LCD_W +
                        (size_t)x0
                    ],
                    &to[
                        (size_t)(y0 + row) *
                            TYPEN_LCD_W +
                        (size_t)x0
                    ],
                    (size_t)w *
                        sizeof(uint16_t)
                );
            }
        } else {
            for (int row = 0;
                 row < old_h;
                 ++row) {
                memcpy(
                    &out[
                        (size_t)(y0 + shift + row) *
                            TYPEN_LCD_W +
                        (size_t)x0
                    ],
                    &from[
                        (size_t)(y0 + row) *
                            TYPEN_LCD_W +
                        (size_t)x0
                    ],
                    (size_t)w *
                        sizeof(uint16_t)
                );
            }

            for (int row = 0;
                 row < new_h;
                 ++row) {
                memcpy(
                    &out[
                        (size_t)(y0 + row) *
                            TYPEN_LCD_W +
                        (size_t)x0
                    ],
                    &to[
                        (size_t)(y0 + old_h + row) *
                            TYPEN_LCD_W +
                        (size_t)x0
                    ],
                    (size_t)w *
                        sizeof(uint16_t)
                );
            }
        }
    }
}

static void ui_anim_present_target_final(void)
{
    if (!s_anim.active ||
        !s_anim.to_frame) {
        return;
    }

    memcpy(
        s_fb,
        s_anim.to_frame,
        UI_FB_BYTES
    );

    present();
}

static void ui_anim_crossfade_to(
    ui_draw_fn_t draw_fn)
{
    if (!s_anim.active ||
        !draw_fn) {
        draw_fn();
        return;
    }

    s_ui_animating = true;

    ui_anim_snapshot_visible(
        s_anim.from_frame
    );

    ui_anim_render_to(
        s_anim.to_frame,
        draw_fn
    );

    for (int frame = 1;
         frame <= UI_ANIM_FRAMES;
         ++frame) {
        const unsigned alpha =
            (unsigned)ui_anim_ease_q8(
                frame,
                UI_ANIM_FRAMES
            );

        ui_anim_compose_crossfade(
            s_fb,
            s_anim.from_frame,
            s_anim.to_frame,
            alpha
        );

        present();
    }

    ui_anim_present_target_final();

    s_ui_animating = false;
}

static void ui_anim_push_to(
    ui_draw_fn_t draw_fn,
    ui_push_direction_t direction)
{
    if (!s_anim.active ||
        !draw_fn) {
        draw_fn();
        return;
    }

    s_ui_animating = true;

    ui_anim_snapshot_visible(
        s_anim.from_frame
    );

    ui_anim_render_to(
        s_anim.to_frame,
        draw_fn
    );

    for (int frame = 1;
         frame <= UI_ANIM_FRAMES;
         ++frame) {
        const int progress =
            ui_anim_ease_q8(
                frame,
                UI_ANIM_FRAMES
            );

        ui_anim_compose_push(
            s_fb,
            s_anim.from_frame,
            s_anim.to_frame,
            progress,
            direction
        );

        present();
    }

    ui_anim_present_target_final();

    s_ui_animating = false;
}

static void ui_anim_home_press(
    int row_y)
{
    if (!s_anim.active) {
        return;
    }

    s_ui_animating = true;
    s_home_pressed_y = row_y;

    draw_home();

    vTaskDelay(
        pdMS_TO_TICKS(
            UI_ANIM_PRESS_MS
        )
    );

    s_home_pressed_y = -1;
    s_ui_animating = false;
}

static void ui_anim_prepare_playback_target(void)
{
    const uint16_t *playback =
        ui_anim_playback_source();

    if (playback) {
        memcpy(
            s_anim.to_frame,
            playback,
            UI_FB_BYTES
        );
        return;
    }

    ui_anim_render_to(
        s_anim.to_frame,
        draw_idle
    );
}

static void ui_anim_close_to_playback(void)
{
    if (!s_anim.active) {
        return;
    }

    s_ui_animating = true;

    ui_anim_snapshot_visible(
        s_anim.from_frame
    );

    ui_anim_prepare_playback_target();

    for (int frame = 1;
         frame <= UI_ANIM_FRAMES;
         ++frame) {
        const unsigned alpha =
            (unsigned)ui_anim_ease_q8(
                frame,
                UI_ANIM_FRAMES
            );

        ui_anim_compose_crossfade(
            s_fb,
            s_anim.from_frame,
            s_anim.to_frame,
            alpha
        );

        present();
    }

    ui_anim_present_target_final();

    s_ui_animating = false;

    /*
     * Put playback FB0 back exactly as it was before the menu borrowed it.
     * The UI framebuffer is forced to scanout first so this memcpy can never
     * race the panel.
     */
    ui_anim_session_end();
}


static void ui_anim_startup_flourish(void)
{
    if (!s_anim.active) {
        draw_idle();
        return;
    }

    s_ui_animating = true;

    /*
     * The first visible boot mark is already on screen. Move directly into the
     * flourish while keeping the original TypeN identity fixed throughout.
     */
    for (int frame = 0;
         frame <= STARTUP_FLOURISH_FRAMES;
         ++frame) {
        const int progress =
            ui_anim_ease_q8(
                frame,
                STARTUP_FLOURISH_FRAMES
            );

        startup_flourish_scene(
            progress,
            frame ==
                STARTUP_FLOURISH_FRAMES
        );

        present();
    }

    s_ui_animating = false;
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
    s_media_pressed_index = (size_t)-1;
    s_media_confirm_visible = false;
    s_media_confirm_mode = MEDIA_CONFIRM_SET;
    s_factory_reset_confirm_visible = false;

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
        ui_anim_home_press(
            SAFE_Y + 146
        );

        ui_anim_push_to(
            draw_display_page,
            UI_PUSH_LEFT
        );
        return;
    }

    if (point_in(
            x, y,
            row_x, SAFE_Y + 220,
            row_w, row_h)) {
        s_media_page_index = 0;
        s_media_pressed_index = (size_t)-1;
        s_media_confirm_visible = false;
        s_media_confirm_mode = MEDIA_CONFIRM_SET;
        ui_anim_home_press(
            SAFE_Y + 220
        );

        ui_anim_push_to(
            draw_media_page,
            UI_PUSH_LEFT
        );
        return;
    }

    if (point_in(
            x, y,
            row_x, SAFE_Y + 294,
            row_w, row_h)) {
        s_factory_reset_confirm_visible = false;

        ui_anim_home_press(
            SAFE_Y + 294
        );

        ui_anim_push_to(
            draw_system_page,
            UI_PUSH_LEFT
        );
        return;
    }

    if (point_in(
            x, y,
            row_x, SAFE_Y + 368,
            row_w, row_h)) {
        ui_anim_home_press(
            SAFE_Y + 368
        );

        ui_anim_push_to(
            draw_about_page,
            UI_PUSH_LEFT
        );
        return;
    }

    if (point_in(
            x, y,
            SAFE_X + 190,
            SAFE_Y + 458,
            170,
            44)) {
        ui_anim_close_to_playback();
        close_menu();
    }
}

static size_t media_card_index_at_point(
    int x,
    int y)
{
    if (s_page != PAGE_MEDIA ||
        s_media_confirm_visible ||
        s_media_count == 0) {
        return (size_t)-1;
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
        const size_t index =
            first + slot;

        if (index >= s_media_count) {
            break;
        }

        if (point_in(
                x,
                y,
                card_x[slot],
                card_y[slot],
                MEDIA_CARD_W,
                MEDIA_CARD_H)) {
            return index;
        }
    }

    return (size_t)-1;
}

static void handle_subpage_tap(int x, int y)
{
    if (s_page == PAGE_SYSTEM &&
        s_factory_reset_confirm_visible) {

        if (point_in(
                x, y,
                SAFE_X + 72,
                SAFE_Y + 424,
                164,
                54)) {
            s_factory_reset_confirm_visible = false;

            ui_anim_push_to(
                draw_system_page,
                UI_PUSH_DOWN
            );
            return;
        }

        if (point_in(
                x, y,
                SAFE_X + SAFE_W - 72 - 164,
                SAFE_Y + 424,
                164,
                54)) {
            esp_err_t err =
                typen_settings_factory_reset();

            if (err == ESP_OK) {
                draw_shell(
                    "RESETTING",
                    "TYPE N"
                );

                draw_text_center(
                    400,
                    SAFE_Y + 246,
                    "SETTINGS CLEARED",
                    3,
                    C_ACCENT
                );

                draw_text_center(
                    400,
                    SAFE_Y + 300,
                    "REBOOTING",
                    2,
                    C_TEXT_DIM
                );

                present();

                vTaskDelay(
                    pdMS_TO_TICKS(300)
                );

                esp_restart();
            } else {
                ESP_LOGE(
                    TAG,
                    "Factory reset failed: %s",
                    esp_err_to_name(err)
                );

                s_factory_reset_confirm_visible = false;

                snprintf(
                    s_status,
                    sizeof(s_status),
                    "%s",
                    "RESET FAILED"
                );

                ui_anim_push_to(
                    draw_system_page,
                    UI_PUSH_DOWN
                );
            }

            return;
        }

        return;
    }

    if (s_page == PAGE_MEDIA &&
        s_media_confirm_visible) {

        if (point_in(
                x, y,
                SAFE_X + 72,
                SAFE_Y + 424,
                164,
                54)) {
            s_media_confirm_visible = false;

            ui_anim_push_to(
                draw_media_page,
                UI_PUSH_DOWN
            );
            return;
        }

        if (point_in(
                x, y,
                SAFE_X + SAFE_W - 72 - 164,
                SAFE_Y + 424,
                164,
                54)) {
            typen_media_item_t item;

            esp_err_t err =
                typen_media_get(
                    s_media_confirm_index,
                    &item
                );

            if (s_media_confirm_mode ==
                MEDIA_CONFIRM_DELETE) {
                if (err == ESP_OK) {
                    err =
                        typen_media_delete(
                            item.path
                        );
                }

                if (err == ESP_OK) {
                    ESP_LOGI(
                        TAG,
                        "Deleted from local media browser: %s",
                        item.path
                    );

                    typen_thumb_cache_reset();

                    s_media_count =
                        typen_media_count();

                    s_media_confirm_visible = false;
                    s_media_confirm_mode = MEDIA_CONFIRM_SET;

                    ui_anim_push_to(
                        draw_media_page,
                        UI_PUSH_DOWN
                    );
                } else {
                    ESP_LOGE(
                        TAG,
                        "Unable to delete media: %s",
                        esp_err_to_name(err)
                    );

                    draw_media_confirmation();
                }

                return;
            }

            if (err == ESP_OK) {
                err =
                    typen_settings_set_selected_media(
                        item.path
                    );
            }

            if (err == ESP_OK) {
                /*
                 * Finish the menu transition and give playback buffers back
                 * before queueing the new item. Static JPEG/PNG decoding can
                 * write playback FB0 even while presentation is paused.
                 */
                ui_anim_close_to_playback();
                close_menu();

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
            } else {
                ESP_LOGE(
                    TAG,
                    "Unable to set media: %s",
                    esp_err_to_name(err)
                );

                s_media_confirm_visible = false;
                s_media_confirm_mode = MEDIA_CONFIRM_SET;
                draw_media_page();
            }

            return;
        }

        return;
    }

    if (point_in(
            x, y,
            SAFE_X + 34,
            SAFE_Y + SAFE_H - 70,
            122,
            44)) {
        ui_anim_push_to(
            s_page == PAGE_DIAGNOSTICS
                ? draw_system_page
                : draw_home,
            UI_PUSH_RIGHT
        );
        return;
    }

    if (s_page == PAGE_SYSTEM) {
        const int action_x =
            SAFE_X + 56;

        const int action_y =
            SAFE_Y + 397;

        const int action_gap = 16;

        const int action_w =
            (SAFE_W - 112 - action_gap) / 2;

        if (point_in(
                x, y,
                action_x,
                action_y,
                action_w,
                54)) {
            ui_anim_push_to(
                draw_diagnostics_page,
                UI_PUSH_LEFT
            );
            return;
        }

        if (point_in(
                x, y,
                action_x +
                    action_w +
                    action_gap,
                action_y,
                action_w,
                54)) {
            s_factory_reset_confirm_visible = true;

            ui_anim_push_to(
                draw_factory_reset_confirmation,
                UI_PUSH_UP
            );
            return;
        }
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

            ui_anim_push_to(
                draw_media_page,
                UI_PUSH_RIGHT
            );
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

            ui_anim_push_to(
                draw_media_page,
                UI_PUSH_LEFT
            );
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

            const int delete_x =
                card_x[slot] + 11 +
                MEDIA_THUMB_W -
                MEDIA_DELETE_W -
                MEDIA_DELETE_MARGIN;

            const int delete_y =
                card_y[slot] + 9 +
                MEDIA_DELETE_MARGIN;

            if (point_in(
                    x, y,
                    delete_x,
                    delete_y,
                    MEDIA_DELETE_W,
                    MEDIA_DELETE_H)) {
                s_media_confirm_index = index;
                s_media_confirm_mode = MEDIA_CONFIRM_DELETE;
                s_media_confirm_visible = true;

                ui_anim_push_to(
                    draw_media_confirmation,
                    UI_PUSH_UP
                );
                return;
            }

            if (point_in(
                    x, y,
                    card_x[slot],
                    card_y[slot],
                    MEDIA_CARD_W,
                    MEDIA_CARD_H)) {
                s_media_confirm_index = index;
                s_media_confirm_mode = MEDIA_CONFIRM_SET;
                s_media_confirm_visible = true;

                ui_anim_push_to(
                    draw_media_confirmation,
                    UI_PUSH_UP
                );
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

static void restore_after_misc(void)
{
    esp_err_t err =
        typen_ui_easter_egg_exit();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Easter egg exit request failed: %s",
            esp_err_to_name(err)
        );
    }
}

static void touch_task(void *arg)
{
    (void)arg;

    bool was_down = false;
    bool long_fired = false;

    bool two_touch_active = false;
    bool suppress_until_release = false;
    TickType_t two_touch_tick = 0;

    uint16_t down_x = 0;
    uint16_t down_y = 0;
    uint16_t last_x = 0;
    uint16_t last_y = 0;

    TickType_t down_tick = 0;

    for (;;) {
        uint16_t xs[2] = {0};
        uint16_t ys[2] = {0};
        uint8_t point_count = 0;

        esp_err_t err =
            typen_board_touch_sample_points(
                xs,
                ys,
                2,
                &point_count
            );

        if (err != ESP_OK) {
            vTaskDelay(
                pdMS_TO_TICKS(
                    TOUCH_POLL_MS
                )
            );
            continue;
        }

        /*
         * While the Easter egg is running the normal UI remains dormant.
         * The touch task only watches the explicit bottom-center EXIT button.
         */
        if (typen_ui_misc_is_active()) {
            was_down = false;
            long_fired = false;
            two_touch_active = false;

            if (point_count > 0 &&
                typen_ui_misc_exit_hit(
                    xs[0],
                    ys[0]
                )) {
                restore_after_misc();
                suppress_until_release = true;
            }

            vTaskDelay(
                pdMS_TO_TICKS(
                    TOUCH_POLL_MS
                )
            );
            continue;
        }

        /*
         * The EXIT tap may still be physically held for a few samples after
         * the renderer closes. Consume that release so it cannot immediately
         * become the normal 700 ms menu gesture.
         */
        if (suppress_until_release) {
            if (point_count == 0) {
                suppress_until_release = false;
            }

            vTaskDelay(
                pdMS_TO_TICKS(
                    TOUCH_POLL_MS
                )
            );
            continue;
        }

        /*
         * Hidden trigger:
         * two simultaneous touch points held for roughly two seconds.
         *
         * Multi-touch is consumed here before the normal one-finger long
         * press so the 700 ms menu gesture cannot fire underneath it.
         */
        if (!s_menu_visible &&
            point_count >= 2) {
            was_down = false;
            long_fired = false;

            if (!two_touch_active) {
                two_touch_active = true;
                two_touch_tick =
                    xTaskGetTickCount();
            } else if (
                (xTaskGetTickCount() -
                 two_touch_tick) >=
                    pdMS_TO_TICKS(
                        MISC_LONG_PRESS_MS
                    )) {
                two_touch_active = false;

                esp_err_t misc_err =
                    typen_ui_easter_egg_start();

                if (misc_err != ESP_OK) {
                    ESP_LOGE(
                        TAG,
                        "Unable to start Easter egg: %s",
                        esp_err_to_name(misc_err)
                    );
                }
            }

            vTaskDelay(
                pdMS_TO_TICKS(
                    TOUCH_POLL_MS
                )
            );
            continue;
        }

        two_touch_active = false;

        const bool down =
            point_count > 0;

        const uint16_t x =
            down ? xs[0] : 0;

        const uint16_t y =
            down ? ys[0] : 0;

        if (down) {
            last_x = x;
            last_y = y;

            if (!was_down) {
                was_down = true;
                long_fired = false;
                down_x = x;
                down_y = y;
                down_tick =
                    xTaskGetTickCount();

                if (s_menu_visible &&
                    s_page == PAGE_MEDIA &&
                    !s_media_confirm_visible) {
                    const size_t pressed_index =
                        media_card_index_at_point(
                            x,
                            y
                        );

                    if (pressed_index !=
                        (size_t)-1) {
                        s_media_pressed_index =
                            pressed_index;
                        draw_media_page();
                    }
                }
            } else if (!s_menu_visible &&
                       !long_fired &&
                       (xTaskGetTickCount() -
                        down_tick) >=
                           pdMS_TO_TICKS(
                               LONG_PRESS_MS
                           )) {
                long_fired = true;

                typen_playback_pause(true);
                typen_thumb_cache_set_paused(false);

                s_menu_visible = true;

                if (ui_anim_session_begin()) {
                    ui_anim_crossfade_to(
                        draw_home
                    );
                } else {
                    s_fb =
                        typen_board_framebuffer();
                    draw_home();
                }

                ESP_LOGI(
                    TAG,
                    "Menu opened by long press"
                );
            }
        } else if (was_down) {
            int dx =
                (int)last_x -
                (int)down_x;

            int dy =
                (int)last_y -
                (int)down_y;

            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;

            const bool had_media_press =
                s_media_pressed_index !=
                (size_t)-1;

            s_media_pressed_index =
                (size_t)-1;

            if (s_menu_visible &&
                !long_fired &&
                dx <= TAP_MOVE_LIMIT &&
                dy <= TAP_MOVE_LIMIT) {
                handle_menu_tap(
                    last_x,
                    last_y
                );
            }

            if (had_media_press &&
                s_menu_visible &&
                s_page == PAGE_MEDIA &&
                !s_media_confirm_visible) {
                draw_media_page();
            }

            was_down = false;
            long_fired = false;
        }

        if (typen_thumb_cache_take_dirty()) {
            if (!s_ui_animating &&
                s_menu_visible &&
                s_page == PAGE_MEDIA) {
                if (s_media_confirm_visible) {
                    draw_media_confirmation();
                } else {
                    draw_media_page();
                }
            }
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                TOUCH_POLL_MS
            )
        );
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

    if (s_misc_control_lock == NULL) {
        s_misc_control_lock =
            xSemaphoreCreateMutex();

        if (s_misc_control_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
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
     * Preserve the original TypeN logo exactly and run one short decorative
     * 3D flourish around it. The final idle state remains the original splash.
     */
    if (ui_anim_session_begin()) {
        ui_anim_startup_flourish();
        ui_anim_session_end();
    } else {
        draw_idle();
    }

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

    if (!s_ui_animating &&
        s_menu_visible &&
        s_page == PAGE_SYSTEM &&
        !s_factory_reset_confirm_visible) {
        draw_system_page();
    }
}

void typen_ui_set_wifi_status(const char *text)
{
    if (text == NULL) {
        return;
    }

    snprintf(s_wifi, sizeof(s_wifi), "%s", text);

    if (!s_ui_animating &&
        s_menu_visible &&
        s_page == PAGE_SYSTEM &&
        !s_factory_reset_confirm_visible) {
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

    if (!s_ui_animating &&
        s_menu_visible &&
        (s_page == PAGE_MEDIA ||
         s_page == PAGE_SYSTEM)) {
        if (s_page == PAGE_MEDIA) {
            draw_media_page();
        } else if (!s_factory_reset_confirm_visible) {
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

    if (!s_ui_animating &&
        s_menu_visible &&
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

esp_err_t typen_ui_easter_egg_start(void)
{
    if (s_misc_control_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(
        s_misc_control_lock,
        portMAX_DELAY
    );

    if (typen_ui_misc_is_active()) {
        xSemaphoreGive(
            s_misc_control_lock
        );

        return ESP_OK;
    }

    /*
     * Web activation can happen while the local menu is open. Give the
     * animation engine's borrowed playback framebuffer back before the misc
     * renderer starts borrowing its own framebuffer.
     */
    if (s_anim.active) {
        ui_anim_session_end();
    }

    /*
     * Drop the local menu state before the renderer takes over so the touch
     * task cannot return to an invisible menu after the Easter egg exits.
     */
    s_menu_visible = false;
    s_page = PAGE_HOME;
    s_media_pressed_index = (size_t)-1;
    s_media_confirm_visible = false;
    s_media_confirm_mode =
        MEDIA_CONFIRM_SET;
    s_factory_reset_confirm_visible = false;

    typen_playback_pause(true);
    typen_thumb_cache_set_paused(true);

    esp_err_t err =
        typen_ui_misc_start();

    if (err != ESP_OK) {
        if (typen_playback_has_frame()) {
            (void)typen_playback_restore();
        } else {
            draw_idle();
        }

        typen_playback_pause(false);
    } else {
        ESP_LOGI(
            TAG,
            "Wireframe Easter egg opened"
        );
    }

    xSemaphoreGive(
        s_misc_control_lock
    );

    return err;
}

esp_err_t typen_ui_easter_egg_exit(void)
{
    if (s_misc_control_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(
        s_misc_control_lock,
        portMAX_DELAY
    );

    if (!typen_ui_misc_is_active()) {
        xSemaphoreGive(
            s_misc_control_lock
        );

        return ESP_OK;
    }

    typen_ui_misc_request_exit();

    while (typen_ui_misc_is_active()) {
        vTaskDelay(
            pdMS_TO_TICKS(5)
        );
    }

    if (typen_playback_has_frame()) {
        esp_err_t restore_err =
            typen_playback_restore();

        if (restore_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "playback restore after Easter egg failed: %s",
                esp_err_to_name(restore_err)
            );

            draw_idle();
        }
    } else {
        draw_idle();
    }

    typen_thumb_cache_set_paused(true);
    typen_playback_pause(false);

    ESP_LOGI(
        TAG,
        "Wireframe Easter egg closed"
    );

    xSemaphoreGive(
        s_misc_control_lock
    );

    return ESP_OK;
}

bool typen_ui_easter_egg_active(void)
{
    return typen_ui_misc_is_active();
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

    if (s_menu_visible) {
        ui_anim_close_to_playback();
        close_menu();
    } else if (s_anim.active) {
        ui_anim_session_end();
    }

    return typen_playback_set(item);
}
