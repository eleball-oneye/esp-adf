/*
 * ble_sig_material.h —— 乙（BETA）档 `prov.pair.ok.sig` 的**签名材料注入点**
 *
 * ============================================================================
 * ★ 为什么需要这个 TU（而不是在 main.c 里直接写）★
 *
 * 乙档的 `prov.pair.ok` 有一个**可选**字段 `sig`（契约 `contracts/local/link-frames.json`
 * 的 `frames[prov.pair.ok].optional`）：
 *   **内嵌证书对本次 `chal`（16 B 原始字节）的签名**、base64。
 *
 * SDK 侧的产出条件是**两条同时成立**（`src/oneye_dev_ble.c` 的 `ble_sig_enabled()`）：
 *   ① 档位**显式**为机型乙（`adv_device_tier_set == true` 且
 *      `adv_device_tier == ONEYE_DEV_TIER_B_CERT_DEVICE`）；
 *   ② `oneye_dev_ble_config_t` 的 `sig_cert_pem` / `sig_key_pem` **两个 PEM 都给了**。
 *
 * ⚠️ **本文件存在的唯一理由 = ②**：`main/wifi_prov_ble.c` 在 `oneye_dev_ble_config_defaults()`
 *    之后**没有**填这两个字段（逐字核对源目录 `wifi_prov_ble.c` 的 `bcfg` 赋值块，
 *    那里只设了 `device_id` / `adv_sn` / `adv_cred_required` / `adv_device_tier` /
 *    `auto_advertise` / `prov.*`）⇒ 乙档即使嵌入了证书，`sig` 也**不会被产出**。
 *    ⇒ 本文件提供一个**最小的注入点**，由 `main/devkitc_oneye_main.c` 在起 BLE 之前调用。
 *
 * ★ 为什么单独一个 TU（⛔ 与 `adv_identity.c` 同一个成因）★
 *   `oneye_dev_ble.h` 带进 `oneye_dev_base.h`，而 `oneye_dev_link.h` 里有**同名不同构**的
 *   `oneye_dev_link_status_t` ⇒ 同一 TU 同时包含两者必编译失败。
 *   本文件**只**包含 `oneye_dev_ble.h`（不碰 link 面），把注入函数做成
 *   `(const char *, const char *)` 这样的普通签名 ⇒ 对调用方零头文件耦合。
 *
 * ⛔ 硬边界（⛔ 不写就会被读成"乙档已经有 sig 了"）：
 *   ① 本文件⛔ **不产生**任何材料、⛔ 不读文件、⛔ 不建连接；
 *   ② 材料来自构建期 `-DONEYE_FW_CERT_DIR=<目录>`（见 `main/CMakeLists.txt`）的
 *      `ca.crt` / `client.crt` / `client.key` 三件 —— 本文件只用 **client.crt** 与
 *      **client.key**（`ca.crt` 是 TLS 信任链，与 `sig` 无关）；
 *   ③ ⛔ **不谎报**：材料缺一件 ⇒ 返回 `ESP_ERR_NOT_FOUND` 并把 `sig_enabled` 置 `false`
 *      ⇒ 乙档**不带 `sig`**（SDK 侧 `sig` 是可选字段，见
 *      `docs/API-ble.md` 与 `include/oneye_dev_ble.h` 的失败姿态说明）；
 *   ④ ⛔ 不把证书/私钥内容打进任何日志（只打**字节数**）。
 */

#ifndef DEVKITC_BLE_SIG_MATERIAL_H
#define DEVKITC_BLE_SIG_MATERIAL_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 让**下一次** `oneye_dev_ble_init()` 带上 `sig` 的签名材料（乙档专用）。
 *
 * 参数：
 *   cert_pem / cert_len —— `client.crt` 的整个 PEM 文本（可为 NUL 结尾或**不含** NUL）
 *   key_pem  / key_len  —— `client.key` 的整个 PEM 文本（同上）
 *
 * 返回：
 *   ESP_OK           —— 材料齐备且指针已记住（**借用**，⛔ 不拷贝：blob 是静态的，活到进程结束）；
 *   ESP_ERR_INVALID_ARG —— 参数为空 / 长度为 0；
 *   ESP_ERR_NOT_FOUND —— 本构建**没有**嵌入证书材料（`ONEYE_FW_EMBED_CERTS` 未定义）。
 *                        ⚠️ 不是错误路径的"异常"，而是丙档的正常形态。
 */
esp_err_t devkitc_ble_sig_material_set(const char *cert_pem, size_t cert_len,
                                       const char *key_pem, size_t key_len);

/* 本构建是否**真的**拿到了可用的签名材料（= 上面返回过 `ESP_OK`）。
 * ⚠️ 它只回答"注入过没有"，⛔ 不回答"证书与私钥配对"——后者由 SDK 在签名时校验，
 *    签不出来时 SDK 会**不发** `prov.pair.ok`（fail-closed），本文件⛔ 不代它表态。 */
bool devkitc_ble_sig_material_present(void);

/*
 * 🔴 **唯一的调用点**在 `main/wifi_prov_ble.c` 的 `wifi_prov_ble_start()`：
 *   `oneye_dev_ble_config_defaults(&bcfg)` **之后**、`oneye_dev_ble_init(&bcfg)` **之前**。
 *
 * 为什么调用点放在那里而不是 `main/devkitc_oneye_main.c`：`bcfg` 是 `wifi_prov_ble.c` 的
 * **局部变量**，通道模块之外拿不到它 —— 若在 main 里另起一份配置结构，就会出现**两个事实源**
 * （档位/设备 id/厂商字段在一边、sig 材料在另一边），那正是"看起来配了、其实没生效"的成因。
 *
 * 参数 = 两个出参（借用，⛔ 不拷贝、⛔ 不释放）：
 *   ESP_OK            —— 已写入（两个出参都非 NULL）；
 *   ESP_ERR_NOT_FOUND —— 本构建没有材料（**丙档的正常形态**）⇒ ⛔ 不动出参、⛔ 不报错级日志。
 */
esp_err_t oneye_dev_ble_sig_material_apply(const char **cert_pem_out, const char **key_pem_out);

#ifdef __cplusplus
}
#endif

#endif /* DEVKITC_BLE_SIG_MATERIAL_H */
