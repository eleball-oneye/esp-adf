/*
 * board_lcd.h —— 板载 ST7789 1.14" 屏（240x135 横屏）的**简易 GUI**
 *
 * 用户 2026-10-25 逐字要求：
 *   「其上屏幕需要实现简易的GUI，显示连接的温感子设备的温度数据以及当前wifi连接状态
 *     （一个wifi图标展示是否连接以及信号强度），UI背景图在 E:\Oneye\logo\oneye1.png」
 *
 * 口径（刻意最简，⛔ 不引 LVGL）：
 *   · 背景图**不入固件**：由 tools/mk-lcd-bg.py 转成 RGB565 + 16 B 头，烧到 `model` 分区
 *     （partitions.csv 里那块 1 MB spiffs 本来就不用）⇒ 固件按 15 行一条带从 flash 流式读，
 *     常驻 RAM 只有一个 240×15 的带缓冲（7.2 KB）。
 *   · 前景只画两类东西：WiFi 扇形图标（是否连接 + 信号强度分级）+ 子设备温度大数字。
 *   · 面板方向/镜像按 Kconfig `ONEYE_T31_LCD_ROT` 选（0..3），换了要重烧才生效。
 *
 * ⚠️ 本模块**只做渲染**：不读 WiFi 状态、不碰 link-sdk。调用方（main.c 的 UI 任务）
 *    把状态填进 lcd_status_t 再调 board_lcd_render()。
 */
#ifndef BOARD_LCD_H
#define BOARD_LCD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一帧要显示的全部状态（结构体传值，调用方随便填，内部不持引用） */
typedef struct {
    bool wifi_connected;   /* STA 是否已连上 AP */
    int  wifi_rssi;        /* dBm；wifi_connected=false 时忽略 */
    bool cloud_up;         /* 云链路是否已建立（MQTT CLIENT 连上） */
    bool sub_online;       /* 温感子设备是否在线（link 已连上） */
    bool has_temp;         /* 是否已收到过温度帧 */
    int  temp_centi;       /* 温度，单位 0.01 ℃（3652 = 36.52℃） */
    int  battery;          /* 子设备电量 %；-1 = 未知 */
    const char *sub_model; /* 子设备档案型号（可 NULL，默认 "T31"） */
} lcd_status_t;

/*
 * 初始化 SPI + ST7789 面板 + 背光 + 背景分区探测。幂等。
 * 失败**不致命**（返回 ESP_FAIL，调用方记一行 WARN 即可；主功能不受影响）。
 */
esp_err_t board_lcd_init(void);

/* 是否初始化成功（调用方可用它决定要不要起 UI 任务） */
bool board_lcd_ready(void);

/*
 * 全屏重绘一帧（内部按 15 行条带流式推送，阻塞式；约 20–40 ms @40 MHz）。
 * ⚠️ 只允许**一个**任务调用（内部有重入保护，重入直接返回）。
 */
void board_lcd_render(const lcd_status_t *st);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_LCD_H */
