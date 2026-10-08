/*
 * adv_identity.c —— 广播厂商字段要用的**设备 SN**（= creds 分区的 node_id）
 *
 * 为什么单独一个 TU（**不是**把它写在 wifi_prov_ble.c 里）：
 *   `oneye_dev_creds.h` 会带进 `oneye_dev_base.h`，而该文件 `:153-164` 把「base 链路状态快照」
 *   这个结构体**误 typedef 成了 `oneye_dev_link_status_t`** —— 与 `oneye_dev_link.h:150-163` 的
 *   「link 信道状态」**同名不同构**。于是**同一个 TU 里同时包含 base.h 与 link.h 必然编译失败**：
 *       error: conflicting types for 'oneye_dev_link_status_t'; have 'struct <anonymous>'
 *   本工程里 `wifi_prov_ble.c` 需要 link.h（prov 帧），本文件需要 creds.h（node_id）⇒ 拆成两个 TU。
 *   ⛔ 这是 SDK 公共头文件的既有缺陷（任何集成方同时用 base 与 link 面都会撞），本批**只绕开、不修**：
 *      修它等于改公共 typedef（改名 = API/ABI 变更，影响其它例程），属另一次决策。
 *
 * 身份口径（与 oneye_start() 完全同源）：`oneye_dev_creds_load()` → `node_id`。
 *   ★ 2026-10-07（本批）：本函数是 BLE 配网路径身份的**单点读取** —— 返回值同时用于
 *     ① 广播名 `ONEYE-<后 4 位>`（SDK 派生）、② 配对 HKDF salt、③ 厂商字段 `sn`。
 *   ⛔ 读失败/为空 ⇒ 返回 false（调用方据此**本通道不启动 / 整段 0xFF 不产出**），
 *      ⛔ 不回落编译期 device_id（那会造成"广播名与 salt 有两个来源"）。
 * ⚠️ 代价：`oneye_dev_creds_load()` 按分区大小 malloc（本机 16 KiB）。故本函数**取完即 free**，
 *    调用点排在 NimBLE 起栈之前（见 wifi_prov_ble.c 的调用顺序注释）。
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "oneye_dev_creds.h"

#include "adv_identity.h"

static const char *TAG = "wifi_prov_ble";

bool adv_identity_sn(char *out, size_t cap, size_t *len_out)
{
    oneye_dev_creds_t creds;
    oneye_dev_sdk_err_t rc;

    if (out == NULL || cap == 0u) {
        return false;
    }
    out[0] = '\0';
    if (len_out != NULL) {
        *len_out = 0u;
    }

    rc = oneye_dev_creds_load(&creds);
    if (rc != ONEYE_DEV_SDK_OK) {
        /* 「没写过」与「坏了」对身份的处置**相同**（BLE 配网通道不启动、厂商字段不产出），但日志分开讲，
         * 与 oneye_start() 的失败姿态口径一致（合同 §2 冻结项）。 */
        ESP_LOGW(TAG, "身份不可用：creds 读取失败（%s）⇒ **BLE 配网通道不启动**（广播名/salt 无来源）"
                      "且厂商字段(0xFF) **整段不产出**（⛔ 不用编译期 device_id 冒充身份）",
                 oneye_dev_strerror(rc));
        return false;
    }
    if (creds.node_id == NULL || creds.node_id[0] == '\0') {
        ESP_LOGE(TAG, "身份不可用：creds 分区里 node_id 为空 ⇒ 不启动 BLE 配网、不产出厂商字段（⛔ 不冒充）");
        oneye_dev_creds_free(&creds);
        return false;
    }
    snprintf(out, cap, "%s", creds.node_id);
    if (len_out != NULL) {
        *len_out = strlen(out);
    }
    ESP_LOGI(TAG, "设备身份：**creds 分区** node_id=%s（%u B，crc32=%08x）"
                  "⇒ ① 广播名 ONEYE-<后 4 位>、② 配对 HKDF salt、③ 厂商字段 sn 三者同源于此",
             out, (unsigned)strlen(out), (unsigned)creds.image_crc32);
    oneye_dev_creds_free(&creds); /* 先把 16 KiB 还回去，再起 NimBLE（见文件头 ⚠️） */
    return (out[0] != '\0');
}
