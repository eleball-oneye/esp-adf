#include "lcd.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "lcd";

#define PIN_LCD_MOSI 7
#define PIN_LCD_SCLK 6
#define PIN_LCD_CS   5
#define PIN_LCD_DC   4
#define PIN_LCD_RST  8
#define PIN_LCD_BL   15

#define COL_BG      0x0B0F14
#define COL_CARD    0x1C2333
#define COL_LINE    0x3E4E66
#define COL_FG      0xE8EEF8
#define COL_DIM     0x8C9BB2
#define COL_TEAL    0x00E5C0
#define COL_AMBER   0xFFC454
#define COL_RED     0xFF6363

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static lv_display_t *s_disp;
static bool s_on = true;

static lv_obj_t *s_hall_card[3];
static lv_obj_t *s_hall_dot[3];
static lv_obj_t *s_clutch_card;
static lv_obj_t *s_clutch_lbl;
static lv_obj_t *s_state_card;
static lv_obj_t *s_state_lbl;
static lv_obj_t *s_bias_lbl;

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

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, const char *text,
                       const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    label(scr, 14, 10, "CLAWBOT", &lv_font_montserrat_20, COL_FG);
    s_bias_lbl = label(scr, 176, 14, "4.0", &lv_font_montserrat_16, COL_DIM);

    lv_obj_t *rule = lv_obj_create(scr);
    lv_obj_remove_style_all(rule);
    lv_obj_set_pos(rule, 14, 42);
    lv_obj_set_size(rule, 212, 2);
    lv_obj_set_style_bg_color(rule, lv_color_hex(COL_TEAL), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    static const char *names[3] = { "I", "M", "R" };
    for (int i = 0; i < 3; i++) {
        int x = 16 + i * 72;
        s_hall_card[i] = card(scr, x, 52, 64, 72, 12);

        s_hall_dot[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(s_hall_dot[i]);
        lv_obj_set_pos(s_hall_dot[i], x + 12, 64);
        lv_obj_set_size(s_hall_dot[i], 10, 10);
        lv_obj_set_style_radius(s_hall_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_hall_dot[i], lv_color_hex(COL_LINE), 0);
        lv_obj_set_style_bg_opa(s_hall_dot[i], LV_OPA_COVER, 0);

        lv_obj_t *t = label(scr, 0, 0, names[i], &lv_font_montserrat_28, COL_FG);
        lv_obj_align_to(t, s_hall_card[i], LV_ALIGN_CENTER, 0, 8);
    }

    s_clutch_card = card(scr, 16, 132, 208, 78, 14);
    label(scr, 28, 142, "CLUTCH", &lv_font_montserrat_16, COL_DIM);
    s_clutch_lbl = label(scr, 28, 164, "ON", &lv_font_montserrat_28, COL_TEAL);

    s_state_card = card(scr, 16, 216, 208, 56, 14);
    label(scr, 28, 224, "STATE", &lv_font_montserrat_16, COL_DIM);
    s_state_lbl = label(scr, 110, 226, "IDLE", &lv_font_montserrat_24, COL_FG);
}

void lcd_status_page(const bool hall[3], bool clutch, int state,
                     float bias_dps, float roll_rate, float pitch_rate)
{
    (void)roll_rate;
    (void)pitch_rate;
    if (!s_disp) {
        return;
    }
    if (!lvgl_port_lock(20)) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(s_hall_dot[i],
                                  lv_color_hex(hall[i] ? COL_TEAL : COL_LINE), 0);
        lv_obj_set_style_border_color(s_hall_card[i],
                                      lv_color_hex(hall[i] ? COL_TEAL : COL_LINE), 0);
    }

    lv_label_set_text(s_clutch_lbl, clutch ? "ON" : "OFF");
    lv_obj_set_style_text_color(s_clutch_lbl, lv_color_hex(clutch ? COL_TEAL : COL_RED), 0);
    lv_obj_set_style_border_color(s_clutch_card,
                                  lv_color_hex(clutch ? COL_TEAL : COL_LINE), 0);

    lv_label_set_text(s_state_lbl, state_name(state));
    uint32_t sc = (state == 2) ? COL_TEAL : (state == 3 ? COL_AMBER : COL_FG);
    lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(sc), 0);
    lv_obj_set_style_border_color(s_state_card,
                                  lv_color_hex((state == 2 || state == 3) ? sc : COL_LINE), 0);

    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", (double)bias_dps);
    lv_label_set_text(s_bias_lbl, buf);

    lvgl_port_unlock();
}

esp_err_t lcd_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

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

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io));

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &dev, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 20));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));

    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(PIN_LCD_BL, 0);

    const lvgl_port_cfg_t port_cfg = {
        .task_priority = 4,
        .task_stack = 4096,
        .task_affinity = 1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = LCD_H_RES * 40,
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    if (lvgl_port_lock(1000)) {
        build_ui();
        lvgl_port_unlock();
    }

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    gpio_set_level(PIN_LCD_BL, 1);
    ESP_LOGI(TAG, "LVGL UI ready, free_heap=%u", (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

void lcd_fill(uint16_t color)
{
    (void)color;
}

void lcd_rect(int x, int y, int w, int h, uint16_t color)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
}

void lcd_set_power(bool on)
{
    if (!s_panel || on == s_on) {
        return;
    }
    s_on = on;
    if (on) {
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
