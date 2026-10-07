/*
 * wifi_prov.c —— Wi-Fi 配网实现（口径见 wifi_prov.h）
 *
 * ★ 本模块是「入网后收敛点」的**唯一持有者**（ADR-0017 D2）★
 *   所有配网通道（C1/C2 BLE、C3 凭据文件、C4 台面 API、C5 Kconfig）都只是**把凭据交进来**；
 *   「入网成功 ⇒ 触发上云」这件事**只**发生在下面 `on_wifi_event()` 的 GOT_IP 分支里
 *   （→ `s_ready_cb()` → `korvo2_oneye_main.c:on_wifi_ready()` → `oneye_start()`）。
 *   ⛔ 新增通道时不要在本文件之外复制这段逻辑；也不要在这里按通道分叉「入网后做什么」——
 *      下游（面板/授时/net_probe/影子/埋点）对**所有**通道是同一条路径，这正是本设计的价值。
 *      ★ 2026-10-07：`net_probe` 与 `sntp` 原先只挂在 C3/C5 的启动路径上（BLE 通道会跳过），
 *      现已收进该下游；门禁 `tools/check-prov-convergence.py` 的 A11/A12 机械断言这件事。
 *
 * ★ 本模块同时是**优先级判据的唯一落码点**（ADR-0017 D2/D4）★
 *   优先级 = **SD 卡 WiFi 凭据 > 蓝牙配网 > 其他（声波/二维码等预留）**，语义是
 *   「同一台设备上多通道都可得时**谁先被采信**」，**不是**互斥开关。判据本体在
 *   `wifi_prov_policy.h`（纯逻辑，与主机侧单测 `tools/test-prov-priority.c` 编译**同一份源**），
 *   本文件只负责 ① 保存采信槽（`s_held_*`）② 按判据决定采信/拒绝 ③ **采信的**才交给连接层。
 *   ⛔ 连接层 `wifi_prov_connect()` 已收为 `static`：**模块外没有任何路径能绕过判据发起入网**。
 *   ⛔ 已入网（拿到 IP）之后一律拒绝 —— **只入网一次**；显式改配 / `prov.reset` 走
 *      `wifi_prov_release_held()` 这条**显式**路径（调用方自己声明"这是重新配网"）。
 *   所有拒绝/抢占都在串口留痕（统一前缀 `[prov-priority]`），不是静默丢弃。
 */

#include <stdio.h>
#include <stdlib.h>
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
/* 扫描态：为 C1/C2 的 AP 扫描把 Wi-Fi 起起来，但**不**自动重连（否则无凭据时空转刷屏）。
 * 一旦某通道提交了凭据（wifi_prov_submit_credentials*），本标志清零。 */
static volatile bool s_scan_only;
static char s_ip[20];
static char s_ssid[WIFI_PROV_SSID_MAX];
static char s_source[48];
static wifi_prov_ready_cb_t s_ready_cb;

/* ★ 采信槽（ADR-0017 D2/D4）：**当前被采信的那一份凭据来自哪条通道**。
 * 同一时刻只有一份被采信；`wifi_prov_policy_decide()` 用它做优先级比较。
 * 只有三处能改它：submit（采信）、release_held（显式释放）、forget（回到未配网态）。 */
static bool s_held_valid;
static wifi_prov_channel_t s_held_chan = WIFI_PROV_CHAN_UNKNOWN;
/* 拒绝计数（留痕可量化：串口日志之外，还能在调试时一眼看出"是不是一直被拒"）。 */
static unsigned s_reject_lower;
static unsigned s_reject_already_up;

bool wifi_prov_is_connected(void)   { return s_connected; }
const char *wifi_prov_ip(void)      { return s_ip; }
const char *wifi_prov_ssid(void)    { return s_ssid; }
const char *wifi_prov_source(void)  { return s_source; }

const char *wifi_prov_channel_source(wifi_prov_channel_t ch)
{
    switch (ch) {
    case WIFI_PROV_CHAN_FILE:    return "file";
    case WIFI_PROV_CHAN_BLE:     return "ble";
    case WIFI_PROV_CHAN_API:     return "api";
    case WIFI_PROV_CHAN_KCONFIG: return "kconfig";
    /* 预留通道**没有**实现：走到这里说明有人只登记了枚举、没登记来源字符串（见 wifi_prov.h
     * 的「新增通道步骤 ①」）。返回 "unknown" 而不是编一个像样的名字，免得日志看起来像已实现。 */
    case WIFI_PROV_CHAN_SONIC:
    case WIFI_PROV_CHAN_QRCODE:
    case WIFI_PROV_CHAN_UNKNOWN:
    default:                     return "unknown";
    }
}

void wifi_prov_set_ready_cb(wifi_prov_ready_cb_t cb) { s_ready_cb = cb; }

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_scan_only) {
            (void)esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip[0] = '\0';
        if (s_scan_only) {
            return;                 /* 扫描态：不自动重连（见 s_scan_only 注释） */
        }
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
        /* ★★ 收敛点：无论凭据来自哪条通道，都在**这一处**触发上云链路（ADR-0017 D2）★★
         * 通道侧不需要、也不允许知道"入网之后要做什么"。 */
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

/* ------------------------------------------------------------------ AP 扫描（供 BLE 通道的 prov.scan.req） */

esp_err_t wifi_prov_prepare_scan(void)
{
    esp_err_t rc = wifi_prov_init();
    if (rc != ESP_OK) {
        return rc;
    }
    /* 先置扫描态再 start：STA_START / DISCONNECTED 两个事件都不会去 connect（见 on_wifi_event）。
     * 顺序不能反 —— esp_wifi_start() 会同步派发 STA_START。 */
    s_scan_only = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    return esp_wifi_start();       /* 已在跑则为幂等 */
}

esp_err_t wifi_prov_scan_json(char *out, size_t cap)
{
    if (out == NULL || cap < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    esp_err_t rc = wifi_prov_prepare_scan();
    if (rc != ESP_OK) {
        return rc;
    }
    wifi_scan_config_t sc = { 0 };
    rc = esp_wifi_scan_start(&sc, true);
    if (rc != ESP_OK && rc != ESP_ERR_WIFI_STATE) {
        return rc;
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

/* ------------------------------------------------------------------ 连接（**模块内部**） */

/* ★ 本函数是 `static`，且**故意**不进 `wifi_prov.h`（ADR-0017 D2/D4）：
 * 连接层是"绕过优先级判据的唯一下口子"，收成内部函数之后，模块外**任何**通道都只能经
 * `wifi_prov_submit_credentials*()` 入网 ⇒ 判据不可能被旁路。门禁 A8 会断言这一点。 */
static esp_err_t wifi_prov_connect(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t rc = wifi_prov_init();
    if (rc != ESP_OK) {
        return rc;
    }
    s_scan_only = false;            /* 有凭据了 ⇒ 退出扫描态，恢复正常的连接/重连语义 */
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

/* ------------------------------------------------- 通道唯一入网入口（含优先级仲裁） */

/**
 * ★ 所有配网通道的**唯一入网入口**（ADR-0017 D2/D4）★
 *
 * ① 过**唯一判据** `wifi_prov_policy_decide()`（本体在 `wifi_prov_policy.h`，与主机侧单测同源）；
 * ② 采信 ⇒ 记来源 + 交给 **static** 的 `wifi_prov_connect()`（由它触发 GOT_IP → s_ready_cb 收敛点）；
 * ③ 拒绝 ⇒ 留痕（`[prov-priority]`）+ 返回 `ESP_ERR_INVALID_STATE`，**不发起入网**。
 * ⛔ 这里**不**做任何「入网后」动作 —— 那是收敛点下游（on_wifi_ready → oneye_start）的事。
 */
esp_err_t wifi_prov_submit_credentials_src(const char *ssid, const char *pass,
                                           wifi_prov_channel_t ch, const char *src_override)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const char *src = (src_override != NULL && src_override[0] != '\0')
                          ? src_override : wifi_prov_channel_source(ch);

    /* ★ 唯一判据 ★ —— 决策 token 与主机侧单测用的是同一个 `wifi_prov_decision_str()`。
     * `onboarded` = 「已用**当前被采信**的那份凭据入网」（`s_connected && s_held_valid`）：
     * 采信槽被**显式释放**（台面改配 / prov.reset）后即视为"可重新配网"，
     * ⛔ 但上云链路仍只启动一次（唯一调用点在收敛点下游，见门禁 A1/A9）。 */
    const wifi_prov_decision_t d =
        wifi_prov_policy_decide(s_held_valid, s_held_chan, ch, s_connected && s_held_valid);

    if (!wifi_prov_decision_is_accept(d)) {
        if (d == WIFI_PROV_DECISION_REJECT_LOWER) {
            s_reject_lower++;
        } else {
            s_reject_already_up++;
        }
        ESP_LOGW(TAG, "[prov-priority] %s（%s）：channel=%s(rank %d)，已采信=%s(rank %d)，已入网=%s"
                      "（ssid=%s）⇒ 不采信、不发起入网（累计 lower=%u already_up=%u）",
                 wifi_prov_decision_str(d),
                 (d == WIFI_PROV_DECISION_REJECT_LOWER)
                     ? "低优先级不采信"
                     : "已入网且采信槽未被释放（只入网一次；要换网请先 prov.reset 或走台面改配）",
                 src, wifi_prov_channel_rank(ch),
                 wifi_prov_channel_source(s_held_chan), wifi_prov_channel_rank(s_held_chan),
                 s_connected ? "是" : "否", ssid, s_reject_lower, s_reject_already_up);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "[prov-priority] %s：channel=%s(rank %d)（ssid=%s，密码%s）",
             wifi_prov_decision_str(d), src, wifi_prov_channel_rank(ch),
             ssid, (pass && pass[0]) ? "已提供" : "为空（开放网络）");
    s_held_valid = true;
    s_held_chan = ch;
    (void)wifi_prov_set_source(src);
    return wifi_prov_connect(ssid, pass, 0);
}

esp_err_t wifi_prov_submit_credentials(const char *ssid, const char *pass, wifi_prov_channel_t ch)
{
    return wifi_prov_submit_credentials_src(ssid, pass, ch, NULL);
}

void wifi_prov_release_held(const char *reason)
{
    if (!s_held_valid) {
        ESP_LOGI(TAG, "[prov-priority] 释放采信槽：本来就是空的（reason=%s）", reason ? reason : "-");
        return;
    }
    ESP_LOGW(TAG, "[prov-priority] 释放采信槽：原=%s（reason=%s）"
                  "⇒ 之后任一通道的提交都按「首次有效凭据」重新裁决",
             wifi_prov_channel_source(s_held_chan), reason ? reason : "-");
    s_held_valid = false;
    s_held_chan = WIFI_PROV_CHAN_UNKNOWN;
}

/**
 * 清除当前凭据并回到未配网态（通道的 `prov.reset` 用）。
 * ⚠️ 本函数**不清 NVS/flash**（那会与「凭据分区是身份来源」的另一条线打架），
 *    只断开连接并清掉 RAM 里的 STA 配置 ⇒ 复位后 Kconfig/凭据文件仍会重新生效。
 * ★ 同时清空**采信槽**（ADR-0017 D4）：`prov.reset` 是操作者的显式动作，
 *   释放后任一通道都可以重新被采信（不是"只入网一次"的例外，而是明确回到未配网态）。
 */
void wifi_prov_forget(void)
{
    s_held_valid = false;
    s_held_chan = WIFI_PROV_CHAN_UNKNOWN;
    if (!s_inited) {
        return;
    }
    esp_wifi_disconnect();
    wifi_config_t sta = { 0 };
    (void)esp_wifi_set_config(WIFI_IF_STA, &sta);
    s_connected = false;
    s_ip[0] = '\0';
    s_ssid[0] = '\0';
    s_source[0] = '\0';
    ESP_LOGW(TAG, "已清除当前 Wi-Fi 凭据（仅 RAM）⇒ 回到未配网态，可经任一通道重新配网");
}
