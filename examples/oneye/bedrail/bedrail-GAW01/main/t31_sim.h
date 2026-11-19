/*
 * t31_sim.h —— 温感子设备模拟器（SUBDEV 角色）：模拟健奥 T31 体温贴的 BLE 外设
 *
 * 对外只有一个入口：起 NimBLE 外设 + 广播 + GATT + 周期推温度帧。
 * ⛔ 本模块**不连云**、**不读 creds 分区**、**不做 BLE 配网**（那是网关角色的事）。
 *
 * 广播/GATT/帧格式见 t31_sim.c 文件头（含"真 T31 编码未解出、本帧为模拟器自洽口径"的说明）。
 */
#ifndef T31_SIM_H
#define T31_SIM_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 起子设备模拟器（幂等性：只应调用一次）。
 * 返回 ESP_OK = NimBLE 已起栈（广播在 host sync 之后自动开始）；其余 = 初始化失败（已打印原因）。 */
esp_err_t t31_sim_start(void);

#ifdef __cplusplus
}
#endif

#endif /* T31_SIM_H */
