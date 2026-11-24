#include "lcd.h"

#include <math.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

static const char *TAG = "lcd";

#define PIN_LCD_MOSI 7
#define PIN_LCD_SCLK 6
#define PIN_LCD_CS   5
#define PIN_LCD_DC   4
#define PIN_LCD_RST  8
#define PIN_LCD_BL   15

#define COL_BG      0x0000
#define COL_FG      0xFFFF
#define COL_DIM     0x4208
#define COL_GREEN   0x07E0
#define COL_RED     0xF800
#define COL_YELLOW  0xFFE0
#define COL_BLUE    0x001F

static esp_lcd_panel_handle_t s_panel;
static uint16_t s_line[LCD_H_RES];

esp_err_t lcd_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &dev, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 20)); /* 240x280 panel inside the 240x320 controller RAM */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(PIN_LCD_BL, 1);

    lcd_fill(COL_BG);
    ESP_LOGI(TAG, "ST7789 %dx%d ready", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

void lcd_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_panel || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_H_RES) { w = LCD_H_RES - x; }
    if (y + h > LCD_V_RES) { h = LCD_V_RES - y; }
    if (w <= 0 || h <= 0) {
        return;
    }
    int chunk = w > LCD_H_RES ? LCD_H_RES : w;
    for (int i = 0; i < chunk; i++) {
        s_line[i] = color;
    }
    for (int row = 0; row < h; row++) {
        esp_lcd_panel_draw_bitmap(s_panel, x, y + row, x + chunk, y + row + 1, s_line);
    }
}

void lcd_fill(uint16_t color)
{
    lcd_rect(0, 0, LCD_H_RES, LCD_V_RES, color);
}

/* 7-segment digit, segments a..g. */
static const uint8_t SEG_MAP[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

static void seg7(int x, int y, int w, int h, int digit, uint16_t color)
{
    if (digit < 0 || digit > 9) {
        digit = 0;
    }
    uint8_t m = SEG_MAP[digit];
    int t = (w > h ? h : w) / 7; /* segment thickness */
    if (t < 3) {
        t = 3;
    }
    if (m & 0x01) lcd_rect(x, y, w, t, color);                              /* a */
    if (m & 0x02) lcd_rect(x + w - t, y, t, h / 2, color);                  /* b */
    if (m & 0x04) lcd_rect(x + w - t, y + h / 2, t, h / 2, color);          /* c */
    if (m & 0x08) lcd_rect(x, y + h - t, w, t, color);                      /* d */
    if (m & 0x10) lcd_rect(x, y + h / 2, t, h / 2, color);                  /* e */
    if (m & 0x20) lcd_rect(x, y, t, h / 2, color);                          /* f */
    if (m & 0x40) lcd_rect(x, y + h / 2, w, t, color);                      /* g */
}

static void bar(int x, int y, int w, int h, float value, float full_scale, uint16_t color)
{
    int mid = x + w / 2;
    int max_half = w / 2 - 2;
    float v = value / full_scale;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    int len = (int)(max_half * fabsf(v));
    lcd_rect(x, y, w, h, COL_DIM);
    if (len > 0) {
        if (v >= 0) {
            lcd_rect(mid, y + 2, len, h - 4, color);
        } else {
            lcd_rect(mid - len, y + 2, len, h - 4, color);
        }
    }
    lcd_rect(mid, y, 1, h, COL_FG);
}

void lcd_status_page(const bool hall[3], bool clutch, int state,
                     float bias_dps, float roll_rate, float pitch_rate)
{
    if (!s_panel) {
        return;
    }

    /* Clear and draw the three hall squares. */
    lcd_fill(COL_BG);
    for (int i = 0; i < 3; i++) {
        int x = 16 + i * 74;
        lcd_rect(x, 16, 58, 58, hall[i] ? COL_GREEN : COL_DIM);
        lcd_rect(x + 4, 78, 50, 4, COL_FG); /* underline marker */
    }

    /* Clutch indicator + state digit. */
    lcd_rect(16, 96, 58, 58, clutch ? COL_GREEN : COL_RED);
    seg7(110, 96, 58, 96, state, COL_YELLOW);

    /* Gyro bias (integer, 0..99) as two digits. */
    int b = (int)(bias_dps + 0.5f);
    if (b > 99) b = 99;
    seg7(16, 176, 40, 64, b / 10, COL_BLUE);
    seg7(64, 176, 40, 64, b % 10, COL_BLUE);

    /* Roll / pitch bars, +-100 dps full scale. */
    bar(16, 250, 208, 10, roll_rate, 100.0f, COL_GREEN);
    bar(16, 264, 208, 10, pitch_rate, 100.0f, COL_YELLOW);
}
