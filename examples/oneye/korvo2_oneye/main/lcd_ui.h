/*
 * lcd_ui.h —— 板载 LCD（ILI9341 320×240）本地验证面
 *
 * ⚠️ 边界（与 panel_api 同口径）：本模块只服务**台面/联调验证**（看得见屏幕、面板可驱动绘图），
 * 不是云端设备面契约的一部分：不新增 topic/影子键/能力位；量产可置 `CONFIG_ONEYE_FW_ENABLE_LCD=n`。
 *
 * 实现参照 ADF 成熟例程：`examples/display/lcd_jpeg`、`examples/display/lcd_camera`
 * （同一 `audio_board_lcd_init()` 句柄 + `esp_lcd_panel_draw_bitmap()` 逐带刷新；本模块用
 * PSRAM 全屏帧缓冲 + 分带 flush，避免逐像素 SPI 写放大）。
 */

#ifndef _LCD_UI_H_
#define _LCD_UI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 面板分辨率（board_def.h：LCD_H_RES/LCD_V_RES = 320×240） */
#define LCD_UI_W 320
#define LCD_UI_H 240

/** 绑定 ADF 板级 LCD 句柄（audio_board_lcd_init 的返回值）并分配帧缓冲；成功即 ready */
esp_err_t lcd_ui_attach(void *panel_handle);

/** 是否已就绪（句柄绑定 + 帧缓冲分配成功） */
bool lcd_ui_ready(void);

/** 当前图案名（bars | grid | checker | status | test），未就绪返回 "" */
const char *lcd_ui_pattern(void);

/** 累计刷新次数 / 最近一次刷新时刻（设备运行毫秒） */
uint32_t lcd_ui_draws(void);
uint64_t lcd_ui_last_ms(void);

/** 帧缓冲大小与所在内存（PSRAM 时为 true）——面板可据此估算显示代价 */
size_t lcd_ui_fb_bytes(void);
bool lcd_ui_fb_in_psram(void);

/** 切换图案并立即刷新；未知图案返回 ESP_ERR_INVALID_ARG（并保留原图案） */
esp_err_t lcd_ui_set_pattern(const char *pattern);

/** 状态屏（图案=status）：三行文本 + 最近按键；面板/固件在按键与链路变化时调用 */
esp_err_t lcd_ui_show_status(const char *line1, const char *line2, const char *line3);

/** 最近一次绘制的参数快照（供 /api/status 序列化） */
typedef struct {
    bool     ready;
    int      w;
    int      h;
    char     pattern[16];
    uint32_t draws;
    uint64_t last_ms;
    size_t   fb_bytes;
    bool     fb_in_psram;
} lcd_ui_state_t;

void lcd_ui_get_state(lcd_ui_state_t *out);

#ifdef __cplusplus
}
#endif

#endif /* _LCD_UI_H_ */
