/*
 * prov_identity.h —— **会话级节点身份**：`prov.verify.req` 交来的激活码（= `node_id`）
 *
 * 为什么需要这个文件（2026-10-20 批，成因见报告 `TLS-MEM-NODE-IDENTITY-FIX-20261020.md`）：
 *   乙 / 丙 的物理事实是「**上平台前无实例身份**」（契约 §4.4.8（〇））⇒ 设备自带**没有**
 *   一个"我就是谁"的值；真正属于这台设备的 `node_id` 是 **App 在配网时经 `prov.verify.req`
 *   交来的激活码**。而在本批之前，`main/devkitc_oneye_main.c` 建云链路时用的是
 *   **编译期** `CONFIG_ONEYE_FW_DEVICE_ID`（`devkitc-0001`）⇒ 真机出现两套身份：
 *     设备侧 `base initialized: node=devkitc-0001`
 *     云端 / 库侧 `DEVKITC0001`（激活码）
 *   ⇒ MQTT 的 `username` / `client_id` / 主题前缀全落在**占位名**上，平台侧认不出这台设备。
 *
 * 本文件只做一件事：**把那一帧里的 `code` 记进 RAM，供建云链路时取用**。
 *
 * ------------------------------------------------------------------ 三条硬边界
 * ① **尽力写 nvs，失败不阻断**（🔴 2026-10-20 用户裁定，**旧边界逐字留痕、⛔ 不抹**）：
 *    旧字逐字为「⛔ **不落盘**：本模块**没有** nvs/分区写路径（⛔ 不写 `creds` 分区、
 *    ⛔ 不新开 nvs 命名空间）。"上电后还能不能拿到身份"是**另一件事**（SDK 自己的 W1/W2
 *    记录，见报告"未做/需裁定"段），⛔ 不在本模块偷偷补一个持久化。」
 *    用户裁定逐字：「**乙型可能无 nvs 等分区**：**只尝试写 nvs**，写失败**不阻断**
 *    （App 重新配网上平台即可；身份恢复没那么重要）」＋「⛔ 不要为此改 SDK 加公开 getter，
 *    也不要持久化失败就让流程失败」。
 *    ⇒ 现值：新增 [prov_identity_nvs_save] / [prov_identity_nvs_load]（命名空间 `oneye_prov`、
 *    键 `node`），**只**做"尽力而为"：任何 `esp_err_t` 失败都只**打日志**，调用方**照常**用 RAM
 *    身份建云链路（⛔ 不 return、⛔ 不改上云判据、⛔ 不写 `creds` 分区、⛔ 不动 SDK 源码）。
 *    ⚠️ **不与 SDK 的 W1/W2 记录抢同一份事实**：SDK 写的是它自己的绑定记录（含 `commit` 位），
 *    本模块写的是"App 在 `prov.verify.req` 交来的激活码"这一个串；两者命名空间/键都不同、
 *    语义不同（本模块**不声称**"云侧已确认"，只声称"上次用过这个 node"）。
 * ② ⛔ **不是身份证明**：本处只如实转存 App 交来的串；"这个 node 是不是这台设备"的**权威在云侧准入**
 *    （与乙型 `prov.verify` 的判据同向：设备侧只做形态/有效性）。
 * ③ ⛔ **不改 SDK、不改协议**：取值方式是 **SDK 的公开 API** —— `oneye_dev_ble_config_t.frame_cb`
 *    （`oneye_dev_ble.c` 在**每一**帧解密后、内置处理**之前**调用它）⇒ 不需要 internal 头、
 *    不需要新增帧/字段。
 */

#ifndef PROV_IDENTITY_H
#define PROV_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 激活码上界（与 SDK `ONEYE_BLE_CODE_MAX` / 契约 `code_charset` 的 `{1,64}` 同源） */
#define PROV_IDENTITY_NODE_MAX 64

/*
 * 帧观察者：把 `oneye_dev_ble_config_t.frame_cb` 指向本函数即可（**在 NimBLE 主机任务上下文执行**，
 * 本实现只做"识别 + 定长拷贝"，⛔ 不阻塞、⛔ 不打日志刷屏：只在**值变化**时打一行）。
 *
 * 只认 `t == "prov.verify.req"` 的帧；`code` 不满足 `^[A-Za-z0-9._-]{1,64}$` ⇒ **不记**
 * （并如实打一行"形态非法、不采用"—— ⛔ 不截断、⛔ 不 trim 后采用）。
 */
void prov_identity_frame_cb(const char *json, size_t len, void *ctx);

/*
 * 取本会话已记录的节点身份。
 *   · 返回 `true`：`out` 里是 NUL 结尾的 node（≤ 64 B）；
 *   · 返回 `false`：**还没有**收到过合法的 `prov.verify.req`（`out` 置空串）。
 * ⛔ 调用方**不得**把 `false` 读成"身份非法"：乙 / 丙 的正常起点就是"没有"。
 */
bool prov_identity_get(char *out, size_t cap);

/* 诊断用：本会话记过几次（0 = 没收到过；>1 = App 重发了同一帧，W2 形态） */
unsigned prov_identity_hits(void);

/*
 * 🔴 **2026-10-20〔node 身份「尽力写 nvs」批〕**：把当前 RAM 里的 node **尽力**写进 nvs。
 *
 * 返回 `esp_err_t`（`ESP_OK` = 写＋commit 都成功）。**调用方⛔ 不得把非 OK 当失败流程**：
 * 用户裁定「写失败**不阻断**」⇒ 调用方只打日志、继续用 RAM 里的身份。
 *
 * 硬边界（⚠️ 逐条都是"不改"）：
 *   · ⛔ 不写 `creds` 分区、⛔ 不碰 SDK 的绑定记录命名空间；
 *   · ⛔ 不因本函数失败改 `base_cfg` 的任何字段、⛔ 不让启动/上云 return；
 *   · 无 `nvs` 分区 / `nvs_flash_init` 失败 ⇒ 这里必然返回错误（`nvs_open` 失败）
 *     ⇒ 调用方照常上云（乙型可能**没有** nvs 分区，这是**合法配置**）。
 *
 * @param node NUL 结尾的 node（必须满足 `^[A-Za-z0-9._-]{1,64}$`；不满足 ⇒ `ESP_ERR_INVALID_ARG`）
 */
esp_err_t prov_identity_nvs_save(const char *node);

/*
 * 🔴 **2026-10-20〔node 身份「尽力写 nvs」批〕**：**身份面（`oneye_prov`）可写性**的只读探测。
 *
 * 为什么它在开机就调一次：乙型**可能没有 nvs 分区**，而"写失败不阻断"这条裁定的**前提事实**
 * （到底有没有 nvs 可写面）不该等到 App 配网那一刻才第一次暴露。返回 `ESP_OK` = 命名空间
 * 能以读写方式打开（媒介前提成立）；其余 = 如实打印 `esp_err_to_name`，⛔ 不影响启动与上云。
 *
 * ⚠️ 它**不写任何键**、⛔ 不创建身份、⛔ 不改任何判据。
 */
esp_err_t prov_identity_nvs_probe(void);

/*
 * 从 nvs 读回上次写入的 node（**只服务"本次上电没收到 `prov.verify.req`"的兜底**）。
 *
 *   · 返回 `true`：`out` 里是 NUL 结尾且**形态合法**的 node；
 *   · 返回 `false`：没有记录 / nvs 不可用 / 形态非法（`out` 置空串）。
 * ⛔ 调用方**不得**把 `false` 读成故障：乙型的正常起点就是"没有"（App 重新配网即可）。
 */
bool prov_identity_nvs_load(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PROV_IDENTITY_H */
