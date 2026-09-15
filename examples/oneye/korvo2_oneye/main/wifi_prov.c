/*
 * wifi_prov.c —— Wi-Fi 配网实现（口径见 wifi_prov.h）
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "wifi_prov.h"

static const char *TAG = "wifi_prov";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_PROV_DEFAULT_TIMEOUT_MS 20000

static EventGroupHandle_t s_eg;
static bool s_inited;
static volatile bool s_connected;
static char s_ip[20];
static char s_ssid[WIFI_PROV_SSID_MAX];
static char s_source[48];
static wifi_prov_ready_cb_t s_ready_cb;

bool wifi_prov_is_connected(void)   { return s_connected; }
const char *wifi_prov_ip(void)      { return s_ip; }
const char *wifi_prov_ssid(void)    { return s_ssid; }
const char *wifi_prov_source(void)  { return s_source; }

void wifi_prov_set_ready_cb(wifi_prov_ready_cb_t cb) { s_ready_cb = cb; }

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip[0] = '\0';
        ESP_LOGW(TAG, "Wi-Fi 断开，重连…");
        (void)esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&evt->ip_info.ip, s_ip, sizeof(s_ip));
        s_connected = true;
        ESP_LOGI(TAG, "Wi-Fi 已获取 IP：%s（ssid=%s，来源=%s）", s_ip, s_ssid, s_source);
        if (s_eg) {
            xEventGroupSetBits(s_eg, WIFI_CONNECTED_BIT);
        }
        if (s_ready_cb) {
            s_ready_cb();       /* 上云启动/补启由应用侧回调处理（本模块不依赖 SDK） */
        }
    }
}

esp_err_t wifi_prov_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    if (s_eg == NULL) {
        s_eg = xEventGroupCreate();
        if (s_eg == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    (void)esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_inited = true;
    return ESP_OK;
}

/* ------------------------------------------------------------------ 凭据文件解析 */

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

static bool parse_creds(const char *path, char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return false;
    }
    char line[160];
    bool have_ssid = false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';') {
            continue;
        }
        char *eq = strchr(p, '=');
        if (eq == NULL) {
            eq = strchr(p, ':');          /* 也接受 ssid: xxx */
        }
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);
        if (strcasecmp(key, "ssid") == 0 || strcasecmp(key, "wifi_ssid") == 0) {
            snprintf(ssid, ssid_cap, "%s", val);
            have_ssid = (val[0] != '\0');
        } else if (strcasecmp(key, "password") == 0 || strcasecmp(key, "pass") == 0 ||
                   strcasecmp(key, "psk") == 0 || strcasecmp(key, "wifi_password") == 0) {
            snprintf(pass, pass_cap, "%s", val);
        }
    }
    fclose(fp);
    return have_ssid;
}

esp_err_t wifi_prov_load_file(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap,
                              char *src_path, size_t path_cap)
{
    if (ssid == NULL || pass == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid[0] = '\0';
    pass[0] = '\0';

    static const char *candidates[] = {
        "/sdcard/oneye-wifi.txt",
        "/spiffs/oneye-wifi.txt",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (parse_creds(candidates[i], ssid, ssid_cap, pass, pass_cap)) {
            if (src_path && path_cap) {
                snprintf(src_path, path_cap, "file:%s", candidates[i]);
            }
            ESP_LOGI(TAG, "从凭据文件读取 Wi-Fi：%s（ssid=%s，密码%s）",
                     candidates[i], ssid, pass[0] ? "已提供" : "为空（开放网络）");
            return ESP_OK;
        }
    }
    ESP_LOGI(TAG, "未找到 Wi-Fi 凭据文件（试过 /sdcard/oneye-wifi.txt 与 /spiffs/oneye-wifi.txt）");
    return ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ 连接 */

esp_err_t wifi_prov_connect(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t rc = wifi_prov_init();
    if (rc != ESP_OK) {
        return rc;
    }
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);

    wifi_config_t sta = { 0 };
    snprintf((char *)sta.sta.ssid, sizeof(sta.sta.ssid), "%s", ssid);
    if (pass) {
        snprintf((char *)sta.sta.password, sizeof(sta.sta.password), "%s", pass);
    }
    s_connected = false;
    s_ip[0] = '\0';
    xEventGroupClearBits(s_eg, WIFI_CONNECTED_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());       /* 已在跑则为幂等 */

    if (timeout_ms == 0) {
        timeout_ms = WIFI_PROV_DEFAULT_TIMEOUT_MS;
    }
    ESP_LOGI(TAG, "连接 Wi-Fi：ssid=%s（超时 %u ms）", ssid, (unsigned)timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Wi-Fi 连接超时（ssid=%s）：请检查凭据文件的 ssid/password", ssid);
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_prov_set_source(const char *src)
{
    snprintf(s_source, sizeof(s_source), "%s", src ? src : "");
    return ESP_OK;
}
