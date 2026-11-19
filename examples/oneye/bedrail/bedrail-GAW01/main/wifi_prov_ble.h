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
 * 🔴 **2026-10-21 订正（⛔ 旧字逐字留痕，⛔ 不抹）**：上面第 ② 条**已作废** —— 旧行为是"已入网 ⇒ 拒绝"，
 *    现为"已入网 ⇒ 先忘记 Wi-Fi 凭据（含 NVS 持久化配置）再进入配网"（长按 BOOT ≥3 s 的语义）。
 *    对"已入网"**不再返回** `ESP_ERR_INVALID_STATE`；需要"只忘记不进入"的调用者请直接调
 *    `wifi_prov_forget_wifi()`。旧实现逐字见 `wifi_prov_ble.c` 的 `pairing_enter()` 内留痕块。
 */
esp_err_t wifi_prov_ble_pairing_enter(void);

/**
 * 🔴 **2026-10-21〔BOOT 长按忘记 WiFi + 重进 BLE 配网批〕长按重配的唯一入口**。
 *
 * 语义 = **忘记 Wi-Fi 凭据 + 重新进入 BLE 配网**（两例程共用这一份）：
 *   ① `wifi_prov_forget_wifi()`：断开并停 STA、清 RAM 采信槽/STA 配置、`esp_wifi_restore()`
 *      擦掉 NVS 持久化 Wi-Fi 配置（⛔ 不动 creds 分区、⛔ 不动 nvs 其它命名空间）；
 *   ② `wifi_prov_ble_pairing_enter()`：起栈（入网后 deinit 过 ⇒ 完整 re-init）/重开广播 + 重置窗口；
 *   ③ 成功时打印用户逐字要求的结论行 `BOOT 长按：已清除 WiFi 凭据，进入 BLE 配网`。
 *
 * 三种起点都收敛到同一条路径（幂等）：
 *   · **已入网**（旧行为是**拒绝**，见实现里的旧字留痕）⇒ 现在受理为重配；
 *   · **在线且 BLE 已 deinit**（乙型省内存路径）⇒ `start()` 走完整 init；
 *   · **离线/待机** ⇒ 忘记是空操作，进入配网模式。
 *
 * ⚠️ 必须从**任务上下文**调用（内部 `esp_wifi_stop()` 阻塞），⛔ 不在 sys_evt / esp_timer 回调里调用。
 * ⛔ 不打任何凭据（SSID/密码/激活码/chal）。
 */
esp_err_t wifi_prov_ble_pairing_reprovision(void);

/** 退出配网模式：停广播 + 停计时（幂等；不在配网模式时只打印一行，不报错）。 */
esp_err_t wifi_prov_ble_pairing_exit(void);

/** 配网模式开着**且**在广播？（进入/退出的唯一判据，供串口/面板自证） */
bool wifi_prov_ble_pairing_is_active(void);

/** 广播中？（供面板/串口自证；`false` = 未启用或已停止） */
bool wifi_prov_ble_active(void);

/**
 * ★ 2026-10-23（宽限窗**提前结束**）：配网 BLE 会话当前是否有对端连接。
 *
 * 用途：`main.c` 的宽限窗循环每秒轮询一次 —— App 完成绑定后会**断开 BLE**，
 *   一旦检测到断开就立即结束宽限窗（⛔ 不再死等到 N 秒），把"添加设备用时很久"压下去。
 *
 * 为什么由本模块转一手（⛔ 不让 main.c 直接 include `oneye_dev_ble.h`）：
 *   `main.c` 必须 include `oneye_dev_sdk.h`（→ `oneye_dev_base.h`），而本仓 SDK 的
 *   `oneye_dev_link.h` / `oneye_dev_ble.h` 存在与 base 不兼容的类型面（详见
 *   `wifi_prov_ble_report_prov_connected` 的注释：main.c 结构上不能引 link 头）
 *   ⇒ 统一由本 TU 暴露一个 bool 查询，main.c 只依赖本头。
 *
 * 判据来源：SDK 既有只读快照 `oneye_dev_ble_get_status()->connected`（⛔ 不改 SDK、⛔ 不发帧）。
 * 返回 `false` 的情形：通道未起 / 未连接 / 查询失败（⛔ 不把失败说成"已连接"）。
 */
bool wifi_prov_ble_session_connected(void);

/**
 * ⛔ 旧声明逐字留痕（⛔ 不抹）—— `const char *wifi_prov_ble_pop(void);`
 *   （「当前配对 POP（🔻 兼容/产测档；未启用/未生成时返回 ""）。**只在串口打印**，⛔ 不进任何上行帧」）：
 *   按用户 2026-10-09 裁定「彻底删除实现及契约」整条删除；配对凭据现值 = 激活码
 *   （`prov.pair.code`，用户从机身贴码输入）＋ 一次性 `chal`（设备在 `prov.hello` 明文回包下发）。
 */

/** 入网成功后停广播（由 `wifi_prov.c` 的收敛点下游调用；unpair 后可用 `wifi_prov_ble_start` 重开） */
void wifi_prov_ble_stop_advertising(void);

/**
 * 🔴 **2026-10-20〔`mqtt client alloc failed` 批〕新增：入网成功后把 BLE 整栈 deinit，回收 ~100 KB**。
 *
 * 为什么需要它（真机读数，⛔ 非推断）：乙型 devkitc 入网后 free heap ≈ 7 KB（NimBLE 主机栈 ＋
 * Wi-Fi 动态缓冲占着），而 `oneye_mqtt_new()` 要一笔 65568 B ＋ 16384 B ⇒ `mqtt client alloc failed`。
 * 用户 2026-10-20 裁定：「RX/TXQ 4096｜**允许 deinit BLE**」。
 *
 * 语义（⛔ 不新增帧 / ⛔ 不改判据 / ⛔ 不动凭据）：
 *   · 先停广播（幂等），再 `oneye_dev_ble_deinit()`（SDK 侧 = `nimble_port_stop()` +
 *     `nimble_port_deinit()` + 释放锁，见 `src/oneye_dev_ble.c:1455-1470`）；
 *   · 本模块的 `s_started` / `s_advertising` / `s_pairing` 一并清零 ⇒ **下一次
 *     `wifi_prov_ble_start()` 会走完整 init 路径**（NimBLE 重新起栈、GATT 重新注册、
 *     广播重新开）—— 这就是"换网 / 重配网"所需的 re-init 路径，⛔ 不需要另外补一套；
 *   · **幂等**：已 deinit / 通道未启用 ⇒ 直接返回，⛔ 不报错、⛔ 不重复释放。
 *
 * ⚠️ 调用时机（唯一合规点）：`devkitc_oneye_main.c` 的 `wifi_ready_work()` —— 即
 *   **Wi-Fi 已 GOT_IP、`prov.status{connected}` 已发出、停广播之后、建 MQTT（`oneye_start()`）之前**。
 *   ⛔ 不要在 `on_wifi_ready()`（sys_evt）里调：`nimble_port_stop()` 要等主机任务退出，属阻塞动作。
 * ⚠️ 副作用（如实登记）：deinit 之后本进程内 `prov.status` 再也发不出去（BLE 不在）——
 *   已入网设备不需要它；Wi-Fi 掉线后再 GOT_IP 的那一次上报会失败并**如实打印"未发出"**，
 *   ⛔ 不假装已上报。用户此时长按 BOOT 仍可重配网（`pairing_enter` 会重新 init BLE）。
 *
 * @return `ESP_OK` = 已 deinit（或本来就不在）；`ESP_FAIL` = SDK 明确拒绝；`ESP_ERR_NOT_SUPPORTED` = 本构建没开 BLE 通道。
 */
esp_err_t wifi_prov_ble_deinit_after_provisioned(void);

/**
 * ★ 2026-10-10：上报 `prov.status{state=connected, ip=…}`（契约 §4.1 的**既有**状态，本批落实）。
 *
 * 为什么这一帧由**本模块**发出（而不是收敛点 `main.c` 直接调 SDK）：
 *   ① link 的初始化与 prov provider 都归本模块（`wifi_prov_ble_start()` 里的 `oneye_dev_link_init`
 *      / `oneye_dev_link_prov_set_provider`）—— 发帧用的 `oneye_dev_link.h` 与它同属这一层；
 *   ② **类型冲突（SDK 既有缺陷，⛔ 本批不修）**：`oneye_dev_link_status_t` 同时定义在
 *      `oneye_dev_base.h:164` 与 `oneye_dev_link.h:163` 且**不兼容**，而 `main.c` 必须 include
 *      `oneye_dev_sdk.h`（→ `oneye_dev_base.h`）与 `oneye_dev_creds.h` ⇒ **main.c 结构上不能**引
 *      `oneye_dev_link.h`（这与 `adv_identity.c`「另立独立 TU」是同一个成因）。
 *   ⇒ 收敛点只表达"该不该报"（判据在 `wifi_prov_status.h`），"怎么发"收在本模块。
 *
 * 返回：
 *   - `ESP_OK` —— 帧已交给 SDK **既有**发送路径（`oneye_dev_link_prov_report_status()` →
 *     `oneye_dev_link_broadcast()`），因此**天然经过**它自己的发送锁（TX 串行化）；
 *   - `ESP_ERR_INVALID_STATE` —— link 未 init / 未 start（本机型没起这条通道）⇒ 没有发出；
 *   - `ESP_FAIL` —— SDK 返回了非 OK。
 * ⚠️ 调用方（`korvo2_oneye_main.c` 的 `prov_status_report_connected_once()`）**只在 `ESP_OK` 时**
 *    置"已上报"幂等位；其余情况**保持未报**，下一次 GOT_IP 再试（⛔ 不把失败写成已上报）。
 * ⛔ 本函数不阻塞、不等回报、不重试、⛔ 不自建第二条发送路径；`ssid` / `source` 可为 NULL/空
 *    （契约里它们是 `optional`，但 `ip` **必须非空** —— 那一条由调用方的判据保证）。
 */
esp_err_t wifi_prov_ble_report_prov_connected(const char *ssid, const char *ip, const char *source);

/**
 * ★ 2026-10-10（诊断版）：把「链路 / 配网」状态打成一行心跳（诊断构建每 5 s 一次）。
 *
 * 实现只在 `CONFIG_ONEYE_FW_DIAG_PROV_TRACE=y` 时**编进来**（见 `wifi_prov_ble.c`）：
 * 诊断构建下声明与定义都在；量产构建下两者都不在（调用点也用同一个宏包起来 ⇒ 不会有未定义符号）。
 * ⛔ 只读状态、⛔ 不发帧、⛔ 不改任何判据。
 */
void wifi_prov_ble_diag_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* _WIFI_PROV_BLE_H_ */
