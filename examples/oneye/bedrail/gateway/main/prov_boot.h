/*
 * prov_boot.h —— 按键触发“下次启动进入 BLE 配网广播”的一次性请求
 *
 * 为什么必须跨重启：配网完成后 NimBLE 已交给 oneye-link-sdk 做中心（连接 T31），
 * 运行期没有 stop/deinit API ⇒ 无法原地退回 peripheral 广播。按键只能置一个
 * RTC 保留标志，重启到“配网启动模式”，由 gw_wifi_prov_boot() 跳过 NVS WiFi、
 * 直接起 wifi_prov_ble_start()。
 */
#ifndef PROV_BOOT_H
#define PROV_BOOT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 置位：请求下一次启动进入 BLE 配网广播（不擦 WiFi 凭据）。 */
void prov_boot_request_set(void);

/* 读取并清除请求：仅本次启动有效。 */
bool prov_boot_request_take(void);

#ifdef __cplusplus
}
#endif

#endif /* PROV_BOOT_H */
