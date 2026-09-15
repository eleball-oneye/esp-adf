/*
 * wifi_prov.h —— Wi-Fi 配网（SD 卡凭据文件优先，Kconfig 兜底；运行期可由验证面板改配）
 *
 * 凭据文件（密钥=值，逐行；`#` 注释；两侧空白与 CR 自动去除）：
 *   ssid=MyWiFi
 *   password=MyPass          # pass / psk 亦可
 *
 * 查找顺序：`/sdcard/oneye-wifi.txt` → `/spiffs/oneye-wifi.txt` → Kconfig(`CONFIG_ONEYE_FW_WIFI_SSID/_PASSWORD`)
 *
 * ⚠️ 安全口径：SD 卡上是**明文** Wi-Fi 密码，仅用于**台面/研发配网**（用户明确选定的方式）。
 *   量产形态不得依赖该路径：应走 claim + 一机一密凭据（见 backend 契约与 ADR-0007），
 *   并在量产固件中关闭本文件的读取（`CONFIG_ONEYE_FW_ENABLE_WIFI_FILE=n`）。
 */

#ifndef _WIFI_PROV_H_
#define _WIFI_PROV_H_

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PROV_SSID_MAX 33
#define WIFI_PROV_PASS_MAX 65

/** 初始化 netif/event/wifi（幂等）；不连接 */
esp_err_t wifi_prov_init(void);

/** 从 SD/SPIFFS 凭据文件读取（找到返回 ESP_OK 并回填；未找到返回 ESP_ERR_NOT_FOUND） */
esp_err_t wifi_prov_load_file(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap,
                              char *src_path, size_t path_cap);

/** 连接（阻塞至获得 IP 或超时）；timeout_ms = 0 取 20000 */
esp_err_t wifi_prov_connect(const char *ssid, const char *pass, uint32_t timeout_ms);

/** 记录凭据来源（展示用："file:/sdcard/..." / "kconfig" / "api"） */
esp_err_t wifi_prov_set_source(const char *src);

/** 联网就绪回调：获得 IP 时触发（含运行期改配后的重新入网）。
 *  ⚠️ 在系统事件任务上下文调用，回调内不要做重活——需要时自行建任务。 */
typedef void (*wifi_prov_ready_cb_t)(void);
void wifi_prov_set_ready_cb(wifi_prov_ready_cb_t cb);

bool wifi_prov_is_connected(void);
const char *wifi_prov_ip(void);        /* "" = 未连接 */
const char *wifi_prov_ssid(void);      /* 当前/最近使用的 SSID */
const char *wifi_prov_source(void);    /* "file:/sdcard/..." | "kconfig" | "api" | "" */

#ifdef __cplusplus
}
#endif

#endif /* _WIFI_PROV_H_ */
