/*
 * wifi_prov_ble.h —— 配网通道 C1/C2：BLE（移动端 App 或另一台嵌入式设备）
 *
 * 口径见 ADR-0017 与本工程 `wifi_prov.h` 顶部注释。
 *
 * ★ 本模块是**通道**，不是「入网后逻辑」★
 *   它只做三件事：① 起 BLE 配网服务（`oneye_dev_ble` 自研 GATT）；② 把 App 下发的 SSID/密码
 *   交给**唯一入网入口** `wifi_prov_submit_credentials(..., WIFI_PROV_CHAN_BLE)`；
 *   ③ 把 AP 扫描结果回给 App。
 *   「入网成功之后做什么」不在这里 —— 那是 `wifi_prov.c` 的 `IP_EVENT_STA_GOT_IP` 处理器
 *   → `s_ready_cb` → `main.c:on_wifi_ready()` → `oneye_start()` 这一条**唯一**路径
 *   （ADR-0017 D2）。⛔ 任何人不得在本文件里调 `oneye_start()`。
 *
 * 协议事实源（不在本仓新增协议）：`contracts/local/ble-gatt.md`、`contracts/local/link-frames.json`
 *   —— `prov.scan.req` / `prov.connect.req` / `prov.reset` 由 `oneye_dev_link` 内置处理并转发到
 *   本模块注册的 provider（`oneye_dev_link.c:267-331`）。
 */

#ifndef _WIFI_PROV_BLE_H_
#define _WIFI_PROV_BLE_H_

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 起 BLE 配网通道（幂等）：link init → ble init → 注册 provider → 绑定为 link 的 ble 信道 → 开始广播。
 * 需要 `CONFIG_ONEYE_FW_ENABLE_BLE_PROV=y`；关闭时本函数直接返回 `ESP_ERR_NOT_SUPPORTED`
 * 并打印一行说明（**不静默**）。
 */
esp_err_t wifi_prov_ble_start(void);

/** 广播中？（供面板/串口自证；`false` = 未启用或已停止） */
bool wifi_prov_ble_active(void);

/** 当前配对 POP（未启用/未生成时返回 ""）。**只在串口打印**，⛔ 不进任何上行帧。 */
const char *wifi_prov_ble_pop(void);

/** 入网成功后停广播（由 `wifi_prov.c` 的收敛点下游调用；unpair 后可用 `wifi_prov_ble_start` 重开） */
void wifi_prov_ble_stop_advertising(void);

#ifdef __cplusplus
}
#endif

#endif /* _WIFI_PROV_BLE_H_ */
