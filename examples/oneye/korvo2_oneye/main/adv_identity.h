/*
 * adv_identity.h —— BLE 配网路径的**身份单点读取**（`creds` 分区的 **两串**）
 *
 * 🔴 2026-10-09（台账 #1 / #3 / #4）：身份**拆成两串**，⛔ 不再"一个串喂三处"：
 *   · `node_id` = **激活码**（**私密**：= 设备证书 CN；⛔ 不广播、⛔ 不进明文帧；✅ 贴机身）
 *     ⇒ 用途只剩两处：① `lcfg.node_id`（link 帧 `from`）② 配对 HKDF **salt** 与
 *        `prov.pair.code` 的**逐字比对基准**（`bcfg.device_id`）。
 *   · `sn` = **SN**（**公开**：贴机身、**可广播**）
 *     ⇒ 用途三处：① 广播名 `ONEYE-<SN 后 4 位>`（SDK 派生）② 厂商字段 `sn_field`
 *        ③ `prov.pair.ok.sn`（⛔ 该帧**不再回 `node_id`**）。
 *   ⛔ **两串⛔ 不得互推**；本文件是**唯一**读取点（⛔ 调用方不得再引入第二个来源，
 *      尤其 ⛔ 不得回落 `CONFIG_ONEYE_FW_DEVICE_ID`）。
 *
 * ★ `creds.sn` 为空 ⇒ **回落**（存量机行为不变）：`sn` 字段填 `node_id`、`sn_fell_back = true`；
 *   厂商字段会因此被如实标注 `0x04`（老世代），App 据此知道"这一台没有独立的 SN"。
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

/* 会话/身份缓冲上界：激活码与 SN 的契约字符集上界均为 64（`^[A-Za-z0-9._-]{1,64}$`）+ 余量 */
#define ADV_IDENTITY_BUF 72u

typedef struct {
    /* 激活码（= `creds.node_id`）：link 节点 id + HKDF salt + `prov.pair.code` 比对基准 */
    char node_id[ADV_IDENTITY_BUF];
    /* SN（= `creds.sn`；为空时**回落** `node_id`，见 `sn_fell_back`）：公开身份 */
    char sn[ADV_IDENTITY_BUF];
    /* `true` = `creds.sn` 为空 ⇒ `sn` 是 `node_id` 的回落值（**存量机**；厂商字段会标 0x04） */
    bool sn_fell_back;
} adv_identity_t;

/*
 * 读取本机身份（一次性 `creds` 读取，两个串一起取出）。
 *
 * | 返回 | 条件 |
 * | --- | --- |
 * | `true` | `out->node_id` 非空（`out->sn` 保证非空：读不到 `creds.sn` 时回落 `node_id`） |
 * | `false` | 参数非法 / `creds` 读失败 / `node_id` 为空 ⇒ 调用方**不得**启动 BLE 配网通道 |
 *
 * ⛔ 失败时 `out` 全清零（不留半截值）：广播里不得出现"看起来像 SN 的东西"，
 *    也不得回落编译期常量（那会让广播名与 salt 出现第二个来源）。
 */
bool adv_identity_load(adv_identity_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ADV_IDENTITY_H */
