#include "lcd.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "font12x24.h"
#include "font10x20.h"

static const char *TAG = "lcd";

#define PIN_LCD_MOSI 7
#define PIN_LCD_SCLK 6
#define PIN_LCD_CS   5
#define PIN_LCD_DC   4
#define PIN_LCD_RST  8
#define PIN_LCD_BL   15

#define BAND_ROWS   96
#define BAR_FULL    100.0f
#define PAGE_MS     200

static uint16_t rgb(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#define COL_BG      rgb(10, 14, 20)
#define COL_CARD    rgb(22, 28, 38)
#define COL_LINE    rgb(40, 50, 66)
#define COL_FG      rgb(232, 238, 248)
#define COL_DIM     rgb(140, 155, 178)
#define COL_TEAL    rgb(0, 229, 192)
#define COL_AMBER   rgb(255, 196, 84)
#define COL_RED     rgb(255, 99, 99)
#define COL_TRACK   rgb(30, 38, 50)

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;
static bool s_on = true;
static uint16_t s_band[LCD_H_RES * BAND_ROWS];
static uint16_t *s_fb;
static int64_t s_last_page_us;

typedef struct {
    bool hall[3];
    bool clutch;
    int state;
    float bias;
    float roll;
    float pitch;
} view_t;

static view_t s_view = { .clutch = true, .state = 0 };

static bool on_color_done(esp_lcd_panel_io_handle_t panel_io,
                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hp);
    return hp == pdTRUE;
}

/* ---------------- framebuffer primitives ---------------- */

static void fb_px(int x, int y, uint16_t c)
{
    if (x < 0 || y < 0 || x >= LCD_H_RES || y >= LCD_V_RES || !s_fb) {
        return;
    }
    s_fb[y * LCD_H_RES + x] = c;
}

static void fb_rect(int x, int y, int w, int h, uint16_t c)
{
    if (!s_fb) {
        return;
    }
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= LCD_V_RES) {
            continue;
        }
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= LCD_H_RES) {
                continue;
            }
            s_fb[yy * LCD_H_RES + xx] = c;
        }
    }
}

static void fb_round(int x, int y, int w, int h, int r, uint16_t fill, uint16_t line)
{
    fb_rect(x + r, y, w - 2 * r, h, fill);
    fb_rect(x, y + r, w, h - 2 * r, fill);
    for (int i = 0; i < r; i++) {
        int dx = r - i;
        int span = (int)(sqrtf((float)(r * r - dx * dx)));
        fb_rect(x + r - span, y + i, span * 2, 1, fill);
        fb_rect(x + r - span, y + h - 1 - i, span * 2, 1, fill);
    }
    if (line) {
        fb_rect(x + r, y, w - 2 * r, 1, line);
        fb_rect(x + r, y + h - 1, w - 2 * r, 1, line);
        fb_rect(x, y + r, 1, h - 2 * r, line);
        fb_rect(x + w - 1, y + r, 1, h - 2 * r, line);
    }
}

static void fb_glyphs(int x, int y, const char *s, const uint8_t *font,
                      int gw, int gh, uint16_t c)
{
    if (!s_fb || !font) {
        return;
    }
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) {
            ch = '?';
        }
        const uint8_t *g = font + (size_t)(ch - 32) * gh;
        for (int row = 0; row < gh; row++) {
            uint8_t bits = g[row];
            int col = 0;
            while (col < gw) {
                if (!(bits & (1 << (gw - 1 - col)))) {
                    col++;
                    continue;
                }
                int start = col;
                while (col < gw && (bits & (1 << (gw - 1 - col)))) {
                    col++;
                }
                fb_rect(x + start, y + row, col - start, 1, c);
            }
        }
        x += gw;
    }
}

static void text12(int x, int y, const char *s, uint16_t c)
{
    fb_glyphs(x, y, s, (const uint8_t *)font12x24, 12, 24, c);
}

static void text10(int x, int y, const char *s, uint16_t c)
{
    fb_glyphs(x, y, s, (const uint8_t *)font10x20, 10, 20, c);
}


/* ---------------- page ---------------- */

static const char *state_name(int s)
{
    switch (s) {
    case 0: return "IDLE";
    case 1: return "PEND";
    case 2: return "SCROLL";
    case 3: return "READY";
    default: return "?";
    }
}

static void draw_page(void)
{
    char buf[16];

    fb_rect(0, 0, LCD_H_RES, LCD_V_RES, COL_BG);

    /* header */
    text12(16, 12, "CLAWBOT", COL_FG);
    text10(196, 16, "POC", COL_DIM);

    /* three hall cards: dot + letter, generous spacing */
    static const char *labs[3] = { "I", "M", "R" };
    for (int i = 0; i < 3; i++) {
        int x = 16 + i * 72;
        fb_round(x, 52, 64, 64, 12, COL_CARD, COL_LINE);
        fb_rect(x + 10, 62, 10, 10, s_view.hall[i] ? COL_TEAL : COL_LINE);
        text12(x + 26, 76, labs[i], s_view.hall[i] ? COL_TEAL : COL_FG);
    }

    /* clutch */
    fb_round(16, 128, 208, 60, 12, COL_CARD, COL_LINE);
    text10(26, 136, "CLUTCH", COL_DIM);
    fb_rect(26, 158, 12, 12, s_view.clutch ? COL_TEAL : COL_RED);
    text12(150, 142, s_view.clutch ? "ON" : "OFF", s_view.clutch ? COL_TEAL : COL_RED);

    /* state */
    fb_round(16, 200, 208, 52, 12, COL_CARD, COL_LINE);
    text10(26, 208, "STATE", COL_DIM);
    uint16_t sc = (s_view.state == 2) ? COL_TEAL : (s_view.state == 3 ? COL_AMBER : COL_FG);
    text12(120, 208, state_name(s_view.state), sc);

    /* footer */
    text10(16, 258, "BOOT LCD", COL_DIM);
    text10(120, 258, "HOLD SLEEP", COL_DIM);
}

static void flush_fb(void)
{
    if (!s_fb || !s_panel) {
        return;
    }
    for (int y = 0; y < LCD_V_RES; y += BAND_ROWS) {
        int n = (LCD_V_RES - y) < BAND_ROWS ? (LCD_V_RES - y) : BAND_ROWS;
        memcpy(s_band, &s_fb[y * LCD_H_RES], (size_t)n * LCD_H_RES * sizeof(uint16_t));
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + n, s_band);
        xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(100));
    }
}

/* ---------------- public API ---------------- */

esp_err_t lcd_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * BAND_ROWS * sizeof(uint16_t),
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
    gpio_hold_dis(PIN_LCD_BL);
    gpio_deep_sleep_hold_dis();

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    s_flush_done = xSemaphoreCreateBinary();
    configASSERT(s_flush_done);

    esp_lcd_panel_io_callbacks_t io_cb = {
        .on_color_trans_done = on_color_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &io_cb, NULL));

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &dev, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 20));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(PIN_LCD_BL, 0);

    s_fb = heap_caps_malloc((size_t)LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (!s_fb) {
        ESP_LOGE(TAG, "framebuffer alloc failed (%d bytes)", LCD_H_RES * LCD_V_RES * 2);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ST7789 240x280 ready, fb=%p free_heap=%u", s_fb,
             (unsigned)esp_get_free_heap_size());

    draw_page();
    flush_fb();
    gpio_set_level(PIN_LCD_BL, 1);
    s_last_page_us = esp_timer_get_time();
    return ESP_OK;
}

void lcd_status_page(const bool hall[3], bool clutch, int state,
                     float bias_dps, float roll_rate, float pitch_rate)
{
    if (!s_fb) {
        return;
    }
    s_view.hall[0] = hall[0];
    s_view.hall[1] = hall[1];
    s_view.hall[2] = hall[2];
    s_view.clutch = clutch;
    s_view.state = state;
    s_view.bias = bias_dps;
    s_view.roll = roll_rate;
    s_view.pitch = pitch_rate;

    int64_t now = esp_timer_get_time();
    if (now - s_last_page_us < PAGE_MS * 1000) {
        return;
    }
    s_last_page_us = now;
    draw_page();
    if (s_on) {
        flush_fb();
    }
}

void lcd_fill(uint16_t color)
{
    fb_rect(0, 0, LCD_H_RES, LCD_V_RES, color);
}

void lcd_rect(int x, int y, int w, int h, uint16_t color)
{
    fb_rect(x, y, w, h, color);
}

void lcd_set_power(bool on)
{
    if (!s_panel || on == s_on) {
        return;
    }
    s_on = on;
    if (on) {
        flush_fb();
        gpio_hold_dis(PIN_LCD_BL);
        esp_lcd_panel_disp_on_off(s_panel, true);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(PIN_LCD_BL, 1);
    } else {
        gpio_set_level(PIN_LCD_BL, 0);
        esp_lcd_panel_disp_on_off(s_panel, false);
    }
    printf("EVT,lcd,%d,0\n", on ? 1 : 0);
}

bool lcd_is_on(void)
{
    return s_on;
}

void lcd_sleep(void)
{
    if (s_panel) {
        esp_lcd_panel_disp_on_off(s_panel, false);
    }
    gpio_set_level(PIN_LCD_BL, 0);
    gpio_hold_en(PIN_LCD_BL);
    gpio_deep_sleep_hold_en();
}
