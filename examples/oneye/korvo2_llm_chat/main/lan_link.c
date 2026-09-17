/*
 * lan_link.c —— 局域网信道数据面实现（UDP 57321 发现 + POST /api/link/frame）
 *
 * 分工（见 oneye_dev_link.h 头注）：数据面（socket/httpd）在本模块，帧语义/节点/选路在 SDK。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "esp_http_server.h"

#include "oneye_dev_link.h"
#include "lan_link.h"

static const char *TAG = "lan_link";

#define LAN_DISCOVER_PORT    57321
#define LAN_FRAME_MAX        8192u  /* 契约 §2：单帧 ≤8 KB（与 ONEYE_DEV_LINK_FRAME_MAX_BYTES 一致） */
#define LAN_REPLY_MAX        4096u

static lan_link_config_t s_cfg;
static httpd_handle_t    s_httpd;
static TaskHandle_t      s_udp_task;
static volatile bool     s_run;
static char              s_token[64];
static char              s_reply[LAN_REPLY_MAX]; /* 待发帧（响应携带） */
static size_t            s_reply_len;
static uint32_t          s_rx_frames;
static uint32_t          s_announces;

static void set_reply(const char *json, size_t len)
{
    if (json == NULL || len == 0 || len >= LAN_REPLY_MAX) {
        return;
    }
    memcpy(s_reply, json, len);
    s_reply[len] = '\0';
    s_reply_len = len;
}

/** 组一条 link 帧 JSON（信封：{v,t,id,from,to,ch,ts,p}，见 contracts/local/README.md §2.1） */
static int build_frame(char *out, size_t cap, const char *to, const char *t, const char *p_json)
{
    return snprintf(out, cap,
                    "{\"v\":1,\"t\":\"%s\",\"id\":\"%s-%u\",\"from\":\"%s\",\"to\":\"%s\","
                    "\"ch\":\"lan\",\"ts\":%lld,\"p\":%s}",
                    t, s_cfg.node_id ? s_cfg.node_id : "oneye-node",
                    (unsigned)(esp_timer_get_time() / 1000),
                    s_cfg.node_id ? s_cfg.node_id : "oneye-node", to ? to : "*",
                    (long long)(esp_timer_get_time() / 1000), p_json ? p_json : "{}");
}

/* ------------------------------------------------------------------ UDP 发现应答 */

static void udp_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "UDP socket 创建失败");
        s_udp_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(LAN_DISCOVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "UDP %d 绑定失败", LAN_DISCOVER_PORT);
        close(sock);
        s_udp_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "UDP 发现已就绪（端口 %d，应答 oneye.discover）", LAN_DISCOVER_PORT);

    char rx[256];
    while (s_run) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int n = recvfrom(sock, rx, sizeof(rx) - 1, 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;
        }
        rx[n] = '\0';
        if (strstr(rx, "oneye.discover") == NULL) {
            continue; /* 非本协议报文：忽略（避免噪声） */
        }
        char p[384];
        snprintf(p, sizeof(p),
                 "{\"node_id\":\"%s\",\"kind\":\"device\",\"name\":\"%s\",\"channels\":\"%s\","
                 "\"caps\":\"%s\",\"http_port\":%u}",
                 s_cfg.node_id ? s_cfg.node_id : "oneye-node", s_cfg.name ? s_cfg.name : "oneye",
                 s_cfg.channels ? s_cfg.channels : "lan", s_cfg.caps ? s_cfg.caps : "",
                 (unsigned)s_cfg.http_port);
        char frame[640];
        build_frame(frame, sizeof(frame), "*", "node.announce", p);
        /* 单播回请求方（契约：设备不做主动广播） */
        (void)sendto(sock, frame, strlen(frame), 0, (struct sockaddr *)&from, from_len);
        s_announces++;
        ESP_LOGI(TAG, "已应答发现请求（第 %u 次）", (unsigned)s_announces);
    }
    close(sock);
    s_udp_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ HTTP 帧面 */

/** 白名单：无配对令牌时允许的帧类型（契约 lan-link.md §2） */
static bool frame_allowed_without_token(const char *body)
{
    static const char *allow[] = { "prov.hello", "link.ping", "node.announce" };
    for (size_t i = 0; i < sizeof(allow) / sizeof(allow[0]); i++) {
        char needle[40];
        snprintf(needle, sizeof(needle), "\"t\":\"%s\"", allow[i]);
        if (strstr(body, needle) != NULL) {
            return true;
        }
    }
    return false;
}

static esp_err_t frame_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || (size_t)req->content_len > LAN_FRAME_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "frame size must be 1..8192 bytes");
        return ESP_FAIL;
    }
    char *body = malloc((size_t)req->content_len + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, (size_t)req->content_len - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    if (s_token[0] != '\0') {
        char hdr[80] = { 0 };
        if (httpd_req_get_hdr_value_str(req, "X-Oneye-Token", hdr, sizeof(hdr)) != ESP_OK ||
            strcmp(hdr, s_token) != 0) {
            ESP_LOGW(TAG, "令牌不匹配，拒绝帧");
            free(body);
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"v\":1,\"t\":\"error\",\"p\":{\"code\":\"unauthorized\","
                                    "\"msg\":\"token required\"}}");
            return ESP_OK;
        }
    } else if (!frame_allowed_without_token(body)) {
        ESP_LOGW(TAG, "未配对（无令牌）：该帧类型不在白名单内");
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"v\":1,\"t\":\"error\",\"p\":{\"code\":\"unauthorized\","
                                "\"msg\":\"pair first (BLE) to get token\"}}");
        return ESP_OK;
    }

    esp_err_t err = oneye_dev_link_inject_frame(body, (size_t)received, ONEYE_DEV_LINK_CHAN_LAN);
    ESP_LOGI(TAG, "收到 lan 帧（%d B）→ inject=%d", received, (int)err);
    free(body);
    s_rx_frames++;

    httpd_resp_set_type(req, "application/json");
    if (s_reply_len > 0) {
        (void)httpd_resp_send(req, s_reply, (ssize_t)s_reply_len);
        s_reply_len = 0;
        s_reply[0] = '\0';
    } else {
        (void)httpd_resp_sendstr(req, "{}");
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ link 的 lan 发送器 */

static int lan_send_fn(const char *to, const void *data, size_t len, void *ctx)
{
    (void)to;
    (void)ctx;
    /* 契约未定义设备→手机的异步推送；本实现暂存为「下一次 HTTP 响应体」（见头注） */
    set_reply((const char *)data, len);
    return 0;
}

/* ------------------------------------------------------------------ 生命周期 */

esp_err_t lan_link_init(const lan_link_config_t *cfg)
{
    if (cfg == NULL || cfg->node_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    s_run = true;

    /* 1) 注册 lan 出站发送器（须先于任何发送动作） */
    oneye_dev_sdk_err_t lerr = oneye_dev_link_register_sender(ONEYE_DEV_LINK_CHAN_LAN, lan_send_fn, NULL);
    if (lerr != ONEYE_DEV_SDK_OK) {
        ESP_LOGW(TAG, "注册 lan 发送器失败：%d", (int)lerr);
    }
    (void)oneye_dev_link_set_chan_up(ONEYE_DEV_LINK_CHAN_LAN, true);

    /* 2) UDP 发现 */
    if (xTaskCreate(udp_task, "lan_disc", 4096, NULL, 4, &s_udp_task) != pdPASS) {
        ESP_LOGE(TAG, "UDP 发现任务创建失败");
        return ESP_FAIL;
    }

    /* 3) HTTP 帧面 */
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port = cfg->http_port;
    hcfg.max_uri_handlers = 4;
    /* 12 KB：本处理函数会经 `oneye_dev_link_inject_frame()` 进入 SDK 的组帧/发送路径，
     * 其栈峰值 ≈11 KB（8 KB frame + 2 KB payload）；缺省 4 KB 会踩穿栈（真机取证 2026-09-17）。 */
    hcfg.stack_size = 12288;
    hcfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &hcfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败（端口 %u）", (unsigned)cfg->http_port);
        return ESP_FAIL;
    }
    httpd_uri_t uri_frame = {
        .uri = "/api/link/frame",
        .method = HTTP_POST,
        .handler = frame_post_handler,
        .user_ctx = NULL,
    };
    if (httpd_register_uri_handler(s_httpd, &uri_frame) != ESP_OK) {
        ESP_LOGE(TAG, "注册 /api/link/frame 失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "HTTP 帧面已就绪：POST http://<ip>:%u/api/link/frame", (unsigned)cfg->http_port);
    return ESP_OK;
}

void lan_link_set_token(const char *token)
{
    if (token == NULL) {
        s_token[0] = '\0';
        return;
    }
    snprintf(s_token, sizeof(s_token), "%s", token);
    ESP_LOGI(TAG, "已设置配对令牌（不打印内容）");
}

bool lan_link_is_active(void)
{
    return s_httpd != NULL;
}
