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
#include "font20x26.h"
#include "font22x34.h"

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

static void fb_glyphs(int x, int y, const char *s, const uint16_t *font,
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
        const uint16_t *g = font + (size_t)(ch - 32) * gh;
        for (int row = 0; row < gh; row++) {
            uint16_t bits = g[row];
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

static void text20(int x, int y, const char *s, uint16_t c)
{
    fb_glyphs(x, y, s, (const uint16_t *)font20x26, 20, 26, c);
}

static void text22(int x, int y, const char *s, uint16_t c)
{
    fb_glyphs(x, y, s, (const uint16_t *)font22x34, 22, 34, c);
}

static uint16_t blend(uint16_t a, uint16_t b, int pct)
{
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = (ar * (100 - pct) + br * pct) / 100;
    int g = (ag * (100 - pct) + bg * pct) / 100;
    int bl = (ab * (100 - pct) + bb * pct) / 100;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static void fb_outline_round(int x, int y, int w, int h, int r, uint16_t c)
{
    fb_rect(x + r, y, w - 2 * r, 1, c);
    fb_rect(x + r, y + h - 1, w - 2 * r, 1, c);
    fb_rect(x, y + r, 1, h - 2 * r, c);
    fb_rect(x + w - 1, y + r, 1, h - 2 * r, c);
    for (int i = 0; i < r; i++) {
        int dy = r - i;
        int span = (int)(sqrtf((float)(r * r - dy * dy)));
        fb_px(x + r - span, y + i, c);
        fb_px(x + w - 1 - r + span, y + i, c);
        fb_px(x + r - span, y + h - 1 - i, c);
        fb_px(x + w - 1 - r + span, y + h - 1 - i, c);
    }
}

static void glow_rect(int x, int y, int w, int h, int r, uint16_t c, int strength)
{
    for (int i = 3; i >= 1; i--) {
        uint16_t col = blend(c, COL_BG, strength / (i + 1));
        fb_outline_round(x - i, y - i, w + 2 * i, h + 2 * i, r + i, col);
    }
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
    int pulse = (int)((esp_timer_get_time() / 200000) % 5) * 8 + 20;

    /* background: vertical gradient + faint corner washes */
    for (int y = 0; y < LCD_V_RES; y++) {
        int t2 = (y * 100) / LCD_V_RES;
        uint16_t row = blend(rgb(18, 26, 48), rgb(4, 6, 10), t2);
        for (int x = 0; x < LCD_H_RES; x++) {
            uint16_t c = row;
            int dx = x - 20, dy = y - 20;
            int d = (int)sqrtf((float)(dx * dx + dy * dy));
            if (d < 180) {
                c = blend(c, rgb(0, 170, 150), (180 - d) / 9);
            }
            dx = x - 240; dy = y - 300;
            d = (int)sqrtf((float)(dx * dx + dy * dy));
            if (d < 160) {
                c = blend(c, rgb(110, 90, 220), (160 - d) / 11);
            }
            fb_px(x, y, c);
        }
    }

    /* header */
    text20(14, 8, "CLAWBOT", COL_FG);
    snprintf(buf, sizeof(buf), "%.1f", (double)s_view.bias);
    text20(172, 8, buf, COL_DIM);

    /* gradient accent line */
    for (int i = 0; i < 212; i++) {
        fb_rect(14 + i, 38, 1, 2, blend(COL_TEAL, rgb(140, 120, 255), (i * 100) / 212));
    }

    /* three hall cards */
    static const char *labs[3] = { "I", "M", "R" };
    for (int i = 0; i < 3; i++) {
        int x = 16 + i * 72;
        fb_round(x, 48, 64, 72, 14, s_view.hall[i] ? blend(COL_CARD, COL_TEAL, 14) : COL_CARD, COL_LINE);
        if (s_view.hall[i]) {
            glow_rect(x, 48, 64, 72, 14, COL_TEAL, pulse);
        }
        fb_rect(x + 12, 60, 10, 10, s_view.hall[i] ? COL_TEAL : COL_LINE);
        text22(x + 21, 76, labs[i], s_view.hall[i] ? COL_TEAL : COL_FG);
    }

    /* clutch: label above, value below */
    uint16_t acc = s_view.clutch ? COL_TEAL : COL_RED;
    fb_round(16, 128, 208, 78, 14, blend(COL_CARD, acc, 12), COL_LINE);
    fb_rect(16, 146, 3, 46, acc);
    if (s_view.clutch) {
        glow_rect(16, 128, 208, 78, 14, COL_TEAL, pulse);
    }
    text20(28, 136, "CLUTCH", COL_DIM);
    text22(28, 166, s_view.clutch ? "ON" : "OFF", acc);

    /* state: value only, centred */
    uint16_t sc = (s_view.state == 2) ? COL_TEAL : (s_view.state == 3 ? COL_AMBER : COL_FG);
    fb_round(16, 212, 208, 58, 14, blend(COL_CARD, sc, 10), COL_LINE);
    fb_rect(16, 224, 3, 36, sc);
    int sw = (int)strlen(state_name(s_view.state)) * 22;
    text22((240 - sw) / 2, 224, state_name(s_view.state), sc);
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
