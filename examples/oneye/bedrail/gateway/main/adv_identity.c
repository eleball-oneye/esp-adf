/*
 * adv_identity.c —— BLE 配网路径的**身份单点读取**：`creds` 分区的 `node_id` 与 `sn`
 *
 * 为什么单独一个 TU（**不是**把它写在 wifi_prov_ble.c 里）：
 *   `oneye_dev_creds.h` 会带进 `oneye_dev_base.h`，而该文件 `:153-164` 把「base 链路状态快照」
 *   这个结构体**误 typedef 成了 `oneye_dev_link_status_t`** —— 与 `oneye_dev_link.h:150-163` 的
 *   「link 信道状态」**同名不同构**。于是**同一个 TU 里同时包含 base.h 与 link.h 必然编译失败**：
 *       error: conflicting types for 'oneye_dev_link_status_t'; have 'struct <anonymous>'
 *   本工程里 `wifi_prov_ble.c` 需要 link.h（prov 帧），本文件需要 creds.h（身份）⇒ 拆成两个 TU。
 *   ⛔ 这是 SDK 公共头文件的既有缺陷（任何集成方同时用 base 与 link 面都会撞），本批**只绕开、不修**：
 *      修它等于改公共 typedef（改名 = API/ABI 变更，影响其它例程），属另一次决策。
 *
 * 🔴 2026-10-09（台账 #1 / #3 / #4）：**两串拆开**，⛔ 不再同源：
 *   · `node_id`（**私密**：激活码 = 设备证书 CN，⛔ 不广播、⛔ 不进明文帧，✅ 贴机身）
 *       ⇒ ① `lcfg.node_id`（link 帧 `from`）② **甲型** `prov.verify.req` 的**逐字核验基准**。
 *         ⚠️ **2026-10-10 订正**：它**不再是 HKDF 的 `salt`**（契约 §4.4.10（四）：`salt = chal`）、
 *         **也不再是** `prov.pair` 阶段的比对基准（§4.4.10（三）：配对不判码）。
 *   · `sn`（**公开**：贴机身、**可广播**）
 *       ⇒ ① 广播名 `ONEYE-<SN 后 4 位>` ② 厂商字段 `sn_field(24)` ③ `prov.pair.ok.sn`（条件必填）。
 *   ⛔ 读失败 ⇒ **甲**型本通道不启动（⛔ 不回落编译期 device_id）；**乙 / 丙** 可在**无实例身份**下起栈。
 * ★ `creds.sn` 为空 ⇒ **回落 `node_id`** 并置 `sn_fell_back`（**存量机行为不变**）：
 *   广播名 / 厂商字段仍取那一串，只是如实标注"非现役 v3 形态"（SDK 按串形自动置 0x04）。
 * ⚠️ 代价：`oneye_dev_creds_load()` 按分区大小 malloc（本机 16 KiB）。故本函数**取完即 free**，
 *    调用点排在 NimBLE 起栈之前（见 wifi_prov_ble.c 的调用顺序注释）。
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "oneye_dev_creds.h"

#include "adv_identity.h"

static const char *TAG = "wifi_prov_ble";

adv_identity_status_t adv_identity_load_ex(adv_identity_t *out, adv_identity_status_t *reason)
{
    oneye_dev_creds_t creds;
    oneye_dev_sdk_err_t rc;

    if (reason != NULL) {
        *reason = ADV_IDENTITY_ERR_CREDS;
    }
    if (out == NULL) {
        return ADV_IDENTITY_ERR_CREDS;
    }
    memset(out, 0, sizeof(*out));

    rc = oneye_dev_creds_load(&creds);
    if (rc != ONEYE_DEV_SDK_OK) {
        /* 「没写过」与「坏了」对身份的处置**相同**（甲：BLE 配网通道不启动），但日志分开讲，
         * 与 oneye_start() 的失败姿态口径一致（合同 §2 冻结项）。
         * 🔴 2026-10-10：本分支**不再**由本文件决定"通道起不起" —— 那个决定是**档位感知**的
         *    （甲 fail-closed / 乙·丙 可无身份起），见 `wifi_prov_ble.c` 的 `wifi_prov_ble_start()`。 */
        ESP_LOGW(TAG, "身份不可用：creds 读取失败（%s）⇒ **甲型**BLE 配网通道不启动；"
                      "乙 / 丙 档可在**无实例身份**下起栈（⛔ 仍不回落编译期 device_id 冒充身份）",
                 oneye_dev_strerror(rc));
        if (reason != NULL) {
            *reason = ADV_IDENTITY_ERR_CREDS;
        }
        return ADV_IDENTITY_ERR_CREDS;
    }
    if (creds.node_id == NULL || creds.node_id[0] == '\0') {
        ESP_LOGE(TAG, "身份不可用：creds 分区里 node_id 为空 ⇒ **甲型**不启动 BLE 配网；"
                      "乙 / 丙 档可在无身份下起栈（⛔ 不冒充）");
        oneye_dev_creds_free(&creds);
        if (reason != NULL) {
            *reason = ADV_IDENTITY_ERR_EMPTY;
        }
        return ADV_IDENTITY_ERR_EMPTY;
    }
    snprintf(out->node_id, sizeof(out->node_id), "%s", creds.node_id);

    if (creds.sn != NULL && creds.sn[0] != '\0') {
        snprintf(out->sn, sizeof(out->sn), "%s", creds.sn);
        out->sn_fell_back = false;
    } else {
        /* 存量机：`creds.sn` 没写过 ⇒ 回落用 `node_id`（**行为与 2026-10-07 那批一致**），
         * 并如实标注：SDK 会据串形把厂商字段标成"老世代"（0x04）。 */
        snprintf(out->sn, sizeof(out->sn), "%s", creds.node_id);
        out->sn_fell_back = true;
    }

    ESP_LOGI(TAG, "设备身份（creds 分区，crc32=%08x）："
                  "① 激活码 node_id=%s（私密：link 节点 id + `prov.pair.code` 的核验基准（甲）；⛔ 不再是 HKDF salt）；"
                  "② SN=%s（公开：广播名/厂商字段/prov.pair.ok，来源=%s，%u B）",
             (unsigned)creds.image_crc32, out->node_id, out->sn,
             out->sn_fell_back ? "**回落 node_id（存量机：creds.sn 为空）**" : "creds.sn",
             (unsigned)strlen(out->sn));
    oneye_dev_creds_free(&creds); /* 先把 16 KiB 还回去，再起 NimBLE（见文件头 ⚠️） */
    if (reason != NULL) {
        *reason = ADV_IDENTITY_OK;
    }
    return ADV_IDENTITY_OK;
}

bool adv_identity_load(adv_identity_t *out)
{
    return adv_identity_load_ex(out, NULL) == ADV_IDENTITY_OK;
}
