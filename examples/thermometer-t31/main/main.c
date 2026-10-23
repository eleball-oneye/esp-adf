/*
 * thermometer-t31 —— ESP32-C6-GEEK 上接入健奥 T31 体温贴的最小例程
 *
 * 流程：
 *   1) NVS / 网络栈初始化
 *   2) oneye-link-sdk 初始化 + 注册内置档案（T31、Oneye 标准 TLV）
 *   3) （可选）连 Wi-Fi → 启动 MQTT 上行
 *   4) 启动 SDK：扫描 → 连接 T31 → 订阅 FFF1 → 收帧 → 解码/透传 → 上云
 *
 * 说明：
 *   - T31 温度编码尚未解出：画面上看到的是 TLV 0x10 原始帧 + 0xB1 电量，
 *     云端可按原始帧解析；解出后在 profiles/t31.json 加一行字段即可（固件不变）。
 *   - 不配 Wi-Fi（SSID 留空）时只做本地打印，便于先验证 BLE 链路。
 */

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "oneye_link.h"
#include "oneye_link_profile.h"
#include "oneye_link_tlv.h"
#include "oneye_link_uplink.h"

static const char *TAG = "t31";

#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_evt;

/* ------------------------------------------------------------ Wi-Fi */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi 断开，3s 后重连");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi 已连接: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(void)
{
    if (strlen(CONFIG_ONEYE_T31_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "未配置 Wi-Fi（SSID 为空）→ 仅本地模式");
        return false;
    }
    s_wifi_evt = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    wifi_config_t wc = { 0 };
    (void)snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", CONFIG_ONEYE_T31_WIFI_SSID);
    (void)snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s",
                   CONFIG_ONEYE_T31_WIFI_PASSWORD);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(20000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* ------------------------------------------------------------ SDK 事件 */

static void on_link_event(const oneye_link_event_t *evt, void *ctx)
{
    (void)ctx;
    char addr[18] = { 0 };
    oneye_link_addr_str(&evt->sub, addr, sizeof(addr));

    switch (evt->type) {
    case ONEYE_LINK_EVT_SUB_ONLINE:
        ESP_LOGI(TAG, "[上线] %s model=%s rssi=%d", addr, evt->model, evt->rssi);
        break;
    case ONEYE_LINK_EVT_SUB_OFFLINE:
        ESP_LOGW(TAG, "[离线] %s model=%s", addr, evt->model);
        break;
    case ONEYE_LINK_EVT_SUB_DATA: {
        char text[160] = { 0 };
        if (evt->tlv && evt->tlv_len) {
            (void)oneye_link_tlv_format(evt->tlv, evt->tlv_len, text, sizeof(text));
        }
        ESP_LOGI(TAG, "[数据] %s cmd=0x%02X %s", addr, evt->has_cmd ? evt->cmd : 0, text);
        break;
    }
    case ONEYE_LINK_EVT_SUB_ERROR:
        ESP_LOGW(TAG, "[错误] %s err=%d", addr, (int)evt->err);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------ 统计 */

static void stats_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        oneye_link_stats_t st;
        if (oneye_link_get_stats(&st) != ONEYE_LINK_OK) {
            continue;
        }
        ESP_LOGI(TAG, "统计: 在线=%u 已知=%u 收帧=%llu 解析=%llu 透传=%llu 丢弃=%llu "
                      "上行=%llu 缓存=%llu 云=%d",
                 (unsigned)st.subdevs_online, (unsigned)st.subdevs_known,
                 (unsigned long long)st.frames_rx, (unsigned long long)st.frames_parsed,
                 (unsigned long long)st.frames_passthrough,
                 (unsigned long long)st.frames_dropped,
                 (unsigned long long)st.uplink_sent, (unsigned long long)st.uplink_queued,
                 (int)st.cloud_connected);
        size_t n = oneye_link_subdev_count();
        for (size_t i = 0; i < n; i++) {
            oneye_link_subdev_info_t info;
            if (!oneye_link_subdev_get(i, &info)) {
                continue;
            }
            char a[18] = { 0 };
            oneye_link_addr_str(&info.addr, a, sizeof(a));
            ESP_LOGI(TAG, "  子设备 %s model=%s state=%d rssi=%d batt=%u frames=%llu",
                     a, info.model, (int)info.state, info.rssi, info.battery,
                     (unsigned long long)info.frames);
        }
    }
}

/* ------------------------------------------------------------ main */

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "oneye-link-sdk %s · thermometer-t31 例程启动", oneye_link_version());

    /* 1) SDK 初始化 */
    oneye_link_config_t cfg = { 0 };
    ONEYE_LINK_STRUCT_INIT(cfg);
    cfg.node_id = CONFIG_ONEYE_T31_NODE_ID;
    cfg.max_links = CONFIG_ONEYE_T31_MAX_LINKS;
    cfg.scan_interval_ms = 1000;
    cfg.scan_window_ms = 200;
    cfg.cache_entries = 512;
    cfg.passthrough_unknown = CONFIG_ONEYE_T31_PASSTHROUGH_UNKNOWN;
    ESP_ERROR_CHECK(oneye_link_init(&cfg) == ONEYE_LINK_OK ? ESP_OK : ESP_FAIL);

    /* 2) 注册内置档案：T31 + Oneye 标准 TLV 设备 */
    const char *t31 = oneye_link_profile_builtin_t31();
    const char *tlv = oneye_link_profile_builtin_oneye_tlv();
    if (oneye_link_profile_register_json(t31, strlen(t31)) != ONEYE_LINK_OK) {
        ESP_LOGE(TAG, "T31 档案注册失败");
    }
    if (oneye_link_profile_register_json(tlv, strlen(tlv)) != ONEYE_LINK_OK) {
        ESP_LOGW(TAG, "Oneye TLV 档案注册失败（可忽略）");
    }

    /* 3) 事件回调 */
    oneye_link_register_event_cb(on_link_event, NULL);

    /* 4) 可选：Wi-Fi + MQTT 上行 */
    if (wifi_connect()) {
        oneye_link_mqtt_cfg_t mq = { 0 };
        ONEYE_LINK_STRUCT_INIT(mq);
        mq.uri = CONFIG_ONEYE_T31_MQTT_URI;
        mq.username = strlen(CONFIG_ONEYE_T31_MQTT_USERNAME) ? CONFIG_ONEYE_T31_MQTT_USERNAME : NULL;
        mq.password = strlen(CONFIG_ONEYE_T31_MQTT_PASSWORD) ? CONFIG_ONEYE_T31_MQTT_PASSWORD : NULL;
        mq.keepalive_s = 60;
        if (oneye_link_uplink_mqtt_start(&mq) != ONEYE_LINK_OK) {
            ESP_LOGW(TAG, "MQTT 启动失败 → 降级为本地模式（数据进缓存）");
        }
    }

    /* 5) 启动接入 */
    ESP_ERROR_CHECK(oneye_link_start() == ONEYE_LINK_OK ? ESP_OK : ESP_FAIL);

    xTaskCreate(stats_task, "t31_stats", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "已启动：等待 T31 广播（名称前缀 T31 / 服务 FFF0）…");
}
