/*
 * prov_service.c —— Wi-Fi 配网（文件 / BLE / SmartConfig 三通道并行）
 *
 * 与两个契约面的关系：
 *   - **本地面**（contracts/local/）：BLE 通道 = oneye_dev_ble 自研 GATT；
 *     节点/组域/状态广播 = oneye_dev_link；`prov.status` 帧由本模块上报；
 *   - **非契约**：凭据文件与 Kconfig 兜底 = 台面/研发便利，量产关闭（Kconfig 默认值可改）。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"

#include "board.h"
#include "audio_hal.h"

#include "oneye_dev_types.h"
#include "oneye_dev_link.h"
#include "oneye_dev_ble.h"

#include "prov_service.h"
#include "lan_link.h"

static const char *TAG = "prov_service";

#define WIFI_BIT_CONNECTED BIT0
#define WIFI_BIT_FAIL      BIT1
#define WIFI_CONNECT_TIMEOUT_MS 20000

static EventGroupHandle_t s_wifi_eg;
static esp_netif_t *s_sta_netif;
static prov_status_t s_st;
static prov_ready_cb_t s_ready_cb;
static bool s_spiffs_mounted;
static bool s_inited;
static TaskHandle_t s_sc_timeout_task;

/* link/配网上报的投递队列（见 prov_report / link_report_task 的注释） */
typedef struct {
    oneye_dev_link_prov_state_t state;
    char ssid[PROV_SSID_MAX];
    char ip[PROV_IP_MAX];
    char src[16];
    char err[24];
} prov_report_msg_t;

static QueueHandle_t s_report_q;

/* ------------------------------------------------------------------ 凭据文件 */

/** 解析 `key=value` 凭据文件（`#` 注释；两侧空白与 CR 去除） */
static bool parse_cred_file(const char *path, char *ssid, size_t ssid_cap, char *pass,
                            size_t pass_cap)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return false;
    }
    char line[192];
    bool got_ssid = false;
    bool got_pass = false;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') {
            continue;
        }
        char *eq = strchr(p, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        /* 去尾空白与 CR/LF */
        char *end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        end = val + strlen(val);
        while (end > val && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' ||
                             end[-1] == '\t')) {
            *--end = '\0';
        }
        if (strcmp(key, "ssid") == 0) {
            snprintf(ssid, ssid_cap, "%s", val);
            got_ssid = true;
        } else if (strcmp(key, "password") == 0 || strcmp(key, "pass") == 0 ||
                   strcmp(key, "psk") == 0) {
            snprintf(pass, pass_cap, "%s", val);
            got_pass = true;
        }
    }
    fclose(fp);
    return got_ssid && got_pass;
}

static void prov_report(oneye_dev_link_prov_state_t state, const char *err)
{
    /* 一律**入队**，由 link_report_task（12 KB 栈）真正调用 SDK：
     * `oneye_dev_link_prov_report_status()` 的组帧路径栈峰值 ≈11 KB（8 KB frame + 2 KB payload），
     * 在事件任务（Wi-Fi/IP 事件）里直接调用会踩穿栈并破坏堆（真机取证 2026-09-17）。 */
    if (s_report_q == NULL) {
        return;
    }
    prov_report_msg_t m = { 0 };
    m.state = state;
    snprintf(m.ssid, sizeof(m.ssid), "%s", s_st.ssid);
    snprintf(m.ip, sizeof(m.ip), "%s", s_st.ip);
    snprintf(m.src, sizeof(m.src), "%s", s_st.source);
    snprintf(m.err, sizeof(m.err), "%s", err ? err : "");
    (void)xQueueSend(s_report_q, &m, 0); /* 满则丢弃：状态面尽力而为，不阻塞事件任务 */
}

/** 所有 link / prov.status 上报都在这条 12 KB 栈的任务里执行 */
static void link_report_task(void *arg)
{
    (void)arg;
    prov_report_msg_t m;
    while (1) {
        if (xQueueReceive(s_report_q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        (void)oneye_dev_link_prov_report_status(m.state, m.ssid[0] ? m.ssid : NULL,
                                               m.ip[0] ? m.ip : NULL,
                                               m.src[0] ? m.src : NULL,
                                               m.err[0] ? m.err : NULL);
    }
}

/* ------------------------------------------------------------------ Wi-Fi 事件 */

static void on_got_ip(void)
{
    s_st.connected = true;
    s_st.last_err = ESP_OK;
    if (s_sta_netif) {
        esp_netif_ip_info_t ip = { 0 };
        if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
            snprintf(s_st.ip, sizeof(s_st.ip), IPSTR, IP2STR(&ip.ip));
        }
    }
    ESP_LOGI(TAG, "已联网：ssid=%s ip=%s source=%s", s_st.ssid, s_st.ip, s_st.source);

    /* 契约 §4：一旦联网成功 → 立即停 SmartConfig、停 BLE 广告 */
#if CONFIG_ONEYE_LLM_ENABLE_SMARTCONFIG
    if (s_st.smartconfig) {
        (void)esp_smartconfig_stop();
        s_st.smartconfig = false;
    }
#endif
#if CONFIG_ONEYE_LLM_ENABLE_BLE_PROV
    if (s_st.ble_active) {
        (void)oneye_dev_ble_advertise(false);
        s_st.ble_active = false;
    }
#endif
    (void)oneye_dev_link_set_chan_up(ONEYE_DEV_LINK_CHAN_LAN, true);
    prov_report(ONEYE_DEV_LINK_PROV_CONNECTED, NULL);
    if (s_ready_cb) {
        s_ready_cb();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_st.connected = false;
        if (!(xEventGroupGetBits(s_wifi_eg) & WIFI_BIT_CONNECTED)) {
            esp_wifi_connect(); /* 首次连上之前持续重试（超时由 prov_service_connect 兜底） */
        } else {
            ESP_LOGW(TAG, "Wi-Fi 断开（等待自动重连）");
            prov_report(ONEYE_DEV_LINK_PROV_FAILED, ONEYE_DEV_LINK_ERR_DHCP_TIMEOUT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_eg, WIFI_BIT_CONNECTED);
        on_got_ip();
    }
}

#if CONFIG_ONEYE_LLM_ENABLE_SMARTCONFIG
static void sc_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != SC_EVENT) {
        return;
    }
    if (id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "SmartConfig：发现信道完成");
    } else if (id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "SmartConfig：已锁定信道");
    } else if (id == SC_EVENT_GOT_SSID_PSWD) {
        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)data;
        char ssid[PROV_SSID_MAX] = { 0 };
        char pass[65] = { 0 };
        memcpy(ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(pass, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SmartConfig：收到凭据（ssid=%s，密码不打印）", ssid);
        snprintf(s_st.source, sizeof(s_st.source), "smartconfig");
        (void)esp_smartconfig_stop();
        s_st.smartconfig = false;
        /* 凭据下发后立刻连接（阻塞在事件任务上不可取 → 交给独立任务） */
        if (prov_service_connect(ssid, pass, WIFI_CONNECT_TIMEOUT_MS) != ESP_OK) {
            prov_report(ONEYE_DEV_LINK_PROV_FAILED, ONEYE_DEV_LINK_ERR_AUTH_FAILED);
        }
    } else if (id == SC_EVENT_SEND_ACK_DONE) {
        (void)esp_smartconfig_stop();
        s_st.smartconfig = false;
    }
}

static void sc_timeout_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(CONFIG_ONEYE_LLM_SMARTCONFIG_TIMEOUT_S * 1000));
    if (s_st.smartconfig && !s_st.connected) {
        (void)esp_smartconfig_stop();
        s_st.smartconfig = false;
        ESP_LOGW(TAG, "SmartConfig 超时（%d s）已停止；BLE 通道保持开启",
                 CONFIG_ONEYE_LLM_SMARTCONFIG_TIMEOUT_S);
        prov_report(ONEYE_DEV_LINK_PROV_FAILED, ONEYE_DEV_LINK_ERR_DHCP_TIMEOUT);
    }
    s_sc_timeout_task = NULL;
    vTaskDelete(NULL);
}
#endif

/* ------------------------------------------------------------------ BLE 配网处理器（应用侧 Wi-Fi 实现） */

#if CONFIG_ONEYE_LLM_ENABLE_BLE_PROV
static int ble_prov_scan(char *json_out, size_t cap, void *ctx)
{
    (void)ctx;
    return prov_service_scan_json(json_out, cap) == ESP_OK ? 0 : -1;
}

static int ble_prov_connect(const char *ssid, const char *password, const char *token, void *ctx)
{
    (void)token;
    (void)ctx;
    snprintf(s_st.source, sizeof(s_st.source), "ble");
    ESP_LOGI(TAG, "BLE 通道下发凭据（ssid=%s；密码不打印）", ssid ? ssid : "");
    return prov_service_connect(ssid, password, WIFI_CONNECT_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

static int ble_prov_reset(void *ctx)
{
    (void)ctx;
    return prov_service_forget() == ESP_OK ? 0 : -1;
}
#endif

/* ------------------------------------------------------------------ 初始化 */

esp_err_t prov_service_init(esp_periph_set_handle_t periph_set, prov_ready_cb_t on_ready)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_ready_cb = on_ready;

    /* 上报队列 + 12 KB 栈的上报任务：**必须先于任何 prov_report()** 建立 */
    if (s_report_q == NULL) {
        s_report_q = xQueueCreate(4, sizeof(prov_report_msg_t));
        if (s_report_q == NULL) {
            ESP_LOGE(TAG, "上报队列创建失败");
            return ESP_ERR_NO_MEM;
        }
        if (xTaskCreate(link_report_task, "link_report", 12288, NULL, 4, NULL) != pdPASS) {
            vQueueDelete(s_report_q);
            s_report_q = NULL;
            ESP_LOGE(TAG, "上报任务创建失败");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init 失败：%s", esp_err_to_name(err));
        return err;
    }

    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
#if CONFIG_ONEYE_LLM_ENABLE_SMARTCONFIG
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, sc_event_handler, NULL));
#endif
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* SPIFFS（storage 分区）：凭据文件兜底存储 */
#if CONFIG_ONEYE_LLM_ENABLE_WIFI_FILE
    esp_vfs_spiffs_conf_t sp = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    if (esp_vfs_spiffs_register(&sp) == ESP_OK) {
        s_spiffs_mounted = true;
    } else {
        ESP_LOGW(TAG, "SPIFFS(storage) 挂载失败：只用 SD 卡上的凭据文件");
    }
#endif

    /* SD 卡（可选；仅用于凭据文件） */
#if CONFIG_ONEYE_LLM_ENABLE_SDCARD
    if (periph_set) {
        if (audio_board_sdcard_init(periph_set, SD_MODE_1_LINE) == ESP_OK) {
            ESP_LOGI(TAG, "microSD 已挂载（凭据文件路径 /sdcard/oneye-wifi.txt）");
        } else {
            ESP_LOGW(TAG, "microSD 未挂载（不影响启动；用 SPIFFS/Kconfig 兜底）");
        }
    }
#endif

    /* ---------------- 本地面：link（节点/组域/信道） ---------------- */
    oneye_dev_link_config_t lcfg;
    oneye_dev_link_config_defaults(&lcfg);
    lcfg.node_id = CONFIG_ONEYE_LLM_NODE_ID;
    /* 本工程不启用 wan 信道（语音面走独立的 voice WS；link wan 需服务端 /v1/link/ws 支持） */
    lcfg.wan_uri = NULL;
    static const oneye_dev_link_chan_t prefer[2] = { ONEYE_DEV_LINK_CHAN_BLE,
                                                     ONEYE_DEV_LINK_CHAN_LAN };
    lcfg.prefer_chan = prefer;
    lcfg.prefer_chan_count = 2;
    /* 局域网为明文信道：仅台面/研发（Kconfig 可关），与契约红线一致 */
    lcfg.allow_plaintext_chan = true;
    err = oneye_dev_link_init(&lcfg);
    if (err != ONEYE_DEV_SDK_OK) {
        ESP_LOGW(TAG, "oneye_dev_link_init 失败：%d（本地面功能不可用）", (int)err);
    } else {
        (void)oneye_dev_link_prov_set_provider(ble_prov_scan, ble_prov_connect, ble_prov_reset, NULL);
        (void)oneye_dev_link_start();

        /* 本机节点 + 默认组域（演示 link 的节点/组抽象；组只含本机，配网后加入手机节点） */
        oneye_dev_link_node_t self = { 0 };
        snprintf(self.node_id, sizeof(self.node_id), "%s", CONFIG_ONEYE_LLM_NODE_ID);
        self.kind = ONEYE_DEV_LINK_NODE_SELF;
        snprintf(self.name, sizeof(self.name), "Korvo-2 LLM Chat");
        self.chan_mask = (1u << ONEYE_DEV_LINK_CHAN_BLE) | (1u << ONEYE_DEV_LINK_CHAN_LAN);
        snprintf(self.caps, sizeof(self.caps), "prov.ble,prov.smartconfig,link.lan,voice.ws");
        (void)oneye_dev_link_node_upsert(&self);

        oneye_dev_link_group_t grp = { 0 };
        snprintf(grp.group_id, sizeof(grp.group_id), "%s-home", CONFIG_ONEYE_LLM_NODE_ID);
        snprintf(grp.name, sizeof(grp.name), "Home");
        snprintf(grp.owner, sizeof(grp.owner), "%s", CONFIG_ONEYE_LLM_NODE_ID);
        grp.chan_mask = (1u << ONEYE_DEV_LINK_CHAN_BLE);
        if (oneye_dev_link_group_upsert(&grp) == ONEYE_DEV_SDK_OK) {
            (void)oneye_dev_link_group_add_member(grp.group_id, self.node_id);
        }
    }

    /* ---------------- BLE 配网（自研 GATT） ---------------- */
#if CONFIG_ONEYE_LLM_ENABLE_BLE_PROV
    oneye_dev_ble_config_t bcfg;
    oneye_dev_ble_config_defaults(&bcfg);
    bcfg.device_id = CONFIG_ONEYE_LLM_NODE_ID;
    bcfg.auto_advertise = false; /* 未配网时由 start() 流程显式开广告 */
    bcfg.prov.scan = ble_prov_scan;
    bcfg.prov.connect = ble_prov_connect;
    bcfg.prov.reset = ble_prov_reset;
    err = oneye_dev_ble_init(&bcfg);
    if (err != ONEYE_DEV_SDK_OK) {
        ESP_LOGW(TAG, "oneye_dev_ble_init 失败：%d（BLE 配网不可用）", (int)err);
    } else {
        if (CONFIG_ONEYE_LLM_PROV_POP[0] != '\0') {
            (void)oneye_dev_ble_set_pop(CONFIG_ONEYE_LLM_PROV_POP);
        }
        (void)oneye_dev_ble_start();
        (void)oneye_dev_ble_bind_to_link(); /* 把 BLE 注册为 link 的 ble 信道发送器 */
    }
#endif

    /* ---------------- 局域网链路（UDP 发现 + HTTP 帧面） ---------------- */
#if CONFIG_ONEYE_LLM_ENABLE_LAN_LINK
    lan_link_config_t llcfg = {
        .node_id = CONFIG_ONEYE_LLM_NODE_ID,
        .name = "Korvo-2 LLM Chat",
        .http_port = CONFIG_ONEYE_LLM_LAN_HTTP_PORT,
        .caps = "prov.ble,prov.smartconfig,link.lan,voice.ws",
        .channels = "ble,lan",
    };
    if (lan_link_init(&llcfg) != ESP_OK) {
        ESP_LOGW(TAG, "局域网链路启动失败（仅影响 lan 信道）");
    }
#endif

    s_inited = true;
    s_st.last_err = ESP_OK;
    return ESP_OK;
}

/* ------------------------------------------------------------------ 连接与配网 */

esp_err_t prov_service_connect(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0) {
        timeout_ms = WIFI_CONNECT_TIMEOUT_MS;
    }
    s_st.attempts++;
    xEventGroupClearBits(s_wifi_eg, WIFI_BIT_CONNECTED | WIFI_BIT_FAIL);
    prov_report(ONEYE_DEV_LINK_PROV_CONNECTING, NULL);

    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass ? pass : "");
    wc.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        s_st.last_err = err;
        return err;
    }
    snprintf(s_st.ssid, sizeof(s_st.ssid), "%s", ssid);
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) { /* 已启动时忽略 */
        s_st.last_err = err;
        return err;
    }
    esp_wifi_disconnect();
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_st.last_err = err;
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_BIT_CONNECTED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_BIT_CONNECTED) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "连接 %s 超时/失败（%u ms）——检查密码或信号", ssid, (unsigned)timeout_ms);
    s_st.last_err = ESP_ERR_TIMEOUT;
    prov_report(ONEYE_DEV_LINK_PROV_FAILED, ONEYE_DEV_LINK_ERR_DHCP_TIMEOUT);
    return ESP_ERR_TIMEOUT;
}

esp_err_t prov_service_start(void)
{
    char ssid[PROV_SSID_MAX] = { 0 };
    char pass[65] = { 0 };

    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_ONEYE_LLM_ENABLE_WIFI_FILE
    const char *paths[] = { "/sdcard/oneye-wifi.txt", "/spiffs/oneye-wifi.txt" };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (parse_cred_file(paths[i], ssid, sizeof(ssid), pass, sizeof(pass))) {
            snprintf(s_st.source, sizeof(s_st.source), "file");
            ESP_LOGI(TAG, "凭据文件命中：%s（ssid=%s）", paths[i], ssid);
            return prov_service_connect(ssid, pass, WIFI_CONNECT_TIMEOUT_MS);
        }
    }
#endif

    if (CONFIG_ONEYE_LLM_WIFI_SSID[0] != '\0') {
        snprintf(s_st.source, sizeof(s_st.source), "kconfig");
        ESP_LOGI(TAG, "使用 Kconfig 兜底凭据（ssid=%s）", CONFIG_ONEYE_LLM_WIFI_SSID);
        return prov_service_connect(CONFIG_ONEYE_LLM_WIFI_SSID, CONFIG_ONEYE_LLM_WIFI_PASSWORD,
                                    WIFI_CONNECT_TIMEOUT_MS);
    }

    /* 无凭据 → 开启配网通道（BLE + SmartConfig 并行）；Wi-Fi 需先 start 供扫描用 */
    (void)esp_wifi_start();
    prov_report(ONEYE_DEV_LINK_PROV_PAIRING, NULL);
    ESP_LOGI(TAG, "未配网：等待手机经 BLE（POP 配对）或 SmartConfig 下发凭据");

#if CONFIG_ONEYE_LLM_ENABLE_BLE_PROV
    if (oneye_dev_ble_is_initialized()) {
        if (oneye_dev_ble_advertise(true) == ONEYE_DEV_SDK_OK) {
            s_st.ble_active = true;
            char pop[ONEYE_DEV_BLE_POP_LEN] = { 0 };
            if (oneye_dev_ble_get_pop(pop, sizeof(pop)) == ONEYE_DEV_SDK_OK) {
                ESP_LOGI(TAG, "BLE 已开始广播（配对 POP：%s，TTL 300 s）", pop);
            }
        }
    }
#endif

#if CONFIG_ONEYE_LLM_ENABLE_SMARTCONFIG
    esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_V2);
    smartconfig_start_config_t sc = SMARTCONFIG_START_CONFIG_DEFAULT();
    sc.esp_touch_v2_enable_crypt = false; /* 台面：不启用 AES（量产应为 true 并注入密钥） */
    if (esp_smartconfig_start(&sc) == ESP_OK) {
        s_st.smartconfig = true;
        if (s_sc_timeout_task == NULL) {
            (void)xTaskCreate(sc_timeout_task, "sc_timeout", 3072, NULL, 4, &s_sc_timeout_task);
        }
    } else {
        ESP_LOGW(TAG, "SmartConfig 启动失败（BLE 通道不受影响）");
    }
#endif
    return ESP_OK;
}

esp_err_t prov_service_scan_json(char *out, size_t cap)
{
    if (out == NULL || cap < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_scan_config_t sc = { 0 };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        return err;
    }
    uint16_t num = 12;
    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t gerr = esp_wifi_scan_get_ap_records(&num, recs);
    if (gerr != ESP_OK) {
        free(recs);
        return gerr;
    }
    size_t off = 0;
    off += snprintf(out + off, cap - off, "[");
    for (uint16_t i = 0; i < num && off < cap - 64; i++) {
        const char *auth = (recs[i].authmode == WIFI_AUTH_OPEN) ? "open" : "wpa2";
        off += snprintf(out + off, cap - off, "%s{\"ssid\":\"%.32s\",\"rssi\":%d,\"auth\":\"%s\"}",
                        i ? "," : "", (const char *)recs[i].ssid, recs[i].rssi, auth);
    }
    snprintf(out + off, cap - off, "]");
    free(recs);
    ESP_LOGI(TAG, "AP 扫描完成：%u 个（仅上报 SSID/RSSI/加密类型）", (unsigned)num);
    return ESP_OK;
}

esp_err_t prov_service_forget(void)
{
    ESP_LOGW(TAG, "清除凭据并回到未配网态");
    esp_wifi_disconnect();
    wifi_config_t wc = { 0 };
    (void)esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_st.connected = false;
    s_st.ssid[0] = '\0';
    s_st.ip[0] = '\0';
    s_st.source[0] = '\0';
#if CONFIG_ONEYE_LLM_ENABLE_BLE_PROV
    if (oneye_dev_ble_is_initialized()) {
        (void)oneye_dev_ble_unpair();
        (void)oneye_dev_ble_advertise(true);
        s_st.ble_active = true;
    }
#endif
    prov_report(ONEYE_DEV_LINK_PROV_IDLE, NULL);
    return ESP_OK;
}

bool prov_service_is_connected(void)
{
    return s_st.connected;
}

const char *prov_service_ip(void)
{
    return s_st.ip;
}

void prov_service_get_status(prov_status_t *out)
{
    if (out) {
        *out = s_st;
    }
}
