#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "typen_board.h"
#include "typen_playback.h"
#include "typen_ui_misc.h"
#include "typen_ui_misc_data.h"

#define MISC_FRAME_MS              50
#define MISC_TASK_STACK          8192
#define MISC_TASK_PRIORITY          4

#define MISC_CENTER_X             400
#define MISC_CENTER_Y             400
#define MISC_CAMERA_DISTANCE    18.0f
#define MISC_FOCAL_LENGTH      900.0f
#define MISC_TWO_PI             6.28318530718f

#define EXIT_X                     320
#define EXIT_Y                     714
#define EXIT_W                     160
#define EXIT_H                      48

#define C_BLACK                  0x0000

/*
 * Darkone83 signature palette, sampled from the supplied logo:
 * near-black violet -> deep purple -> violet -> hot magenta -> pale pink.
 */
#define C_PANEL                  0x0802
#define C_PANEL_2                0x2807
#define C_LINE                   0x6091
#define C_ACCENT                 0xD91A
#define C_TEXT                   0xF67E

/* Fake bloom layers for the neon wireframe. */
#define C_GLOW_OUTER             0x206B
#define C_GLOW_INNER             0x6091
#define C_GLOW_FAR               0x9A79
#define C_GLOW_MID               0xD91A
#define C_GLOW_NEAR              0xF67E

typedef struct {
    int16_t x;
    int16_t y;
    int16_t depth;
    bool valid;
} projected_vertex_t;

static const char *TAG = "typen_ui_misc";

static volatile bool s_active = false;
static volatile bool s_exit_requested = false;
static TaskHandle_t s_task = NULL;

static inline void put_px(
    uint16_t *fb,
    int x,
    int y,
    uint16_t color)
{
    if ((unsigned)x < TYPEN_LCD_W &&
        (unsigned)y < TYPEN_LCD_H) {
        fb[y * TYPEN_LCD_W + x] = color;
    }
}

static void fill_rect(
    uint16_t *fb,
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
        uint16_t *row =
            &fb[yy * TYPEN_LCD_W + x];

        for (int xx = 0; xx < w; ++xx) {
            row[xx] = color;
        }
    }
}

static int corner_inset(
    int radius,
    int dy)
{
    const int rr = radius * radius;
    int inset = 0;

    while (inset < radius) {
        const int dx =
            radius - inset - 1;

        if ((dx * dx + dy * dy) <= rr) {
            break;
        }

        ++inset;
    }

    return inset;
}

static void fill_round_rect(
    uint16_t *fb,
    int x,
    int y,
    int w,
    int h,
    int radius,
    uint16_t color)
{
    if (radius < 1) {
        fill_rect(
            fb,
            x,
            y,
            w,
            h,
            color
        );
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
            inset =
                corner_inset(
                    radius,
                    radius - yy - 1
                );
        } else if (yy >= h - radius) {
            inset =
                corner_inset(
                    radius,
                    yy - (h - radius)
                );
        }

        fill_rect(
            fb,
            x + inset,
            y + yy,
            w - inset * 2,
            1,
            color
        );
    }
}

static void stroke_round_rect(
    uint16_t *fb,
    int x,
    int y,
    int w,
    int h,
    int radius,
    int thickness,
    uint16_t border,
    uint16_t inside)
{
    fill_round_rect(
        fb,
        x,
        y,
        w,
        h,
        radius,
        border
    );

    if (thickness > 0 &&
        w > thickness * 2 &&
        h > thickness * 2) {
        fill_round_rect(
            fb,
            x + thickness,
            y + thickness,
            w - thickness * 2,
            h - thickness * 2,
            radius > thickness
                ? radius - thickness
                : 1,
            inside
        );
    }
}

static const uint8_t *exit_glyph(
    char c)
{
    switch (c) {
        case 'E': {
            static const uint8_t g[7] = {
                0x1F, 0x10, 0x10, 0x1E,
                0x10, 0x10, 0x1F
            };
            return g;
        }

        case 'X': {
            static const uint8_t g[7] = {
                0x11, 0x11, 0x0A, 0x04,
                0x0A, 0x11, 0x11
            };
            return g;
        }

        case 'I': {
            static const uint8_t g[7] = {
                0x1F, 0x04, 0x04, 0x04,
                0x04, 0x04, 0x1F
            };
            return g;
        }

        case 'T': {
            static const uint8_t g[7] = {
                0x1F, 0x04, 0x04, 0x04,
                0x04, 0x04, 0x04
            };
            return g;
        }

        default: {
            static const uint8_t g[7] = {
                0, 0, 0, 0, 0, 0, 0
            };
            return g;
        }
    }
}

static void draw_char(
    uint16_t *fb,
    int x,
    int y,
    char c,
    int scale,
    uint16_t color)
{
    const uint8_t *rows =
        exit_glyph(c);

    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (rows[row] &
                (1U << (4 - col))) {
                fill_rect(
                    fb,
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

static void draw_exit_button(
    uint16_t *fb)
{
    stroke_round_rect(
        fb,
        EXIT_X,
        EXIT_Y,
        EXIT_W,
        EXIT_H,
        14,
        2,
        C_LINE,
        C_PANEL
    );

    fill_round_rect(
        fb,
        EXIT_X + 10,
        EXIT_Y + 10,
        4,
        EXIT_H - 20,
        2,
        C_ACCENT
    );

    const char text[] = "EXIT";
    const int scale = 2;
    const int width =
        (int)(sizeof(text) - 1U) *
        6 * scale -
        scale;

    int x =
        EXIT_X +
        (EXIT_W - width) / 2;

    const int y =
        EXIT_Y +
        (EXIT_H - 7 * scale) / 2;

    for (size_t i = 0;
         i < sizeof(text) - 1U;
         ++i) {
        draw_char(
            fb,
            x,
            y,
            text[i],
            scale,
            C_TEXT
        );

        x += 6 * scale;
    }
}

static void draw_line(
    uint16_t *fb,
    int x0,
    int y0,
    int x1,
    int y1,
    int offset_x,
    int offset_y,
    uint16_t color)
{
    x0 += offset_x;
    y0 += offset_y;
    x1 += offset_x;
    y1 += offset_y;

    int dx = abs(x1 - x0);
    const int sx =
        x0 < x1 ? 1 : -1;

    int dy = -abs(y1 - y0);
    const int sy =
        y0 < y1 ? 1 : -1;

    int err = dx + dy;

    for (;;) {
        put_px(
            fb,
            x0,
            y0,
            color
        );

        if (x0 == x1 &&
            y0 == y1) {
            break;
        }

        const int e2 = err * 2;

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

static uint16_t depth_color(
    int depth)
{
    /*
     * Negative transformed Z is closer to the camera in this projection.
     * Brightening near edges gives the flat wireframe some useful volume.
     */
    if (depth < -350) {
        return C_GLOW_NEAR;
    }

    if (depth > 350) {
        return C_GLOW_FAR;
    }

    return C_GLOW_MID;
}

static void project_vertices(
    projected_vertex_t *projected,
    float angle)
{
    const float sy = sinf(angle);
    const float cy = cosf(angle);

    const float tilt =
        0.20f +
        sinf(angle * 0.53f) *
        0.06f;

    const float sx = sinf(tilt);
    const float cx = cosf(tilt);

    for (size_t i = 0;
         i < TYPEN_UI_MISC_VERTEX_COUNT;
         ++i) {
        const typen_ui_misc_vertex_t *v =
            &s_typen_ui_misc_vertices[i];

        const float x =
            (float)v->x /
            TYPEN_UI_MISC_VERTEX_Q;

        const float y =
            (float)v->y /
            TYPEN_UI_MISC_VERTEX_Q;

        const float z =
            (float)v->z /
            TYPEN_UI_MISC_VERTEX_Q;

        const float x1 =
            x * cy +
            z * sy;

        const float z1 =
            -x * sy +
            z * cy;

        const float y2 =
            y * cx -
            z1 * sx;

        const float z2 =
            y * sx +
            z1 * cx;

        const float denominator =
            MISC_CAMERA_DISTANCE +
            z2;

        if (denominator <= 1.0f) {
            projected[i].valid = false;
            continue;
        }

        const float scale =
            MISC_FOCAL_LENGTH /
            denominator;

        const float screen_x =
            MISC_CENTER_X +
            x1 * scale;

        const float screen_y =
            MISC_CENTER_Y -
            y2 * scale;

        projected[i].x =
            (int16_t)(
                screen_x +
                (screen_x >= 0.0f
                    ? 0.5f
                    : -0.5f)
            );

        projected[i].y =
            (int16_t)(
                screen_y +
                (screen_y >= 0.0f
                    ? 0.5f
                    : -0.5f)
            );

        projected[i].depth =
            (int16_t)(z2 * 256.0f);

        projected[i].valid = true;
    }
}

static void draw_wireframe(
    uint16_t *fb,
    const projected_vertex_t *p)
{
    /*
     * Halo first. The bright core is drawn afterward so overlapping halo
     * pixels never dull the actual wireframe.
     */
    for (size_t i = 0;
         i < TYPEN_UI_MISC_EDGE_COUNT;
         ++i) {
        const typen_ui_misc_edge_t *edge =
            &s_typen_ui_misc_edges[i];

        const projected_vertex_t *a =
            &p[edge->a];

        const projected_vertex_t *b =
            &p[edge->b];

        if (!a->valid ||
            !b->valid) {
            continue;
        }

        draw_line(
            fb,
            a->x, a->y,
            b->x, b->y,
            -2, 0,
            C_GLOW_OUTER
        );

        draw_line(
            fb,
            a->x, a->y,
            b->x, b->y,
            2, 0,
            C_GLOW_OUTER
        );

        draw_line(
            fb,
            a->x, a->y,
            b->x, b->y,
            0, -2,
            C_GLOW_OUTER
        );

        draw_line(
            fb,
            a->x, a->y,
            b->x, b->y,
            0, 2,
            C_GLOW_OUTER
        );

        draw_line(
            fb,
            a->x, a->y,
            b->x, b->y,
            -1, 0,
            C_GLOW_INNER
        );

        draw_line(
            fb,
            a->x, a->y,
            b->x, b->y,
            1, 0,
            C_GLOW_INNER
        );

        draw_line(
            fb,
            a->x, a->y,
            b->x, b->y,
            0, -1,
            C_GLOW_INNER
        );

        draw_line(
            fb,
            a->x, a->y,
            b->x, b->y,
            0, 1,
            C_GLOW_INNER
        );
    }

    for (size_t i = 0;
         i < TYPEN_UI_MISC_EDGE_COUNT;
         ++i) {
        const typen_ui_misc_edge_t *edge =
            &s_typen_ui_misc_edges[i];

        const projected_vertex_t *a =
            &p[edge->a];

        const projected_vertex_t *b =
            &p[edge->b];

        if (!a->valid ||
            !b->valid) {
            continue;
        }

        const int depth =
            ((int)a->depth +
             (int)b->depth) /
            2;

        draw_line(
            fb,
            a->x, a->y,
            b->x, b->y,
            0, 0,
            depth_color(depth)
        );
    }
}

static void misc_task(
    void *arg)
{
    (void)arg;

    /*
     * Render into two native DPI framebuffers and page-flip between them.
     *
     * The old renderer drew each new frame directly into FB0 while FB0 was
     * still being scanned by the panel. That makes the panel see a partially
     * cleared / partially redrawn frame and is the source of the visible
     * flicker.
     *
     * Playback is already paused before this task starts, so FB1 is safe to
     * borrow temporarily. Its contents are backed up and restored before the
     * task exits so the normal playback restore path remains unchanged.
     */
    uint16_t *ui_fb =
        typen_board_framebuffer();

    uint16_t *alt_fb =
        typen_board_playback_buffer(0);

    const size_t fb_bytes =
        (size_t)TYPEN_LCD_W *
        (size_t)TYPEN_LCD_H *
        sizeof(uint16_t);

    projected_vertex_t *projected =
        (projected_vertex_t *)
        heap_caps_malloc(
            TYPEN_UI_MISC_VERTEX_COUNT *
                sizeof(projected_vertex_t),
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    uint16_t *alt_backup =
        (uint16_t *)
        heap_caps_malloc(
            fb_bytes,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (!ui_fb ||
        !alt_fb ||
        !projected ||
        !alt_backup) {
        ESP_LOGE(
            TAG,
            "Unable to allocate wireframe renderer"
        );

        if (projected) {
            heap_caps_free(projected);
        }

        if (alt_backup) {
            heap_caps_free(alt_backup);
        }

        s_active = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    memcpy(
        alt_backup,
        alt_fb,
        fb_bytes
    );

    uint16_t *render_fb[2] = {
        ui_fb,
        alt_fb
    };

    /*
     * Start on a framebuffer that is not currently visible. With media on
     * screen, FB0 (UI) is inactive. With no playback frame, FB0 owns the idle
     * screen, so start on the borrowed playback buffer instead.
     */
    size_t render_index =
        typen_playback_has_frame()
            ? 0U
            : 1U;

    uint16_t *displayed_fb = NULL;
    float angle = 0.0f;

    ESP_LOGI(
        TAG,
        "Wireframe Easter egg started: vertices=%u edges=%u",
        (unsigned)TYPEN_UI_MISC_VERTEX_COUNT,
        (unsigned)TYPEN_UI_MISC_EDGE_COUNT
    );

    while (!s_exit_requested) {
        uint16_t *fb =
            render_fb[render_index];

        memset(
            fb,
            0,
            fb_bytes
        );

        project_vertices(
            projected,
            angle
        );

        draw_wireframe(
            fb,
            projected
        );

        draw_exit_button(
            fb
        );

        esp_err_t err =
            typen_board_present_buffer(
                fb
            );

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "wireframe present failed: %s",
                esp_err_to_name(err)
            );
        } else {
            displayed_fb = fb;
            render_index ^= 1U;
        }

        angle += 0.035f;

        if (angle >= MISC_TWO_PI) {
            angle -= MISC_TWO_PI;
        }

        /*
         * Keep the original ~20 FPS target. The full-frame delay also gives
         * the DPI driver several scan periods to complete the page flip before
         * the previously displayed framebuffer is touched again.
         */
        vTaskDelay(
            pdMS_TO_TICKS(MISC_FRAME_MS)
        );
    }

    /*
     * Never restore the borrowed playback framebuffer while it is still the
     * visible scanout buffer. Flip back to FB0 first, wait a little over one
     * 64.9 Hz display period, then put FB1 back exactly as we found it.
     */
    if (displayed_fb == alt_fb) {
        esp_err_t err =
            typen_board_present_buffer(
                ui_fb
            );

        if (err == ESP_OK) {
            vTaskDelay(
                pdMS_TO_TICKS(20)
            );
        } else {
            ESP_LOGW(
                TAG,
                "final UI-buffer flip failed: %s",
                esp_err_to_name(err)
            );
        }
    }

    memcpy(
        alt_fb,
        alt_backup,
        fb_bytes
    );

    heap_caps_free(
        alt_backup
    );

    heap_caps_free(
        projected
    );

    s_task = NULL;
    s_active = false;

    ESP_LOGI(
        TAG,
        "Wireframe Easter egg stopped"
    );

    vTaskDelete(NULL);
}

esp_err_t typen_ui_misc_start(void)
{
    if (s_active ||
        s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_exit_requested = false;
    s_active = true;

    BaseType_t ok =
        xTaskCreate(
            misc_task,
            "typen_misc_3d",
            MISC_TASK_STACK,
            NULL,
            MISC_TASK_PRIORITY,
            &s_task
        );

    if (ok != pdPASS) {
        s_task = NULL;
        s_active = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void typen_ui_misc_request_exit(void)
{
    s_exit_requested = true;
}

bool typen_ui_misc_is_active(void)
{
    return s_active;
}

bool typen_ui_misc_exit_hit(
    uint16_t x,
    uint16_t y)
{
    return x >= EXIT_X &&
           x < EXIT_X + EXIT_W &&
           y >= EXIT_Y &&
           y < EXIT_Y + EXIT_H;
}
