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
 *
 * ★ 2026-10-09 起：本函数的行为**逐字未变**（起栈 + 立刻广播）——它仍是 `wifi_prov_boot()` 里那一处
 *   调用，也是 `check-prov-convergence` A8a/A13b 依赖的既有路径。要"开机只待机、由按键进入配网模式"，
 *   由调用方**先**调 `wifi_prov_ble_set_autostart(false)`；⛔ 不要复制/分叉本函数。
 */
esp_err_t wifi_prov_ble_start(void);

/**
 * 开机是否自动广播（缺省 `true` = 旧行为）。**必须在 `wifi_prov_ble_start()` 之前**设置。
 * 置 `false` ⇒ `start()` 起栈后停在**待机**（NimBLE 在、GATT 在、不广播），等按键进入配网模式。
 * ⛔ 这与 `CONFIG_ONEYE_FW_ENABLE_BLE_PROV` 是两件事：后者决定"本机型有没有这条通道"。
 */
void wifi_prov_ble_set_autostart(bool autostart);

/**
 * 进入配网模式（=「接受 App 的蓝牙配网」）：起通道（若未起）+ 开始广播 + 起超时计时
 * （`CONFIG_ONEYE_FW_BLE_PROV_PAIRING_WINDOW_S`）。幂等：已在广播中只重置计时。
 * 幂等之外的两个**显式拒绝**（都不静默）：① 通道未启用 ⇒ `ESP_ERR_NOT_SUPPORTED`；
 * ② 设备**已入网** ⇒ `ESP_ERR_INVALID_STATE`（入网成功后不再保持可配网态）。
 */
esp_err_t wifi_prov_ble_pairing_enter(void);

/** 退出配网模式：停广播 + 停计时（幂等；不在配网模式时只打印一行，不报错）。 */
esp_err_t wifi_prov_ble_pairing_exit(void);

/** 配网模式开着**且**在广播？（进入/退出的唯一判据，供串口/面板自证） */
bool wifi_prov_ble_pairing_is_active(void);

/** 广播中？（供面板/串口自证；`false` = 未启用或已停止） */
bool wifi_prov_ble_active(void);

/**
 * 当前配对 POP（🔻 **兼容/产测档**；未启用/未生成时返回 ""）。
 * **只在串口打印**，⛔ 不进任何上行帧。
 * ⚠️ 2026-10-09 起**主档配对凭据** = 激活码（`prov.pair.code`，用户从机身贴码输入）＋ 一次性
 *    `chal`（设备在 `prov.hello` 明文回包下发）⇒ 本函数只是兼容/产测档的读数（台账 #5：⛔ 未删）。
 */
const char *wifi_prov_ble_pop(void);

/** 入网成功后停广播（由 `wifi_prov.c` 的收敛点下游调用；unpair 后可用 `wifi_prov_ble_start` 重开） */
void wifi_prov_ble_stop_advertising(void);

#ifdef __cplusplus
}
#endif

#endif /* _WIFI_PROV_BLE_H_ */
