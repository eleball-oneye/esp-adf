/*
 * adv_identity.h —— BLE 配网路径的**设备身份单点读取**（node_id = `creds` 分区）
 *
 * ★ 2026-10-07（本批）：返回值**同时**喂三个消费点（用户逐字裁定「广播名来源**改为由 node_id 派生**」）：
 *   ① 广播名 `ONEYE-<node_id 后 4 位>`（派生在 SDK，见 `wifi_prov_ble.c` 文件头的 ①）；
 *   ② 配对 HKDF salt = 同一个 node_id（同上 ②）；
 *   ③ 广播厂商字段（AD 0xFF）的 `sn`（同一个串，尾部 ≤11 B 由 SDK 截断）。
 *   ⇒ ⛔ 调用方**不得**再引入第二个身份来源（如 `CONFIG_ONEYE_FW_DEVICE_ID`）。
 *
 * ⛔ 本头**不包含**任何 SDK 头：`wifi_prov_ble.c` 需要 link 面，而 `oneye_dev_creds.h` 会带进
 *    `oneye_dev_base.h` 的 `oneye_dev_link_status_t`（与 link 面同名不同构）⇒ 必须由独立 TU 提供。
 *    详细理由与"这是 SDK 既有缺陷、本批只绕开不修"的口径见 adv_identity.c 文件头。
 */

#ifndef ADV_IDENTITY_H
#define ADV_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 取设备身份（= `creds` 分区 `node_id`）写入 `out`（保证 NUL 结尾）。
 *
 * | 返回 | 条件 |
 * | --- | --- |
 * | `true` | `out` 内为非空 node_id（可交给 `oneye_dev_ble_config_t.device_id` 与 `.adv_sn`） |
 * | `false` | 参数非法 / `creds` 读失败 / `node_id` 为空 ⇒ 调用方**不得**广播、不得算 salt |
 *
 * ⛔ 失败时**不留任何半截值**（`out[0] = '\0'`）：广播里不得出现"看起来像 SN 的东西"，
 *    也不得回落编译期常量（那会让广播名与 salt 出现第二个来源）。
 */
bool adv_identity_sn(char *out, size_t cap, size_t *len_out);

#ifdef __cplusplus
}
#endif

#endif /* ADV_IDENTITY_H */
