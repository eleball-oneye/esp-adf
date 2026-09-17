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

/* ------------------------------------------------- 契约 §2：限流与幂等（帧面） */

/*
 * 契约 [C4] §2 两条**硬要求**，此前两侧都没实现（2026-09-17 补齐，真机取证见 README §7.1.16）：
 *   · 限流：≤20 帧/s/对端；超限回 `error{code:"busy"}`
 *   · 幂等：以帧 `id` 去重（窗口 60 s），重复 `id` 返回**最后一次结果**（不重复执行副作用）
 *
 * 为什么必须做：`id` 幂等是"手机重试"的安全网（LAN 明文信道更容易丢/重发）；限流是防止
 * 一个坏对端把设备拖死（帧面处理链本身不便宜 —— 见 §7.1.15 的栈取证）。
 * 实现留在数据面（本模块）：这是"承载层"的保护，不改变 SDK 的帧语义。
 */
#define LAN_RATE_LIMIT_PER_S 20u     /* 契约值：20 帧/s/对端 */
#define LAN_RATE_PEERS       4u      /* 同时跟踪的对端数（台面足够；满了就轮转最旧） */
#define LAN_RATE_WINDOW_MS   1000u
#define LAN_IDEM_ENTRIES     4u      /* 幂等窗口内保留的最近帧数 */
#define LAN_IDEM_TTL_MS      60000u  /* 契约值：60 s */
#define LAN_FRAME_ID_MAX     64u
#define LAN_IDEM_REPLY_MAX   1024u   /* 单条缓存应答上限（超出则不缓存，退化为"不幂等"） */

typedef struct {
    uint32_t ip;            /* 对端 IPv4（主机序）；0 = 空槽 */
    uint32_t win_start_ms;
    uint32_t count;
} lan_rate_t;

typedef struct {
    char    id[LAN_FRAME_ID_MAX];
    int64_t at_ms;
    char   *reply;          /* 堆上副本（可为 NULL = 该次无应答） */
    size_t  reply_len;
} lan_idem_t;

static lan_rate_t s_rate[LAN_RATE_PEERS];
static lan_idem_t s_idem[LAN_IDEM_ENTRIES];

/** 取对端 IPv4（主机序）；取不到返回 0（= 不限流，避免把正常流量误判）。 */
static uint32_t frame_peer_ip(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return 0;
    }
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (getpeername(fd, (struct sockaddr *)&sa, &len) != 0 || sa.sin_family != AF_INET) {
        return 0;
    }
    return (uint32_t)ntohl(sa.sin_addr.s_addr);
}

/** 计数并判定：true = 放行，false = 超限（契约要求回 busy）。 */
static bool rate_allow(uint32_t ip, int64_t now_ms)
{
    if (ip == 0u) {
        return true;
    }
    lan_rate_t *slot = NULL;
    lan_rate_t *oldest = &s_rate[0];
    for (size_t i = 0; i < LAN_RATE_PEERS; i++) {
        if (s_rate[i].ip == ip) {
            slot = &s_rate[i];
            break;
        }
        if (s_rate[i].ip == 0u) {
            slot = &s_rate[i];
            break;
        }
        if (s_rate[i].win_start_ms < oldest->win_start_ms) {
            oldest = &s_rate[i];
        }
    }
    if (slot == NULL) {
        slot = oldest; /* 表满：轮转最旧（台面口径，够用且无分配） */
    }
    if (slot->ip != ip || now_ms - (int64_t)slot->win_start_ms >= (int64_t)LAN_RATE_WINDOW_MS) {
        slot->ip = ip;
        slot->win_start_ms = (uint32_t)now_ms;
        slot->count = 0;
    }
    slot->count++;
    return slot->count <= LAN_RATE_LIMIT_PER_S;
}

/** 从帧 JSON 里取字符串字段（零依赖扫描；找不到返回 false）。 */
static bool json_str_field(const char *body, const char *key, char *out, size_t cap)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(body, pat);
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    size_t i = 0;
    while (p[i] != '\0' && p[i] != '"' && i + 1u < cap) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0u;
}

/** 幂等命中？命中则把上次应答写回 [s_reply] 并返回 true（**不再执行**该帧的副作用）。 */
static bool idem_hit(const char *id, int64_t now_ms)
{
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < LAN_IDEM_ENTRIES; i++) {
        if (s_idem[i].id[0] == '\0' || strcmp(s_idem[i].id, id) != 0) {
            continue;
        }
        if (now_ms - s_idem[i].at_ms > (int64_t)LAN_IDEM_TTL_MS) {
            s_idem[i].id[0] = '\0'; /* 过期：当未命中，槽位顺手释放 */
            free(s_idem[i].reply);
            s_idem[i].reply = NULL;
            s_idem[i].reply_len = 0;
            return false;
        }
        s_idem[i].at_ms = now_ms;
        s_reply_len = 0;
        s_reply[0] = '\0';
        if (s_idem[i].reply != NULL && s_idem[i].reply_len > 0u) {
            set_reply(s_idem[i].reply, s_idem[i].reply_len);
        }
        ESP_LOGI(TAG, "幂等命中：id=%s（返回上次结果，不重复执行）", id);
        return true;
    }
    return false;
}

/** 记住本次结果（应答为空也记 —— "重复 id 返回最后一次结果" 含空结果）。 */
static void idem_store(const char *id, const char *reply, size_t reply_len, int64_t now_ms)
{
    if (id == NULL || id[0] == '\0' || reply_len > LAN_IDEM_REPLY_MAX) {
        return;
    }
    lan_idem_t *slot = &s_idem[0];
    for (size_t i = 0; i < LAN_IDEM_ENTRIES; i++) {
        if (s_idem[i].id[0] == '\0' || strcmp(s_idem[i].id, id) == 0) {
            slot = &s_idem[i];
            break;
        }
        if (s_idem[i].at_ms < slot->at_ms) {
            slot = &s_idem[i];
        }
    }
    free(slot->reply);
    slot->reply = NULL;
    slot->reply_len = 0;
    if (reply_len > 0u) {
        slot->reply = (char *)malloc(reply_len + 1u);
        if (slot->reply == NULL) {
            slot->id[0] = '\0';
            return; /* 内存不足：放弃缓存（退化为不幂等），不影响本次应答 */
        }
        memcpy(slot->reply, reply, reply_len);
        slot->reply[reply_len] = '\0';
        slot->reply_len = reply_len;
    }
    snprintf(slot->id, sizeof(slot->id), "%s", id);
    slot->at_ms = now_ms;
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

    const int64_t now_ms = esp_timer_get_time() / 1000;

    /* 契约 §2 限流：≤20 帧/s/对端，超限回 busy（**先于**任何帧处理，避免坏对端拖死设备）。 */
    if (!rate_allow(frame_peer_ip(req), now_ms)) {
        ESP_LOGW(TAG, "对端超限（>%u 帧/s）：回 busy", (unsigned)LAN_RATE_LIMIT_PER_S);
        free(body);
        s_reply_len = 0;
        s_reply[0] = '\0';
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"v\":1,\"t\":\"error\",\"p\":{\"code\":\"busy\","
                                "\"msg\":\"rate limit exceeded\"}}");
        return ESP_OK;
    }

    /* 契约 §2 幂等：同 `id` 在 60 s 内重复 ⇒ 直接回上次结果，**不重复执行**副作用。 */
    char frame_id[LAN_FRAME_ID_MAX] = { 0 };
    (void)json_str_field(body, "id", frame_id, sizeof(frame_id));
    s_reply_len = 0;
    s_reply[0] = '\0';
    if (idem_hit(frame_id, now_ms)) {
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

    esp_err_t err = oneye_dev_link_inject_frame(body, (size_t)received, ONEYE_DEV_LINK_CHAN_LAN);
    ESP_LOGI(TAG, "收到 lan 帧（%d B）→ inject=%d", received, (int)err);
    free(body);
    s_rx_frames++;

    if (s_reply_len > 0) {
        idem_store(frame_id, s_reply, s_reply_len, now_ms);
    } else {
        idem_store(frame_id, NULL, 0u, now_ms);
    }

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
    /* HTTP 帧面任务栈：本处理函数经 `oneye_dev_link_inject_frame()` 进 SDK 的组帧/发送路径
     * （`link_dispatch` → `oneye_dev_link_send` → `oneye_link_frame_build_simple` → `jsonw_fmt`
     *  → `vsnprintf`），实测**这条链比看上去重**。
     *
     * 真机取证（2026-09-17，两轮）：
     *   ① 6144（上一版值）**不够** —— 重复 `POST /api/link/frame`（`link.ping`）会把 httpd 栈撞穿：
     *      `***ERROR*** A stack overflow in task httpd has been detected.` / 无金丝雀时
     *      `Guru Meditation … (Double exception)` + 回溯含 `_xt_alloca_exc` ⇒ 设备重启；
     *   ② **调 `CONFIG_HTTPD_STACK_SIZE` 无效**：IDF v5.5 的 `HTTPD_DEFAULT_CONFIG()` 里
     *      `.stack_size` 是**写死的 4096**（不读该 Kconfig），而下面这行又覆盖了宏值 ⇒
     *      配置项与该任务栈无关（上一轮据此做的四档实验是无效的，已在 README §7.1.15 订正）。
     *
     * 取值依据：+2 KB 内部 RAM 换掉"偶发打重启"。若日后内部 RAM 更紧，正确做法是**降需求**
     * （把 SDK 组帧路径的 ≈2.3 KB/帧 结构下移堆分配，或把帧面处理移到独立任务），而不是压回 6144。 */
    hcfg.stack_size = 8192;
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
