#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_lcd_jd9365.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"

#include "driver/i2c_master.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/esp32_p4_platform.h"

#include "typen_board.h"

#define TYPEN_DSI_LANES              2
#define TYPEN_DSI_LANE_MBPS          1200

/*
 * Waveshare's 800x800 macro requests 80 MHz. On ESP32-P4 the default DPI
 * source is PLL_F240M with an integer divider, so 48 MHz is exact (240/5).
 *
 * With the stock 880x840 total timing this is ~64.9 Hz:
 *     48,000,000 / 880 / 840 ~= 64.9 Hz
 *
 * This is far closer to a normal display refresh than the stock 80 MHz
 * (~108 Hz), and cuts continuous PSRAM scanout bandwidth by 40%.
 */
#define TYPEN_DPI_CLOCK_MHZ          48

#define TYPEN_DPI_FB_COUNT           3

#define TYPEN_MIPI_PHY_LDO_CHAN      3
#define TYPEN_MIPI_PHY_LDO_MV        2500

static const char *TAG = "typen_board";

static esp_ldo_channel_handle_t s_dsi_ldo = NULL;
static esp_lcd_dsi_bus_handle_t s_dsi_bus = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;

static esp_lcd_touch_handle_t s_touch = NULL;
static esp_lcd_panel_io_handle_t s_touch_io = NULL;

static uint16_t *s_fbs[TYPEN_DPI_FB_COUNT] = {0};
static uint32_t s_first_frame_ms = 0;

static void draw_early_boot_frame(uint16_t *fb)
{
    const uint16_t bg = 0x0000;
    const uint16_t mark = 0x8010;
    const uint16_t bright = 0xF81F;

    for (int i = 0; i < TYPEN_LCD_W * TYPEN_LCD_H; ++i) {
        fb[i] = bg;
    }

    const int cx = 400;
    const int cy = 400;
    const int r_outer = 90;
    const int r_inner = 82;

    for (int y = cy - r_outer; y <= cy + r_outer; ++y) {
        for (int x = cx - r_outer; x <= cx + r_outer; ++x) {
            const int dx = x - cx;
            const int dy = y - cy;
            const int d2 = dx * dx + dy * dy;

            if (d2 <= r_outer * r_outer &&
                d2 >= r_inner * r_inner) {
                fb[y * TYPEN_LCD_W + x] = mark;
            }
        }
    }

    for (int x = 370; x <= 430; ++x) {
        fb[400 * TYPEN_LCD_W + x] = bright;
    }
}

static esp_err_t create_display(void)
{
    /*
     * Keep the same proven Waveshare/JD9365 path, but build it locally so
     * TypeN can override only the DPI pixel clock and framebuffer count.
     *
     * No panel preflight/probe is added here.
     */
    ESP_RETURN_ON_ERROR(
        bsp_display_brightness_init(),
        TAG,
        "brightness/I2C init failed"
    );

    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = TYPEN_MIPI_PHY_LDO_CHAN,
        .voltage_mv = TYPEN_MIPI_PHY_LDO_MV,
    };

    ESP_RETURN_ON_ERROR(
        esp_ldo_acquire_channel(
            &ldo_cfg,
            &s_dsi_ldo
        ),
        TAG,
        "DSI PHY LDO failed"
    );

    esp_lcd_dsi_bus_config_t bus_cfg =
        JD9365_PANEL_BUS_DSI_2CH_CONFIG();

    bus_cfg.num_data_lanes =
        TYPEN_DSI_LANES;

    bus_cfg.lane_bit_rate_mbps =
        TYPEN_DSI_LANE_MBPS;

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_dsi_bus(
            &bus_cfg,
            &s_dsi_bus
        ),
        TAG,
        "DSI bus failed"
    );

    esp_lcd_dbi_io_config_t dbi_cfg =
        JD9365_PANEL_IO_DBI_CONFIG();

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_dbi(
            s_dsi_bus,
            &dbi_cfg,
            &s_panel_io
        ),
        TAG,
        "DBI IO failed"
    );

    esp_lcd_dpi_panel_config_t dpi_cfg =
        JD9365_800_800_PANEL_60HZ_DPI_CONFIG(
            LCD_COLOR_PIXEL_FORMAT_RGB565
        );

    dpi_cfg.dpi_clock_freq_mhz =
        TYPEN_DPI_CLOCK_MHZ;

    dpi_cfg.num_fbs =
        TYPEN_DPI_FB_COUNT;

    jd9365_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num = TYPEN_DSI_LANES,
        },
    };

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_jd9365(
            s_panel_io,
            &panel_cfg,
            &s_panel
        ),
        TAG,
        "JD9365 create failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_reset(s_panel),
        TAG,
        "JD9365 reset failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_init(s_panel),
        TAG,
        "JD9365 init failed"
    );

    void *fb0 = NULL;
    void *fb1 = NULL;
    void *fb2 = NULL;

    ESP_RETURN_ON_ERROR(
        esp_lcd_dpi_panel_get_frame_buffer(
            s_panel,
            TYPEN_DPI_FB_COUNT,
            &fb0,
            &fb1,
            &fb2
        ),
        TAG,
        "get DPI framebuffers failed"
    );

    s_fbs[0] = (uint16_t *)fb0;
    s_fbs[1] = (uint16_t *)fb1;
    s_fbs[2] = (uint16_t *)fb2;

    if (!s_fbs[0] || !s_fbs[1] || !s_fbs[2]) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "JD9365: 800x800 RGB565, DPI=%d MHz (~64.9 Hz), DSI=%d Mbps/lane",
        TYPEN_DPI_CLOCK_MHZ,
        TYPEN_DSI_LANE_MBPS
    );

    ESP_LOGI(
        TAG,
        "DPI FB0(UI)=%p FB1(playback-A)=%p FB2(playback-B)=%p",
        (void *)s_fbs[0],
        (void *)s_fbs[1],
        (void *)s_fbs[2]
    );

    return ESP_OK;
}

esp_err_t typen_board_display_early_start(void)
{
    const int64_t t0 =
        esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "EARLY: initialize JD9365 display"
    );

    ESP_RETURN_ON_ERROR(
        create_display(),
        TAG,
        "display creation failed"
    );

    draw_early_boot_frame(
        s_fbs[0]
    );

    ESP_RETURN_ON_ERROR(
        typen_board_present(),
        TAG,
        "first framebuffer present failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(
            s_panel,
            true
        ),
        TAG,
        "display enable failed"
    );

    ESP_RETURN_ON_ERROR(
        bsp_display_brightness_set(40),
        TAG,
        "backlight failed"
    );

    s_first_frame_ms =
        (uint32_t)(
            (esp_timer_get_time() - t0) /
            1000
        );

    ESP_LOGI(
        TAG,
        "FIRST VISIBLE FRAME: %u ms",
        (unsigned)s_first_frame_ms
    );

    return ESP_OK;
}

static uint8_t find_touch_address(
    i2c_master_bus_handle_t bus)
{
    if (i2c_master_probe(
            bus,
            0x5D,
            100) == ESP_OK) {
        return 0x5D;
    }

    if (i2c_master_probe(
            bus,
            0x14,
            100) == ESP_OK) {
        return 0x14;
    }

    return 0;
}

esp_err_t typen_board_ui_start(void)
{
    ESP_LOGI(
        TAG,
        "Initialize raw GT9xx touch"
    );

    ESP_RETURN_ON_ERROR(
        bsp_i2c_init(),
        TAG,
        "bsp_i2c_init failed"
    );

    i2c_master_bus_handle_t bus =
        bsp_i2c_get_handle();

    if (!bus) {
        return ESP_FAIL;
    }

    const uint8_t touch_addr =
        find_touch_address(bus);

    if (touch_addr == 0) {
        ESP_LOGE(
            TAG,
            "Touch not found at 0x5D or 0x14"
        );
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_i2c_config_t io_cfg =
        ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();

    io_cfg.dev_addr =
        touch_addr;

    io_cfg.scl_speed_hz =
        CONFIG_BSP_I2C_CLK_SPEED_HZ;

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(
            bus,
            &io_cfg,
            &s_touch_io
        ),
        TAG,
        "touch I2C IO failed"
    );

    const esp_lcd_touch_config_t touch_cfg = {
        .x_max = TYPEN_LCD_W,
        .y_max = TYPEN_LCD_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(
            s_touch_io,
            &touch_cfg,
            &s_touch
        ),
        TAG,
        "GT9xx touch init failed"
    );

    return ESP_OK;
}

uint16_t *typen_board_framebuffer(void)
{
    return s_fbs[0];
}

size_t typen_board_playback_buffer_count(void)
{
    if (!s_fbs[1] || !s_fbs[2]) {
        return 0U;
    }

    return 2U;
}

uint16_t *typen_board_playback_buffer(
    size_t index)
{
    if (index >= 2U) {
        return NULL;
    }

    return s_fbs[index + 1U];
}

esp_err_t typen_board_present_buffer_rows(
    uint16_t *buffer,
    int y_start,
    int y_end)
{
    if (!s_panel || !buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    if (y_start < 0) {
        y_start = 0;
    }

    if (y_end > TYPEN_LCD_H) {
        y_end = TYPEN_LCD_H;
    }

    if (y_end <= y_start) {
        return ESP_OK;
    }

    return esp_lcd_panel_draw_bitmap(
        s_panel,
        0,
        y_start,
        TYPEN_LCD_W,
        y_end,
        buffer
    );
}

esp_err_t typen_board_present_buffer(
    uint16_t *buffer)
{
    return typen_board_present_buffer_rows(
        buffer,
        0,
        TYPEN_LCD_H
    );
}

esp_err_t typen_board_present(void)
{
    return typen_board_present_buffer(
        s_fbs[0]
    );
}

esp_err_t typen_board_touch_sample_points(
    uint16_t *x,
    uint16_t *y,
    uint8_t max_points,
    uint8_t *point_count)
{
    if (!x ||
        !y ||
        !point_count ||
        max_points == 0 ||
        max_points > TYPEN_TOUCH_MAX_POINTS) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_touch) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_read_data(
            s_touch
        ),
        TAG,
        "touch read failed"
    );

    uint16_t strength[TYPEN_TOUCH_MAX_POINTS] = {0};
    uint8_t count = 0;

    const bool touched =
        esp_lcd_touch_get_coordinates(
            s_touch,
            x,
            y,
            strength,
            &count,
            max_points
        );

    *point_count =
        touched
            ? count
            : 0;

    return ESP_OK;
}

esp_err_t typen_board_touch_sample(
    uint16_t *x,
    uint16_t *y,
    bool *pressed)
{
    if (!x ||
        !y ||
        !pressed) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t xs[1] = {0};
    uint16_t ys[1] = {0};
    uint8_t count = 0;

    esp_err_t err =
        typen_board_touch_sample_points(
            xs,
            ys,
            1,
            &count
        );

    if (err != ESP_OK) {
        return err;
    }

    if (count > 0) {
        *x = xs[0];
        *y = ys[0];
        *pressed = true;
    } else {
        *pressed = false;
    }

    return ESP_OK;
}

esp_err_t typen_board_set_brightness(
    uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    return bsp_display_brightness_set(
        percent
    );
}

uint32_t typen_board_first_frame_ms(void)
{
    return s_first_frame_ms;
}
