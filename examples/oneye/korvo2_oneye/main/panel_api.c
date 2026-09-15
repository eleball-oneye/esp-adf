/*
 * panel_api.c —— 设备侧本地验证面实现（HTTP JSON API；边界见 panel_api.h 顶部注释）
 *
 * 设计要点：
 *   - 只读 GET；不依赖云端链路（Wi-Fi 起来即可用），MQTT 未连通时键历史仍可用（uplink 停在 pending/failed）；
 *   - 「历史响应」三态：本地检测（pending）→ event/up 已投递 SDK（queued/sent）→ 云端 ack（acked，按 data.ref 关联）；
 *   - 与串口 [board-check] 同源：自检行同时登记到本 API，面板可在线渲染板级核对表；
 *   - 不引入 cJSON：JSON 由固定形状的字符串构造器生成（键名/取值来源受控），字符串值统一转义。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "panel_api.h"
#include "media_api.h"
#include "aec_capture.h"
#include "player.h"

static const char *TAG = "panel_api";

#define PANEL_KEYS               6
#define PANEL_HISTORY_DEFAULT    50
#define PANEL_ACK_PAYLOAD_MAX    512

typedef struct {
    uint32_t seq;
    char     id[24];
    char     key[16];
    char     action[16];
    uint64_t ts_ms;
    uint64_t ack_ms;      /* 0 = 未收到云端 ack */
    char     uplink[12];  /* pending | queued | sent | failed | acked */
} panel_key_hist_t;

typedef struct {
    char  item[64];
    char  expect[24];
    char  actual[24];
    bool  pass;
} panel_check_t;

static SemaphoreHandle_t  s_lock;
static httpd_handle_t     s_httpd;

static char     s_fw[24] = "unknown";
static char     s_board[48] = "ESP32-S3-Korvo-2 v3";

static panel_check_t s_checks[PANEL_CHECK_MAX];
static int           s_check_n;
static int           s_check_fail;

static panel_key_hist_t s_hist[PANEL_KEY_HISTORY_MAX];
static int              s_hist_n;        /* 有效条数（≤ MAX） */
static int              s_hist_head;     /* 下一条写入位置 */
static uint32_t         s_key_seq;       /* 自增序号（= 历史条数累计） */
static panel_key_hist_t s_current;       /* 最近一次按键事件 */
static bool             s_has_current;
static uint32_t         s_action_count[5];  /* unknown/click/click_release/press/press_release */

static bool     s_cloud_up;
static char     s_transport[16] = "none";
static uint32_t s_tx_frames, s_rx_frames;
static bool     s_wifi_up;
static char     s_ip[20] = "";
static char     s_wifi_ssid[36] = "";
static char     s_wifi_source[64] = "";   /* "file:/sdcard/oneye-wifi.txt" | "kconfig" | "api" */

/* ------------------------------------------------------------------ 字符串构造器（JSON） */

typedef struct {
    char  *p;
    size_t len;
    size_t cap;
} sb_t;

static bool sb_init(sb_t *s, size_t cap)
{
    s->p = (char *)malloc(cap);
    if (s->p == NULL) {
        return false;
    }
    s->cap = cap;
    s->len = 0;
    s->p[0] = '\0';
    return true;
}

static bool sb_reserve(sb_t *s, size_t extra)
{
    if (s->len + extra + 1 <= s->cap) {
        return true;
    }
    size_t ncap = s->cap * 2;
    while (ncap < s->len + extra + 1) {
        ncap *= 2;
    }
    char *np = (char *)realloc(s->p, ncap);
    if (np == NULL) {
        return false;
    }
    s->p = np;
    s->cap = ncap;
    return true;
}

static void sb_raw(sb_t *s, const char *txt)
{
    size_t n = strlen(txt);
    if (!sb_reserve(s, n)) {
        return;
    }
    memcpy(s->p + s->len, txt, n);
    s->len += n;
    s->p[s->len] = '\0';
}

static void sb_str(sb_t *s, const char *txt)   /* JSON 字符串（含转义与引号） */
{
    sb_raw(s, "\"");
    for (const unsigned char *c = (const unsigned char *)(txt ? txt : ""); *c; c++) {
        switch (*c) {
        case '"':  sb_raw(s, "\\\""); break;
        case '\\': sb_raw(s, "\\\\"); break;
        case '\n': sb_raw(s, "\\n");  break;
        case '\r': sb_raw(s, "\\r");  break;
        case '\t': sb_raw(s, "\\t");  break;
        default:
            if (*c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*c);
                sb_raw(s, esc);
            } else {
                char one[2] = { (char)*c, '\0' };
                sb_raw(s, one);
            }
            break;
        }
    }
    sb_raw(s, "\"");
}

static void sb_fmt(sb_t *s, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_raw(s, tmp);
}

static void sb_kv_str(sb_t *s, const char *k, const char *v)
{
    sb_str(s, k);
    sb_raw(s, ":");
    sb_str(s, v);
}

static void sb_kv_i(sb_t *s, const char *k, long long v)
{
    sb_str(s, k);
    sb_raw(s, ":");
    sb_fmt(s, "%lld", v);
}

/* ------------------------------------------------------------------ 数据登记实现 */

void panel_api_set_identity(const char *fw_version, const char *board_name)
{
    if (fw_version) {
        snprintf(s_fw, sizeof(s_fw), "%s", fw_version);
    }
    if (board_name) {
        snprintf(s_board, sizeof(s_board), "%s", board_name);
    }
}

void panel_api_record_check(const char *item, const char *expect, const char *actual, bool pass)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_check_n++;                                  /* 累计项数（超上限只计数不入表） */
    if (s_check_n <= PANEL_CHECK_MAX) {
        panel_check_t *c = &s_checks[s_check_n - 1];
        snprintf(c->item, sizeof(c->item), "%s", item ? item : "");
        snprintf(c->expect, sizeof(c->expect), "%s", expect ? expect : "");
        snprintf(c->actual, sizeof(c->actual), "%s", actual ? actual : "");
        c->pass = pass;
    }
    if (!pass) {
        s_check_fail++;
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static int action_index(const char *action)
{
    if (action == NULL) {
        return 0;
    }
    if (strcmp(action, "click") == 0)          return 1;
    if (strcmp(action, "click_release") == 0)  return 2;
    if (strcmp(action, "press") == 0)          return 3;
    if (strcmp(action, "press_release") == 0)  return 4;
    return 0;
}

void panel_api_key_event(const char *key, const char *action, const char *id, uint64_t ts_ms)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    panel_key_hist_t *e = &s_hist[s_hist_head];
    memset(e, 0, sizeof(*e));
    s_key_seq++;
    e->seq = s_key_seq;
    snprintf(e->id, sizeof(e->id), "%s", id ? id : "");
    snprintf(e->key, sizeof(e->key), "%s", key ? key : "unknown");
    snprintf(e->action, sizeof(e->action), "%s", action ? action : "unknown");
    e->ts_ms = ts_ms;
    e->ack_ms = 0;
    snprintf(e->uplink, sizeof(e->uplink), "pending");
    s_hist_head = (s_hist_head + 1) % PANEL_KEY_HISTORY_MAX;
    if (s_hist_n < PANEL_KEY_HISTORY_MAX) {
        s_hist_n++;
    }
    s_current = *e;
    s_has_current = true;
    s_action_count[action_index(action)]++;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "key event #%u %s/%s id=%s", (unsigned)s_key_seq, e->key, e->action, e->id);
}

void panel_api_key_uplink(const char *id, const char *state)
{
    if (id == NULL || state == NULL) {
        return;
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    for (int i = 0; i < s_hist_n; i++) {
        if (strcmp(s_hist[i].id, id) == 0) {
            snprintf(s_hist[i].uplink, sizeof(s_hist[i].uplink), "%s", state);
            if (strcmp(s_current.id, id) == 0) {
                snprintf(s_current.uplink, sizeof(s_current.uplink), "%s", state);
            }
            break;
        }
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void panel_api_key_ack(const char *payload, size_t len)
{
    if (payload == NULL || len == 0) {
        return;
    }
    char buf[PANEL_ACK_PAYLOAD_MAX];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0';

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    for (int i = 0; i < s_hist_n; i++) {
        if (s_hist[i].id[0] == '\0' || strcmp(s_hist[i].uplink, "acked") == 0) {
            continue;
        }
        if (strstr(buf, s_hist[i].id) != NULL) {   /* data.ref == 上行 id */
            snprintf(s_hist[i].uplink, sizeof(s_hist[i].uplink), "acked");
            s_hist[i].ack_ms = (uint64_t)(esp_timer_get_time() / 1000);
            if (strcmp(s_current.id, s_hist[i].id) == 0) {
                s_current.ack_ms = s_hist[i].ack_ms;
                snprintf(s_current.uplink, sizeof(s_current.uplink), "acked");
            }
            ESP_LOGI(TAG, "key event %s 已被云端 ack", s_hist[i].id);
            break;
        }
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void panel_api_set_link(bool cloud_link_up, const char *transport, uint32_t tx_frames, uint32_t rx_frames)
{
    s_cloud_up = cloud_link_up;
    if (transport) {
        snprintf(s_transport, sizeof(s_transport), "%s", transport);
    }
    s_tx_frames = tx_frames;
    s_rx_frames = rx_frames;
}

void panel_api_set_wifi(bool connected, const char *ip, const char *ssid, const char *source)
{
    s_wifi_up = connected;
    if (ip) {
        snprintf(s_ip, sizeof(s_ip), "%s", ip);
    }
    if (ssid) {
        snprintf(s_wifi_ssid, sizeof(s_wifi_ssid), "%s", ssid);
    }
    if (source) {
        snprintf(s_wifi_source, sizeof(s_wifi_source), "%s", source);
    }
}

/* ------------------------------------------------------------------ HTTP 处理 */

static esp_err_t send_json(httpd_req_t *req, sb_t *s)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");   /* 面板可能跨源取数 */
    esp_err_t rc = httpd_resp_send(req, s->p ? s->p : "{}", s->len);
    free(s->p);
    s->p = NULL;
    return rc;
}

static int query_int(httpd_req_t *req, const char *name, int dflt)
{
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return dflt;
    }
    char v[16];
    if (httpd_query_key_value(q, name, v, sizeof(v)) != ESP_OK) {
        return dflt;
    }
    int n = atoi(v);
    return n > 0 ? n : dflt;
}

static esp_err_t h_ping(httpd_req_t *req)
{
    sb_t s;
    if (!sb_init(&s, 256)) {
        return httpd_resp_send_500(req);
    }
    sb_raw(&s, "{");
    sb_kv_str(&s, "ok", "true");
    sb_raw(&s, ",");
    sb_kv_str(&s, "api_version", "1");
    sb_raw(&s, ",");
    sb_kv_str(&s, "role", "local-verification-only");
    sb_raw(&s, "}");
    return send_json(req, &s);
}

static esp_err_t h_status(httpd_req_t *req)
{
    sb_t s;
    if (!sb_init(&s, 1024)) {
        return httpd_resp_send_500(req);
    }
    uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);

    sb_raw(&s, "{");
    sb_kv_str(&s, "fw", s_fw);          sb_raw(&s, ",");
    sb_kv_str(&s, "board", s_board);    sb_raw(&s, ",");
    sb_kv_i(&s, "uptime_ms", (long long)uptime_ms); sb_raw(&s, ",");

    sb_raw(&s, "\"wifi\":{");
    sb_fmt(&s, "\"connected\":%s,", s_wifi_up ? "true" : "false");
    sb_kv_str(&s, "ip", s_ip); sb_raw(&s, ",");
    sb_kv_str(&s, "ssid", s_wifi_ssid); sb_raw(&s, ",");
    sb_kv_str(&s, "source", s_wifi_source);
    sb_raw(&s, "},");

    sb_raw(&s, "\"cloud\":{");
    sb_fmt(&s, "\"link_up\":%s,", s_cloud_up ? "true" : "false");
    sb_kv_str(&s, "transport", s_transport); sb_raw(&s, ",");
    sb_kv_i(&s, "tx_frames", (long long)s_tx_frames); sb_raw(&s, ",");
    sb_kv_i(&s, "rx_frames", (long long)s_rx_frames); sb_raw(&s, "},");

    sb_raw(&s, "\"selftest\":{");
    sb_kv_i(&s, "total", (long long)s_check_n); sb_raw(&s, ",");
    sb_kv_i(&s, "failed", (long long)s_check_fail); sb_raw(&s, "},");

    sb_raw(&s, "\"keys\":{");
    sb_kv_i(&s, "count", PANEL_KEYS); sb_raw(&s, ",");
    sb_kv_i(&s, "events", (long long)s_key_seq); sb_raw(&s, ",");
    sb_kv_i(&s, "history_max", PANEL_KEY_HISTORY_MAX);
    sb_raw(&s, "},");

    /* 媒体与 AEC 采集（本地验证面：面板"音频"卡片据此启用/禁用录音按钮） */
    aec_capture_status_t aec;
    aec_capture_get_status(&aec);
    sb_raw(&s, "\"aec\":{");
    sb_fmt(&s, "\"enabled\":%s,", aec.enabled ? "true" : "false");
    sb_fmt(&s, "\"recording\":%s,", aec.recording ? "true" : "false");
    sb_kv_str(&s, "last_file", aec.last_file); sb_raw(&s, ",");
    sb_kv_i(&s, "last_bytes", (long long)aec.last_bytes); sb_raw(&s, ",");
    sb_kv_i(&s, "total_files", (long long)aec.total_files); sb_raw(&s, ",");
    sb_kv_str(&s, "root", aec_capture_root());
    sb_raw(&s, "},");

    sb_raw(&s, "\"media\":{");
    sb_fmt(&s, "\"sd_mounted\":%s,", aec.sd_mounted ? "true" : "false");
    sb_kv_str(&s, "rec_root", aec_capture_root()); sb_raw(&s, ",");
    sb_kv_i(&s, "poll_hint_ms", 1000);
    sb_raw(&s, "},");

    /* 板上回放状态（面板据此显示"播放中/文件名/音量"并切换按钮） */
    player_status_t ps;
    player_get_status(&ps);
    sb_raw(&s, "\"player\":{");
    sb_fmt(&s, "\"playing\":%s,", ps.playing ? "true" : "false");
    sb_kv_str(&s, "path", ps.path); sb_raw(&s, ",");
    sb_kv_str(&s, "codec", ps.codec); sb_raw(&s, ",");
    sb_kv_i(&s, "rate_hz", (long long)ps.rate_hz); sb_raw(&s, ",");
    sb_kv_i(&s, "channels", (long long)ps.channels); sb_raw(&s, ",");
    sb_kv_i(&s, "elapsed_ms", (long long)ps.elapsed_ms); sb_raw(&s, ",");
    sb_kv_i(&s, "volume", (long long)ps.volume); sb_raw(&s, ",");
    sb_kv_str(&s, "msg", ps.last_msg);
    sb_raw(&s, "},");

    sb_raw(&s, "\"panel\":{");
    sb_kv_str(&s, "api_version", "1");
    sb_raw(&s, ",");
    sb_kv_str(&s, "scope", "local-verification-only");
    sb_raw(&s, "}");
    sb_raw(&s, "}");
    return send_json(req, &s);
}

static esp_err_t h_selftest(httpd_req_t *req)
{
    sb_t s;
    if (!sb_init(&s, 2048)) {
        return httpd_resp_send_500(req);
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    int n = s_check_n < PANEL_CHECK_MAX ? s_check_n : PANEL_CHECK_MAX;

    sb_raw(&s, "{");
    sb_kv_i(&s, "total", (long long)s_check_n); sb_raw(&s, ",");
    sb_kv_i(&s, "failed", (long long)s_check_fail); sb_raw(&s, ",");
    sb_kv_str(&s, "note", "运行期核对项；编译期断言（board_expect.h）失败时固件根本不构建");
    sb_raw(&s, ",");
    sb_raw(&s, "\"items\":[");
    for (int i = 0; i < n; i++) {
        if (i) {
            sb_raw(&s, ",");
        }
        sb_raw(&s, "{");
        sb_kv_str(&s, "item", s_checks[i].item);   sb_raw(&s, ",");
        sb_kv_str(&s, "expect", s_checks[i].expect); sb_raw(&s, ",");
        sb_kv_str(&s, "actual", s_checks[i].actual); sb_raw(&s, ",");
        sb_fmt(&s, "\"pass\":%s", s_checks[i].pass ? "true" : "false");
        sb_raw(&s, "}");
    }
    sb_raw(&s, "]}");
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return send_json(req, &s);
}

static const char *key_label(int id)
{
    switch (id) {
    case 0: return "volup";
    case 1: return "voldown";
    case 2: return "set";
    case 3: return "play";
    case 4: return "mode";
    case 5: return "rec";
    default: return "unknown";
    }
}

static void sb_key_entry(sb_t *s, const panel_key_hist_t *e, bool with_seq)
{
    sb_raw(s, "{");
    if (with_seq) {
        sb_kv_i(s, "seq", (long long)e->seq);
        sb_raw(s, ",");
    }
    sb_kv_str(s, "id", e->id);        sb_raw(s, ",");
    sb_kv_str(s, "key", e->key);      sb_raw(s, ",");
    sb_kv_str(s, "action", e->action); sb_raw(s, ",");
    sb_kv_i(s, "ts_ms", (long long)e->ts_ms); sb_raw(s, ",");
    sb_kv_str(s, "uplink", e->uplink); sb_raw(s, ",");
    sb_kv_i(s, "ack_ms", (long long)e->ack_ms);
    sb_raw(s, "}");
}

static esp_err_t h_keys(httpd_req_t *req)
{
    int limit = query_int(req, "limit", PANEL_HISTORY_DEFAULT);
    if (limit > PANEL_KEY_HISTORY_MAX) {
        limit = PANEL_KEY_HISTORY_MAX;
    }
    sb_t s;
    if (!sb_init(&s, 4096)) {
        return httpd_resp_send_500(req);
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    sb_raw(&s, "{");
    sb_raw(&s, "\"keys\":[");
    for (int i = 0; i < PANEL_KEYS; i++) {
        if (i) {
            sb_raw(&s, ",");
        }
        sb_raw(&s, "{");
        sb_kv_i(&s, "id", i); sb_raw(&s, ",");
        sb_kv_str(&s, "label", key_label(i));
        sb_raw(&s, "}");
    }
    sb_raw(&s, "],");

    sb_raw(&s, "\"current\":");
    if (s_has_current) {
        sb_key_entry(&s, &s_current, true);
    } else {
        sb_raw(&s, "null");
    }
    sb_raw(&s, ",");

    sb_raw(&s, "\"counters\":{");
    sb_kv_i(&s, "total", (long long)s_key_seq); sb_raw(&s, ",");
    sb_kv_i(&s, "unknown", (long long)s_action_count[0]); sb_raw(&s, ",");
    sb_kv_i(&s, "click", (long long)s_action_count[1]); sb_raw(&s, ",");
    sb_kv_i(&s, "click_release", (long long)s_action_count[2]); sb_raw(&s, ",");
    sb_kv_i(&s, "press", (long long)s_action_count[3]); sb_raw(&s, ",");
    sb_kv_i(&s, "press_release", (long long)s_action_count[4]);
    sb_raw(&s, "},");

    sb_kv_i(&s, "history_total", (long long)s_key_seq); sb_raw(&s, ",");
    sb_raw(&s, "\"history\":[");
    /* 历史按**新→旧**输出：ring buffer 从 head-1 往回走 */
    int emitted = 0;
    for (int i = 0; i < s_hist_n && emitted < limit; i++) {
        int idx = (s_hist_head - 1 - i + PANEL_KEY_HISTORY_MAX * 2) % PANEL_KEY_HISTORY_MAX;
        if (emitted) {
            sb_raw(&s, ",");
        }
        sb_key_entry(&s, &s_hist[idx], true);
        emitted++;
    }
    sb_raw(&s, "]}");
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return send_json(req, &s);
}

esp_err_t panel_api_start(uint16_t port)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (port == 0) {
#ifdef CONFIG_ONEYE_FW_PANEL_PORT
        port = (uint16_t)CONFIG_ONEYE_FW_PANEL_PORT;
#else
        port = 80;
#endif
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port;
    cfg.max_uri_handlers = 12;          /* 4 个 /api 只读 + /media/list + 媒体通配 + /api/action（留余量） */
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 10;
    /* ⚠️ 必须显式指定通配匹配函数：IDF 的 HTTPD_DEFAULT_CONFIG() 里 uri_match_fn = NULL，
     * 而 httpd_find_uri_handler() 在 NULL 时退化为 httpd_uri_match_simple（**精确串比较**），
     * 于是以星号结尾的通配路由（如媒体下载路由）**永不命中**——真机实测：/media/list 正常，
     * 而 /media/<alias>/<path> 返回 "Nothing matches the given URI"。
     * 主机 mock 用 Python 自写前缀匹配，故该缺陷只能在真机暴露。 */
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t rc = httpd_start(&s_httpd, &cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败：%s（面板不可用，固件其余功能不受影响）", esp_err_to_name(rc));
        s_httpd = NULL;
        return rc;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/api/ping",     .method = HTTP_GET, .handler = h_ping },
        { .uri = "/api/status",   .method = HTTP_GET, .handler = h_status },
        { .uri = "/api/selftest", .method = HTTP_GET, .handler = h_selftest },
        { .uri = "/api/keys",     .method = HTTP_GET, .handler = h_keys },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        rc = httpd_register_uri_handler(s_httpd, &uris[i]);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败：%s", uris[i].uri, esp_err_to_name(rc));
        }
    }
    ESP_LOGI(TAG, "本地验证面已启动：http://<设备IP>:%u/api/{ping,status,selftest,keys}", (unsigned)port);
    /* 媒体与动作（/media/list、/media/<alias>/<path>、POST /api/action）注册到同一实例 */
    (void)media_api_register(s_httpd);
    ESP_LOGW(TAG, "提示：该 API 仅为台面/联调验证面，不是云端设备面契约；量产应置 CONFIG_ONEYE_FW_ENABLE_PANEL_API=n");
    return ESP_OK;
}

bool panel_api_is_running(void)
{
    return s_httpd != NULL;
}
