#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP32-S3-Touch-LCD-1.69: ST7789 240x280, SPI */
#define LCD_H_RES   240
#define LCD_V_RES   280

esp_err_t lcd_init(void);
void lcd_sleep(void);   /* panel off + backlight low + output hold */
void lcd_set_power(bool on);
bool lcd_is_on(void);
void lcd_fill(uint16_t color);
void lcd_rect(int x, int y, int w, int h, uint16_t color);

/* Debug page: hall[3], clutch, gesture state (0..3), bias magnitude, plane rates. */
void lcd_status_page(const bool hall[3], bool clutch, int state,
                     float bias_dps, float roll_rate, float pitch_rate);

#ifdef __cplusplus
}
#endif
