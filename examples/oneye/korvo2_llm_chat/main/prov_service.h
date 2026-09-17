/*
 * prov_service.h —— Wi-Fi 配网服务（本地面：BLE 自研 GATT + SmartConfig + 凭据文件）
 *
 * 三条通道**并行**，先成功者胜出（契约 contracts/local/README.md §4）：
 *   1. 凭据文件（台面/研发）：`/sdcard/oneye-wifi.txt` → `/spiffs/oneye-wifi.txt` → Kconfig 兜底；
 *   2. BLE（主通道，加密）：`oneye_dev_ble` 自研 GATT + POP 配对；AP 扫描/连接由本模块
 *      以 `oneye_dev_ble_prov_handlers_t` 注入（契约要求 Wi-Fi 生命周期归应用）；
 *   3. SmartConfig（备用，无 BLE 权限时）：ESP-IDF 内置 ESPTouch v2（设备侧不自研协议栈）。
 *
 * 链路侧（本地面）：
 *   - `oneye_dev_link`：本机作为**设备节点**注册/announce，转发配网状态（`prov.status`）；
 *   - BLE 通道由 `oneye_dev_ble_bind_to_link()` 绑定；局域网通道见 lan_link.h。
 *
 * 红线：Wi-Fi 密码**只**在内存与加密链路中出现，不落日志、不进状态面、不落盘（凭据文件除外，
 *  且仅在台面开关打开时读取）。
 */

#ifndef _PROV_SERVICE_H_
#define _PROV_SERVICE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_peripherals.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROV_SSID_MAX 33
#define PROV_IP_MAX   16

typedef struct {
    bool     connected;
    bool     ble_active;       /* BLE 广播中（未配网时） */
    bool     smartconfig;      /* SmartConfig 监听中 */
    char     ssid[PROV_SSID_MAX];
    char     ip[PROV_IP_MAX];
    char     source[16];       /* "file" | "kconfig" | "ble" | "smartconfig" | "api" */
    uint32_t attempts;         /* 连接尝试次数 */
    esp_err_t last_err;
} prov_status_t;

/** 联网成功回调（**事件任务上下文**调用：回调内只置标志/投递任务，勿做重活） */
typedef void (*prov_ready_cb_t)(void);

/** 初始化：nvs/事件循环/netif/Wi-Fi + 本地面节点注册 + BLE 配网 + 局域网链路（按 Kconfig） */
esp_err_t prov_service_init(esp_periph_set_handle_t periph_set, prov_ready_cb_t on_ready);

/** 启动配网流程：文件 → Kconfig 直连；都不可用则开启 BLE + SmartConfig 等待手机下凭据 */
esp_err_t prov_service_start(void);

/** 按凭据连接（阻塞至获得 IP 或超时）；供 BLE/SmartConfig/局域网通道调用 */
esp_err_t prov_service_connect(const char *ssid, const char *pass, uint32_t timeout_ms);

/** AP 扫描结果（JSON 数组；契约 ble-gatt.md `prov.scan.rsp` 的 `p.aps` 口径） */
esp_err_t prov_service_scan_json(char *out, size_t cap);

/** 清除已保存凭据并回到未配网态（BLE 解除配对、停止 SmartConfig） */
esp_err_t prov_service_forget(void);

bool prov_service_is_connected(void);
const char *prov_service_ip(void);
void prov_service_get_status(prov_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* _PROV_SERVICE_H_ */
