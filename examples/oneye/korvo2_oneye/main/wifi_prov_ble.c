/*
 * wifi_prov_ble.c —— 配网通道 C1/C2：BLE（口径见 wifi_prov_ble.h 与 ADR-0017）
 *
 * 本文件**只是通道**。它把凭据交给 `wifi_prov_submit_credentials()`，然后什么都不做 ——
 * 「入网成功 ⇒ 上云」由 wifi_prov.c 的 GOT_IP 处理器统一触发（唯一收敛点）。
 *
 * 与既有例程的关系：`examples/oneye/korvo2_llm_chat/main/prov_service.c:274-292/424-443`
 * 是同一套 SDK API（`oneye_dev_ble_*` + `oneye_dev_link_prov_set_provider`）的既有实现，
 * 真机已打通（见该例程 README §7.1）。本文件按 korvo2_oneye 的模块边界重写，**不复制**它的
 * 「入网后逻辑」（那一份在 llm_chat 里是调 `s_ready_cb()`；本工程收敛点在 wifi_prov.c）。
 */

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"

#include "oneye_dev_ble.h"
#include "oneye_dev_link.h"

#include "wifi_prov.h"
#include "wifi_prov_ble.h"

static const char *TAG = "wifi_prov_ble";

static bool s_started;
static bool s_advertising;
static char s_pop[ONEYE_DEV_BLE_POP_LEN];

/* ------------------------------------------------------------------ provider（应用侧 Wi-Fi 动作） */

/* 扫描：直接转发给 wifi_prov.c 的实现（Wi-Fi 生命周期归它管，本模块不碰 esp_wifi_*） */
static int ble_prov_scan(char *json_out, size_t cap, void *ctx)
{
    (void)ctx;
    return wifi_prov_scan_json(json_out, cap) == ESP_OK ? 0 : -1;
}

/*
 * 连接：**只**把凭据交给唯一入网入口，然后返回。
 * ⛔ 这里不调 oneye_start()、不建任务、不碰面板/授时/影子/埋点 —— 那些都在收敛点的下游。
 * ⚠️ 本函数在 NimBLE 主机任务上下文（`ble_on_rx → oneye_dev_link_inject_frame → link_handle_prov`
 *    → 本 provider，见 `oneye_dev_sdk/src/oneye_dev_ble.c:202-207` 与 `oneye_dev_link.c:289-320`）
 *    里被**同步**调用，因此会阻塞该任务至多 `wifi_prov_connect` 的超时（缺省 20 s）。
 *    这与既有例程 `korvo2_llm_chat/main/prov_service.c:280-287` 的口径一致；
 *    不另起任务的原因见本工程 `korvo2_oneye_main.c` 关于内部 RAM（`largest_internal_block`）的实测注释。
 * ★ 被优先级判据拒绝时（`ESP_ERR_INVALID_STATE`：槽里已有 SD 卡凭据，或设备已入网 ⇒ 只入网一次）
 *   本函数返回 **-1**，立刻返回、**不阻塞**；SDK 据此向对端报 `PROV_FAILED`
 *   （`oneye_dev_link.c:317-320`）⇒ 手机侧能立刻知道"这一次没被采信"，而不是静默超时。
 */
static int ble_prov_connect(const char *ssid, const char *password, const char *token, void *ctx)
{
    (void)token;    /* 契约 §4 的 rvd 自定义数据：korvo2_oneye 暂不使用（不臆造语义） */
    (void)ctx;
    ESP_LOGI(TAG, "BLE 通道收到凭据：ssid=%s（密码不打印）⇒ 交给唯一入网入口（含优先级判据）", ssid);
    esp_err_t rc = wifi_prov_submit_credentials(ssid, password, WIFI_PROV_CHAN_BLE);
    if (rc == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "本次 BLE 凭据**未被采信**（优先级判据拒绝；串口 [prov-priority] 有留痕）"
                      "⇒ 向对端报失败，不发起入网");
    }
    return (rc == ESP_OK) ? 0 : -1;
}

/* 清除凭据：只断开并清 Wi-Fi 配置，然后重新广播（不重启、不擦 NVS） */
static int ble_prov_reset(void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "BLE 通道请求清除凭据 ⇒ 断开并回到未配网态（重新广播）");
    wifi_prov_forget();
    if (s_started) {
        (void)oneye_dev_ble_unpair();
        if (oneye_dev_ble_advertise(true) == ONEYE_DEV_SDK_OK) {
            s_advertising = true;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ 通道启动 */

esp_err_t wifi_prov_ble_start(void)
{
#if !CONFIG_ONEYE_FW_ENABLE_BLE_PROV
    ESP_LOGW(TAG, "BLE 配网通道未启用（CONFIG_ONEYE_FW_ENABLE_BLE_PROV=n）⇒ 本机型应改用其它通道");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_started) {
        if (!s_advertising && oneye_dev_ble_advertise(true) == ONEYE_DEV_SDK_OK) {
            s_advertising = true;
        }
        return ESP_OK;
    }

    /* 1) link：prov.* 帧的内置处理器在 link 层（oneye_dev_link.c:267-331），必须先于 ble 起来。
     *    本工程只用它的 ble 信道，不启用 lan/wan（那两条是别的通道/别的决策）。 */
    if (!oneye_dev_link_is_initialized()) {
        oneye_dev_link_config_t lcfg;
        memset(&lcfg, 0, sizeof(lcfg));
        ONEYE_DEV_STRUCT_INIT(lcfg);
        oneye_dev_link_config_defaults(&lcfg);
        lcfg.node_id = CONFIG_ONEYE_FW_DEVICE_ID;
        /* 只跑 BLE：不给 wan_uri ⇒ wan 信道不启用；allow_plaintext_chan 保持缺省
         * （本工程没有 lan 信道，明文口径无从触发）。 */
        oneye_dev_sdk_err_t lrc = oneye_dev_link_init(&lcfg);
        if (lrc != ONEYE_DEV_SDK_OK) {
            ESP_LOGE(TAG, "link 初始化失败（%d）⇒ BLE 配网通道不可用", (int)lrc);
            return ESP_FAIL;
        }
        (void)oneye_dev_link_start();
    }

    /* 2) ble：注册 provider（Wi-Fi 动作仍由本工程实现，SDK 不代管 Wi-Fi） */
    oneye_dev_ble_config_t bcfg;
    memset(&bcfg, 0, sizeof(bcfg));
    ONEYE_DEV_STRUCT_INIT(bcfg);
    oneye_dev_ble_config_defaults(&bcfg);
    bcfg.device_id = CONFIG_ONEYE_FW_DEVICE_ID;
    bcfg.auto_advertise = false;      /* 起完再显式开始广播，便于把结果讲清楚 */
    bcfg.prov.scan = ble_prov_scan;
    bcfg.prov.connect = ble_prov_connect;
    bcfg.prov.reset = ble_prov_reset;

    oneye_dev_sdk_err_t rc = oneye_dev_ble_init(&bcfg);
    if (rc != ONEYE_DEV_SDK_OK) {
        ESP_LOGE(TAG, "BLE 初始化失败（%d）⇒ 本通道不可用（其余通道不受影响）", (int)rc);
        return ESP_FAIL;
    }
    if (CONFIG_ONEYE_FW_BLE_POP[0] != '\0') {
        /* 固定 POP（产测/台面）；不是 6 位数字时 SDK 会拒，这里退回随机 POP 而不是硬失败 */
        if (oneye_dev_ble_set_pop(CONFIG_ONEYE_FW_BLE_POP) != ONEYE_DEV_SDK_OK) {
            ESP_LOGW(TAG, "固定 POP「%s」被拒（须 6 位数字）⇒ 改用 SDK 随机 POP",
                     CONFIG_ONEYE_FW_BLE_POP);
        }
    }
    rc = oneye_dev_ble_start();
    if (rc != ONEYE_DEV_SDK_OK) {
        ESP_LOGE(TAG, "BLE 启动失败（%d）⇒ 本通道不可用", (int)rc);
        return ESP_FAIL;
    }
    /* 3) 绑定为 link 的 ble 信道发送器（链路回帧用；失败只影响 link 帧，不影响配网本身） */
    if (oneye_dev_ble_bind_to_link() != ONEYE_DEV_SDK_OK) {
        ESP_LOGW(TAG, "BLE 绑定 link 信道失败 ⇒ link/组域帧不走 BLE（配网仍可用）");
    }
    if (oneye_dev_ble_advertise(true) == ONEYE_DEV_SDK_OK) {
        s_advertising = true;
    } else {
        ESP_LOGW(TAG, "BLE 开始广播失败 ⇒ 手机可能扫不到本机");
    }

    s_started = true;
    s_pop[0] = '\0';
    if (oneye_dev_ble_get_pop(s_pop, sizeof(s_pop)) == ONEYE_DEV_SDK_OK) {
        ESP_LOGI(TAG, "BLE 配网通道已就绪：广播中（设备名 ONEYE-<id 后 4 位>），"
                      "配对 POP=%s（TTL %d s）", s_pop, CONFIG_ONEYE_DEV_SDK_BLE_POP_TTL_S);
    } else {
        s_pop[0] = '\0';
        ESP_LOGI(TAG, "BLE 配网通道已就绪：广播中（POP 读回失败，串口另有 SDK 日志）");
    }
    return ESP_OK;
#endif /* CONFIG_ONEYE_FW_ENABLE_BLE_PROV */
}

bool wifi_prov_ble_active(void)
{
    return s_started && s_advertising;
}

const char *wifi_prov_ble_pop(void)
{
    return s_pop;
}

void wifi_prov_ble_stop_advertising(void)
{
    if (!s_started || !s_advertising) {
        return;
    }
    if (oneye_dev_ble_advertise(false) == ONEYE_DEV_SDK_OK) {
        s_advertising = false;
        ESP_LOGI(TAG, "已入网 ⇒ 停止 BLE 广播（契约：入网成功后不再保持可配网态）");
    }
}
