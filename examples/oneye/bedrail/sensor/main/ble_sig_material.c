/*
 * ble_sig_material.c —— 乙（BETA）档 `prov.pair.ok.sig` 的**签名材料注入点**（实现）
 *
 * 详细口径（为什么必须有这个 TU / 为什么单独一个 TU / 四条硬边界）见 `ble_sig_material.h`。
 *
 * ⛔ 本文件**只**包含 `oneye_dev_ble.h`：`oneye_dev_link.h` 里有同名不同构的
 *    `oneye_dev_link_status_t`，同一 TU 同时包含两者必编译失败（与 `adv_identity.c` 同因）。
 *    因此"注入"这件事不走结构调整体，而走**窄接口**
 *    （`oneye_dev_ble_sig_material_apply(const char *, const char *)`）。
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "oneye_dev_ble.h"

#include "ble_sig_material.h"

static const char *TAG = "devkitc_sig";

/* 借用指针（⛔ 不拷贝）：blob 由链接器生成，生命周期 = 整个镜像。 */
static const char *s_cert;
static const char *s_key;
static bool s_present;

esp_err_t devkitc_ble_sig_material_set(const char *cert_pem, size_t cert_len,
                                       const char *key_pem, size_t key_len)
{
    if (cert_pem == NULL || key_pem == NULL || cert_len == 0u || key_len == 0u) {
        /* ⚠️ 参数非法 ⇒ 如实清掉"有材料"这一位（⛔ 不留半截状态）。
         * 不动 s_cert / s_key：它们仍指向旧材料，但 s_present=false ⇒ 调用方不会注入。 */
        s_present = false;
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 规范化成以 NUL 结尾的 C 串（SDK 的 `sig_cert_pem` / `sig_key_pem` 是 `const char *`
     * ⇒ 它按 C 串解析 PEM）。objcopy 生成的 blob **末尾带一个 NUL**，但那是"多一个字节"，
     * 不能当作"已经是 C 串"来依赖：若材料里 PEM 之后还有别的字节，直接当 C 串用会把垃圾
     * 一并交给解析器。这里逐字节检查**最后一个有效字节必须就是 NUL**：
     *   · 是 ⇒ 直接用（最常见的形态，对象是 `_binary_client_crt_start`）；
     *   · 否 ⇒ 拒绝（⛔ 不自己拼一个缓冲区出来：那会引入一个会失效的临时生命周期）。
     */
    if (cert_pem[cert_len - 1u] != '\0' || key_pem[key_len - 1u] != '\0') {
        ESP_LOGE(TAG, "证书/私钥 blob 末尾不是 NUL ⇒ 拒绝注入（⛔ 不把非 C 串交给 PEM 解析器；"
                      "cert_len=%u key_len=%u）",
                 (unsigned)cert_len, (unsigned)key_len);
        s_present = false;
        return ESP_ERR_INVALID_ARG;
    }

    s_cert = cert_pem;
    s_key = key_pem;
    s_present = true;

    /* ⛔ 只打字节数，⛔ 不打任何材料内容。 */
    ESP_LOGI(TAG, "乙档 sig 材料已就绪（cert %u B / key %u B）⇒ 档位=乙 时 SDK 会对 chal 签名"
                  "（签不出来时 SDK 会**不发** prov.pair.ok，fail-closed）",
             (unsigned)cert_len, (unsigned)key_len);
    return ESP_OK;
}

bool devkitc_ble_sig_material_present(void)
{
    return s_present;
}

/*
 * 🔴 供 `main/wifi_prov_ble.c` 调用的**窄接口**（唯一的调用点）。
 *
 * 为什么参数是 `const char *` 而不是结构体：`oneye_dev_ble_config_t` 在
 * `oneye_dev_ble.h` 里，而 `wifi_prov_ble.c` **已经**包含那个头 —— 但把结构体类型写进
 * 本文件的签名会逼本文件也把它的完整定义带进来（本文件已经带了，无妨），真正的问题是
 * **调用点**：`wifi_prov_ble.c` 里那一段在 `oneye_dev_ble_config_defaults(&bcfg)` 之后，
 * 用两个 `const char *` 出参最不容易写错（⛔ 不会有人误以为要在这里 memset 配置结构）。
 *
 * 返回：
 *   ESP_OK            —— 已写入（出参非 NULL，至少 cert 非 NULL）；
 *   ESP_ERR_NOT_FOUND —— 本构建没有材料（丙档正常形态）⇒ ⛔ 不动出参，调用方不设置字段。
 */
esp_err_t oneye_dev_ble_sig_material_apply(const char **cert_pem_out, const char **key_pem_out)
{
    if (cert_pem_out == NULL || key_pem_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_present) {
        return ESP_ERR_NOT_FOUND;
    }
    *cert_pem_out = s_cert;
    *key_pem_out = s_key;
    return ESP_OK;
}
