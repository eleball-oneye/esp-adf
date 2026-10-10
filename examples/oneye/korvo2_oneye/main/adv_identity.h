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

/*
 * 🔴 **2026-10-10 新增**：身份装载的**结果**（供调用方按**档位**决定能否继续）。
 *
 * 为什么不能只返回 `bool`（契约 §4.4.10（七·一）1. 的实现依据）：
 *   「读不到身份」对**甲**与对**乙 / 丙**的处置**不同** —— 甲必须 fail-closed（⛔ 不冒充身份），
 *   而乙 / 丙**在上平台前本来就没有实例身份**（契约 §4.4.8（一）身份状态矩阵：T1 / T2 无身份）
 *   ⇒ 它们**必须**能在无身份下起 BLE 配网栈（否则"乙 / 丙 永远配不上网"）。
 *   ⛔ 但**无论哪一档**都⛔ **不得**回落 `CONFIG_ONEYE_FW_DEVICE_ID`（第二来源 = 冒充）。
 */
typedef enum {
    ADV_IDENTITY_OK = 0,      /* 身份已装载：`node_id` 非空（`sn` 非空：读不到时回落 `node_id`） */
    ADV_IDENTITY_ERR_CREDS,   /* `creds` 读失败（没写过 / 坏了）—— `out` 已全清零 */
    ADV_IDENTITY_ERR_EMPTY,   /* `creds` 读到了，但 `node_id` 为空 —— `out` 已全清零 */
} adv_identity_status_t;

typedef struct {
    /* 激活码（= `creds.node_id`）：link 节点 id + `prov.pair.code` 的**核验基准**（甲，见 `prov.verify.req`） */
    char node_id[ADV_IDENTITY_BUF];
    /* SN（= `creds.sn`；为空时**回落** `node_id`，见 `sn_fell_back`）：公开身份 */
    char sn[ADV_IDENTITY_BUF];
    /* `true` = `creds.sn` 为空 ⇒ `sn` 是 `node_id` 的回落值（**存量机**；厂商字段会标 0x04） */
    bool sn_fell_back;
} adv_identity_t;

/*
 * 读取本机身份（一次性 `creds` 读取，两个串一起取出）。
 *
 * | 返回 | 条件 | 调用方处置 |
 * | --- | --- | --- |
 * | `ADV_IDENTITY_OK` | `out->node_id` 非空（`out->sn` 保证非空：读不到 `creds.sn` 时回落 `node_id`） | 照常起通道 |
 * | `ADV_IDENTITY_ERR_CREDS` | 参数非法 / `creds` 读失败 | **甲**：⛔ 不起通道；**乙 / 丙**：可**无身份**起通道（⛔ 仍不回落编译期常量） |
 * | `ADV_IDENTITY_ERR_EMPTY` | `creds` 读到了但 `node_id` 为空 | 同上 |
 *
 * ⛔ 任何失败路径下 `out` 都**全清零**（不留半截值）：广播里不得出现"看起来像 SN 的东西"，
 *    也不得回落编译期常量（那会让广播名与身份出现第二个来源）。
 * ⚠️ `reason` 可为 NULL（调用方不关心成因时）。
 */
adv_identity_status_t adv_identity_load_ex(adv_identity_t *out, adv_identity_status_t *reason);

/* 旧签名（**逐字保留**，⛔ 不删）：`true` ⇔ `adv_identity_load_ex(...) == ADV_IDENTITY_OK`。
 * ⚠️ 新代码请用 `_ex` 版 —— 只有它能区分"读失败"与"node_id 为空"，而乙 / 丙 的档位感知需要这个区分。 */
bool adv_identity_load(adv_identity_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ADV_IDENTITY_H */
