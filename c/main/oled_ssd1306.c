#include "oled_ssd1306.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

static const char *TAG_OLED = "oled";

static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static uint8_t s_fb[OLED_H_RES * OLED_V_RES / 8];

extern const uint8_t font5x7_96[96 * 5];

static void fb_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

static void fb_set_px(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_H_RES || y < 0 || y >= OLED_V_RES) {
        return;
    }
    int page = y / 8;
    int bit = y % 8;
    size_t idx = (size_t)page * OLED_H_RES + (size_t)x;
    if (on) {
        s_fb[idx] |= (uint8_t)(1u << bit);
    } else {
        s_fb[idx] &= (uint8_t)~(1u << bit);
    }
}

static void draw_char(int x0, int y0, char c)
{
    if (c < 32 || c > 127) {
        c = ' ';
    }
    const uint8_t *glyph = &font5x7_96[(size_t)(c - 32) * 5];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            bool on = (bits & (1u << row)) != 0;
            fb_set_px(x0 + col, y0 + row, on);
        }
    }
}

static void draw_str(int x0, int y0, const char *s, int max_px)
{
    int x = x0;
    for (; *s && x < max_px - 6; s++) {
        draw_char(x, y0, *s);
        x += 6;
    }
}

void oled_show_three_lines(const char *line1, const char *line2, const char *line3)
{
    if (!s_panel) {
        return;
    }
    fb_clear();
    draw_str(0, 0, line1 ? line1 : "", OLED_H_RES);
    draw_str(0, 8, line2 ? line2 : "", OLED_H_RES);
    draw_str(0, 16, line3 ? line3 : "", OLED_H_RES);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_H_RES, OLED_V_RES, s_fb);
}

esp_err_t oled_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = OLED_PIN_SCL,
        .sda_io_num = OLED_PIN_SDA,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG_OLED, "i2c bus");

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = OLED_I2C_ADDR,
        .scl_speed_hz = 400 * 1000,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &s_io), TAG_OLED, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
    };
    esp_lcd_panel_ssd1306_config_t ssd1306_cfg = { .height = OLED_V_RES };
    panel_cfg.vendor_config = &ssd1306_cfg;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(s_io, &panel_cfg, &s_panel), TAG_OLED, "ssd1306");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG_OLED, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG_OLED, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG_OLED, "on");

    fb_clear();
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_H_RES, OLED_V_RES, s_fb), TAG_OLED, "blank");
    return ESP_OK;
}
