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
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "panel_api.h"
#include "media_api.h"
#include "aec_capture.h"
#include "player.h"
#include "lcd_ui.h"
#include "camera_api.h"
#include "esp_camera.h"     /* CAMERA_FB_IN_PSRAM / PIXFORMAT_*（仅本地验证面重配旋钮用） */

static const char *TAG = "panel_api";

#define PANEL_HISTORY_DEFAULT    50
#define PANEL_ACK_PAYLOAD_MAX    512

/* 板载按键清单：**按 ADF 的 user_id 取值**（`board_def.h` 的 `INPUT_KEY_DEFAULT_INFO()`：
 * REC=1 / MUTE=7 / SET=2 / PLAY=3 / VOLUP=6 / VOLDOWN=5 —— 注意本板**没有 MODE 键**，
 * 且 id 不是 0..5；早前按下标 0..5 猜标签会把 MUTE 显示成 MODE、并把 VOLUP/VOLDOWN 错位）。 */
static const struct {
    int         id;
    const char *label;
} k_panel_keys[] = {
    { 1, "rec" }, { 7, "mute" }, { 2, "set" }, { 3, "play" }, { 6, "volup" }, { 5, "voldown" },
};
#define PANEL_KEY_COUNT ((int)(sizeof(k_panel_keys) / sizeof(k_panel_keys[0])))

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

/* 授时状态（契约 §7）：未授时时 synced=false、offset_ms=0 ⇒ 时间戳为运行时刻 */
static bool     s_time_synced;
static uint64_t s_time_cloud_ts_ms;
static int64_t  s_time_offset_ms;
static char     s_time_source[32] = "";
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

/* 设备当前墙上时间（毫秒）：定义见下方（授时状态之后）；此处前置声明供 ack 时间戳使用 */
static uint64_t panel_api_now_ms(void);

/* 把某条历史（按索引）标记为云端已确认；ok=false 记 failed。
 * ack 时间与事件时间用**同一时钟**（panel_api_now_ms：已授时=UTC 毫秒，未授时=运行时刻），
 * 否则「端到端 = ack_ms − ts_ms」在授时前后会得到负值/天文数字。 */
static void panel_key_mark_acked_locked(int i, bool ok)
{
    snprintf(s_hist[i].uplink, sizeof(s_hist[i].uplink), "%s", ok ? "acked" : "failed");
    s_hist[i].ack_ms = ok ? panel_api_now_ms() : 0;
    if (strcmp(s_current.id, s_hist[i].id) == 0) {
        s_current.ack_ms = s_hist[i].ack_ms;
        snprintf(s_current.uplink, sizeof(s_current.uplink), "%s", s_hist[i].uplink);
    }
}

bool panel_api_key_ack(const char *payload, size_t len)
{
    if (payload == NULL || len == 0) {
        return false;
    }
    char buf[PANEL_ACK_PAYLOAD_MAX];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0';

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    bool hit = false;
    for (int i = 0; i < s_hist_n; i++) {
        if (s_hist[i].id[0] == '\0' || strcmp(s_hist[i].uplink, "acked") == 0) {
            continue;
        }
        if (strstr(buf, s_hist[i].id) != NULL) {   /* 台面手动 ack：ref 恰为本地事件 id */
            panel_key_mark_acked_locked(i, true);
            ESP_LOGI(TAG, "key event %s 已被云端 ack（ref 内含本地 id）", s_hist[i].id);
            hit = true;
            break;
        }
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return hit;
}

bool panel_api_key_ack_item(const char *id, bool ok)
{
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    bool hit = false;
    for (int i = 0; i < s_hist_n; i++) {
        if (strcmp(s_hist[i].id, id) == 0) {
            panel_key_mark_acked_locked(i, ok);
            hit = true;
            break;
        }
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return hit;
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

/** 本地验证面：授时状态登记（契约 §7；由 TIME_SYNCED 事件驱动） */
void panel_api_set_time(bool synced, uint64_t cloud_ts_ms, int64_t offset_ms, const char *source)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_time_synced = synced;
    s_time_cloud_ts_ms = cloud_ts_ms;
    s_time_offset_ms = offset_ms;
    if (source != NULL) {
        snprintf(s_time_source, sizeof(s_time_source), "%s", source);
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/** 设备当前墙上时间（毫秒）：未授时时为运行时刻（两段历史都由此口径自证一致） */
static uint64_t panel_api_now_ms(void)
{
    uint64_t up = (uint64_t)(esp_timer_get_time() / 1000);
    return s_time_synced ? (uint64_t)((int64_t)up + s_time_offset_ms) : up;
}

/** 本次启动的复位原因（面板/联调判定"离线在线跳变"是否来自设备重启） */
static const char *panel_api_reset_reason(void){
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "poweron";
        case ESP_RST_EXT:      return "ext";
        case ESP_RST_SW:       return "sw";        /* esp_restart() 或 assert/panic 后软复位 */
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT:      return "wdt";
        case ESP_RST_DEEPSLEEP:return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO:     return "sdio";
        default:               return "unknown";
    }
}

/* 按键注入回调（由 korvo2_oneye_main.c 注册；未注册 = 该路由不可用） */
static panel_key_sim_fn_t s_key_sim;

void panel_api_set_key_simulator(panel_key_sim_fn_t fn)
{
    s_key_sim = fn;
}

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
    sb_kv_i(&s, "count", PANEL_KEY_COUNT); sb_raw(&s, ",");
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
    /* 内存视图（本地验证面）：排查「SD 写入失败（DMA 内存不足）/ 任务栈不足」这类问题必需 */
    sb_raw(&s, "\"heap\":{");
    sb_kv_i(&s, "internal_free", (long long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    sb_raw(&s, ",");
    sb_kv_i(&s, "internal_largest", (long long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    sb_raw(&s, ",");
    sb_kv_i(&s, "dma_free", (long long)heap_caps_get_free_size(MALLOC_CAP_DMA));
    sb_raw(&s, ",");
    sb_kv_i(&s, "dma_largest", (long long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    sb_raw(&s, ",");
    sb_kv_i(&s, "psram_free", (long long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    sb_raw(&s, "},");
    /* 重启原因（本地验证面）：面板看到「设备离线/在线来回跳」时，用它区分
     * 「设备在重启」与「网络抖动」——SW/panic 复位会给出确切复位码。 */
    sb_kv_str(&s, "reset_reason", panel_api_reset_reason());
    sb_raw(&s, ",");
    /* 授时状态（契约 §7）：synced=true ⇒ 所有 ts 为 UTC 毫秒；否则为运行时刻（已告警）。
     * now_ms 为设备当前墙上时间（未授时时 = 运行毫秒），供面板显示/对照。 */
    sb_raw(&s, "\"time\":{");
    sb_raw(&s, s_time_synced ? "\"synced\":true" : "\"synced\":false");   /* 契约口径：bool */
    sb_raw(&s, ",");
    sb_kv_i(&s, "cloud_ts_ms", (long long)s_time_cloud_ts_ms);
    sb_raw(&s, ",");
    sb_kv_i(&s, "offset_ms", (long long)s_time_offset_ms);
    sb_raw(&s, ",");
    sb_kv_str(&s, "source", s_time_source);
    sb_raw(&s, ",");
    sb_kv_i(&s, "now_ms", (long long)panel_api_now_ms());
    sb_raw(&s, "},");
    /* 板载 LCD（本地验证面）：面板可读状态并可下发绘制；未启用 LCD 时 ready=false */
    {
        lcd_ui_state_t lc;
        lcd_ui_get_state(&lc);
        sb_raw(&s, "\"lcd\":{");
        sb_kv_str(&s, "ready", lc.ready ? "true" : "false");
        sb_raw(&s, ",");
        sb_kv_i(&s, "w", lc.w); sb_raw(&s, ",");
        sb_kv_i(&s, "h", lc.h); sb_raw(&s, ",");
        sb_kv_str(&s, "pattern", lc.pattern); sb_raw(&s, ",");
        sb_kv_i(&s, "draws", (long long)lc.draws); sb_raw(&s, ",");
        sb_kv_i(&s, "last_ms", (long long)lc.last_ms); sb_raw(&s, ",");
        sb_kv_i(&s, "fb_bytes", (long long)lc.fb_bytes); sb_raw(&s, ",");
        sb_kv_str(&s, "fb_mem", lc.fb_in_psram ? "psram" : (lc.ready ? "internal" : "-"));
        sb_raw(&s, "},");
    }
    /* 板载摄像头（本地验证面）：sensor PID 是"模组是否装配"的取证依据（0 = 未识别/未装配） */
    {
        camera_state_t cs;
        camera_api_get_state(&cs);
        sb_raw(&s, "\"camera\":{");
        sb_kv_str(&s, "inited", cs.inited ? "true" : "false");
        sb_raw(&s, ",");
        sb_kv_str(&s, "sensor", cs.sensor);
        sb_raw(&s, ",");
        sb_kv_i(&s, "pid", (long long)cs.pid); sb_raw(&s, ",");
        sb_kv_i(&s, "pid_hex", (long long)cs.pid); sb_raw(&s, ",");   /* 面板按 hex 展示 */
        sb_kv_i(&s, "frames", (long long)cs.frames); sb_raw(&s, ",");
        sb_kv_i(&s, "errors", (long long)cs.errors); sb_raw(&s, ",");
        sb_kv_i(&s, "last_bytes", (long long)cs.last_bytes); sb_raw(&s, ",");
        sb_kv_i(&s, "last_w", cs.last_width); sb_raw(&s, ",");
        sb_kv_i(&s, "last_h", cs.last_height); sb_raw(&s, ",");
        sb_kv_i(&s, "last_ms", (long long)cs.last_ms); sb_raw(&s, ",");
        sb_kv_str(&s, "last_path", cs.last_path); sb_raw(&s, ",");
        sb_kv_str(&s, "last_err", cs.last_err); sb_raw(&s, ",");
        /* 花屏对策可观测性：彩噪评分（越小越干净）与实际取样帧数 */
        sb_kv_i(&s, "last_noise", (long long)cs.last_noise); sb_raw(&s, ",");
        sb_kv_i(&s, "last_grabs", cs.last_grabs); sb_raw(&s, ",");
        sb_kv_str(&s, "root", cs.root);
        sb_raw(&s, ",");
        /* 生效配置（排障用：NO-SOI 类取帧失败靠这几个旋钮在真机上对比定位） */
        sb_kv_str(&s, "format", cs.format); sb_raw(&s, ",");
        sb_kv_str(&s, "fb_loc", cs.fb_loc); sb_raw(&s, ",");
        sb_kv_i(&s, "fb_count", cs.fb_count); sb_raw(&s, ",");
        sb_kv_str(&s, "grab", cs.grab); sb_raw(&s, ",");
        sb_kv_i(&s, "xclk_mhz", cs.xclk_mhz); sb_raw(&s, ",");
        sb_kv_i(&s, "psram_dma", cs.psram_dma); sb_raw(&s, ",");
        sb_kv_str(&s, "size", cs.size); sb_raw(&s, ",");
        sb_kv_i(&s, "quality", cs.quality); sb_raw(&s, ",");
        /* MJPEG 预览流（本地验证面）：port=0 ⇒ 未启动，面板回落到「单帧轮询预览」 */
        sb_kv_i(&s, "stream_port", cs.stream_port); sb_raw(&s, ",");
        sb_kv_i(&s, "stream_frames", (long long)cs.stream_frames); sb_raw(&s, ",");
        sb_kv_i(&s, "stream_clients", cs.stream_clients); sb_raw(&s, ",");
        sb_kv_i(&s, "stream_ends", (long long)cs.stream_ends); sb_raw(&s, ",");
        sb_kv_str(&s, "stream_end_reason", cs.stream_end_reason);
        sb_raw(&s, "},");
    }
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

/* 板载按键清单（定义见文件顶部 `k_panel_keys`；标签按 ADF user_id 取值，非下标） */

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
    for (int i = 0; i < PANEL_KEY_COUNT; i++) {
        if (i) {
            sb_raw(&s, ",");
        }
        sb_raw(&s, "{");
        sb_kv_i(&s, "id", k_panel_keys[i].id); sb_raw(&s, ",");
        sb_kv_str(&s, "label", k_panel_keys[i].label);
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

/* POST /api/simulate/key?key=rec&action=click
 *
 * 本地验证面专用：把一次**按键事件**按与物理按键**完全相同**的路径上报（同一函数、同一 id
 * 生成、同一 event/up 组装与 SDK 投递），用于远程/自动化验证「上行 → 云端 ack → 面板第三态」。
 * 触发源是 HTTP 而非 ADC 按键 ⇒ 日志里带 `(注入)` 标记，且**不得**当作物理按键的验收证据。 */
static esp_err_t h_simulate_key(httpd_req_t *req)
{
    char q[64];
    char key[16] = { 0 };
    char action[24] = { 0 };

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "key", key, sizeof(key)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "need ?key=<volup|voldown|set|play|mode|rec>[&action=click|click_release|press|press_release]");
    }
    if (httpd_query_key_value(q, "action", action, sizeof(action)) != ESP_OK) {
        snprintf(action, sizeof(action), "click");
    }
    if (s_key_sim == NULL) {
        /* IDF 的 esp_http_server 没有 503 枚举，用 500 表示「注入器未注册」 */
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "key simulator 未注册（按键路径未就绪）");
    }
    if (s_key_sim(key, action) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "key/action 非法");
    }

    sb_t s;
    if (!sb_init(&s, 256)) {
        return httpd_resp_send_500(req);
    }
    sb_raw(&s, "{");
    sb_kv_str(&s, "ok", "true");
    sb_raw(&s, ",");
    sb_kv_str(&s, "key", key);
    sb_raw(&s, ",");
    sb_kv_str(&s, "action", action);
    sb_raw(&s, ",");
    sb_kv_str(&s, "note", "injected on the local verification surface; same uplink path as a physical key");
    sb_raw(&s, "}");
    return send_json(req, &s);
}

/* POST /api/lcd/draw?pattern=bars|grid|checker|test|status
 *
 * 本地验证面：驱动板载 ILI9341 绘制**可对账**的图案（bars/grid/checker/test 用于核对分辨率、
 * 镜像、颜色与偏移；status 为状态屏）。面板据此验证"设备真的会显示"，人眼/拍照即可复核。 */
static esp_err_t h_lcd_draw(httpd_req_t *req)
{
    char q[64];
    char pattern[16] = { 0 };

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "pattern", pattern, sizeof(pattern)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "need ?pattern=bars|grid|checker|test|status");
    }
    esp_err_t rc = lcd_ui_set_pattern(pattern);
    if (rc == ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "LCD 未启用/未就绪（CONFIG_ONEYE_FW_ENABLE_LCD）");
    }
    if (rc != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pattern 非法");
    }

    sb_t s;
    if (!sb_init(&s, 256)) {
        return httpd_resp_send_500(req);
    }
    lcd_ui_state_t lc;
    lcd_ui_get_state(&lc);
    sb_raw(&s, "{");
    sb_kv_str(&s, "ok", "true");
    sb_raw(&s, ",");
    sb_kv_str(&s, "pattern", lc.pattern);
    sb_raw(&s, ",");
    sb_kv_i(&s, "draws", (long long)lc.draws);
    sb_raw(&s, "}");
    return send_json(req, &s);
}

/* POST /api/camera/capture
 *
 * 本地验证面：抓一帧 JPEG 并落盘（SD 优先、SPIFFS 兜底）。落盘目录 `cam/` 走既有
 * `/media/<alias>/<path>` 只读面（带 Range）供网页显示 —— 不新增对外形态。 */
static esp_err_t h_camera_capture(httpd_req_t *req)
{
    if (!camera_api_ready()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "camera 未初始化（SCCB 未探测到 sensor？）");
    }
    camera_capture_t cap;
    esp_err_t rc = camera_api_capture(&cap);

    sb_t s;
    if (!sb_init(&s, 512)) {
        return httpd_resp_send_500(req);
    }
    sb_raw(&s, "{");
    sb_kv_str(&s, "ok", rc == ESP_OK ? "true" : "false");
    sb_raw(&s, ",");
    sb_kv_str(&s, "path", cap.path);
    sb_raw(&s, ",");
    sb_kv_i(&s, "bytes", (long long)cap.bytes); sb_raw(&s, ",");
    sb_kv_i(&s, "w", cap.width); sb_raw(&s, ",");
    sb_kv_i(&s, "h", cap.height); sb_raw(&s, ",");
    sb_kv_i(&s, "ms", (long long)cap.ms); sb_raw(&s, ",");
    sb_kv_str(&s, "err", cap.err); sb_raw(&s, ",");
    /* 网页可直接用的地址（经设备 /media 面） */
    {
        char rel[104];
        char url[128];
        const char *alias = (strncmp(cap.path, "/spiffs/", 8) == 0) ? "spiffs" : "sdcard";
        const char *p = cap.path;
        if (strncmp(p, "/sdcard/", 8) == 0 || strncmp(p, "/spiffs/", 8) == 0) {
            p += 8;
        }
        snprintf(rel, sizeof(rel), "%.*s", (int)sizeof(cap.path) - 1, p);
        snprintf(url, sizeof(url), "media/%s/%s", alias, rel);
        sb_kv_str(&s, "url", url);
    }
    sb_raw(&s, "}");
    return send_json(req, &s);
}

/* POST /api/camera/reinit?fb=dram|psram&fmt=jpeg|rgb565&fbc=1|2|3&grab=when_empty|latest&xclk=10|20|40&q=0..63
 *
 * 本地验证面：以运行时旋钮重配摄像头，用于在**同一块板子**上对比定位取帧失败
 * （真机实测教训：NO-SOI / fb timeout 与 fb 位置、像素格式相关，靠反复烧写猜测代价太高）。
 * 缺省参数 = 保持当前值；只在本地面使用，不进云端契约。 */
static esp_err_t h_camera_reinit(httpd_req_t *req)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    char *q = (char *)malloc(qlen > 1 ? qlen : 2);
    if (q == NULL) {
        return httpd_resp_send_500(req);
    }
    char v[24];
    camera_cfg_t cfg = { -1, -1, -1, -1, -1, -1, -1, -1 };

    if (qlen > 1 && httpd_req_get_url_query_str(req, q, qlen) == ESP_OK) {
        if (httpd_query_key_value(q, "fb", v, sizeof(v)) == ESP_OK) {
            cfg.fb_location = (strcmp(v, "psram") == 0) ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
        }
        if (httpd_query_key_value(q, "fmt", v, sizeof(v)) == ESP_OK) {
            cfg.pixel_format = (strcmp(v, "jpeg") == 0) ? PIXFORMAT_JPEG : PIXFORMAT_RGB565;
        }
        if (httpd_query_key_value(q, "fbc", v, sizeof(v)) == ESP_OK) {
            cfg.fb_count = atoi(v);
        }
        if (httpd_query_key_value(q, "grab", v, sizeof(v)) == ESP_OK) {
            cfg.grab_mode = (strcmp(v, "latest") == 0) ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
        }
        if (httpd_query_key_value(q, "xclk", v, sizeof(v)) == ESP_OK) {
            cfg.xclk_mhz = atoi(v);
        }
        if (httpd_query_key_value(q, "q", v, sizeof(v)) == ESP_OK) {
            cfg.jpeg_quality = atoi(v);
        }
        if (httpd_query_key_value(q, "psram", v, sizeof(v)) == ESP_OK) {
            cfg.psram_dma = (strcmp(v, "0") == 0) ? 0 : 1;
        }
        if (httpd_query_key_value(q, "size", v, sizeof(v)) == ESP_OK) {
            if (strcmp(v, "qqvga") == 0) {
                cfg.frame_size = FRAMESIZE_QQVGA;
            } else if (strcmp(v, "vga") == 0) {
                cfg.frame_size = FRAMESIZE_VGA;
            } else if (strcmp(v, "qvga") == 0) {
                cfg.frame_size = FRAMESIZE_QVGA;
            }
        }
    }
    free(q);

    esp_err_t rc = camera_api_apply(&cfg);

    sb_t s;
    if (!sb_init(&s, 384)) {
        return httpd_resp_send_500(req);
    }
    camera_state_t cs;
    camera_api_get_state(&cs);
    sb_raw(&s, "{");
    sb_kv_str(&s, "ok", rc == ESP_OK ? "true" : "false");
    sb_raw(&s, ",");
    sb_kv_str(&s, "err", rc == ESP_OK ? "" : esp_err_to_name(rc)); sb_raw(&s, ",");
    sb_kv_str(&s, "sensor", cs.sensor); sb_raw(&s, ",");
    sb_kv_str(&s, "format", cs.format); sb_raw(&s, ",");
    sb_kv_str(&s, "fb_loc", cs.fb_loc); sb_raw(&s, ",");
    sb_kv_i(&s, "fb_count", cs.fb_count); sb_raw(&s, ",");
    sb_kv_str(&s, "grab", cs.grab); sb_raw(&s, ",");
    sb_kv_i(&s, "xclk_mhz", cs.xclk_mhz); sb_raw(&s, ",");
    sb_kv_i(&s, "psram_dma", cs.psram_dma); sb_raw(&s, ",");
    sb_kv_i(&s, "quality", cs.quality);
    sb_raw(&s, "}");
    return send_json(req, &s);
}

/* POST /api/camera/snapshot
 *
 * 本地验证面：抓一帧并**就地**编码为 JPEG 直接回给客户端（**不落盘**）。
 * 用途：面板「预览」的回落通道 —— 主验证面按 ~1–2 fps 轮询本路由即可得到近实时画面，
 * 不依赖 MJPEG 流服务（端口 81）是否启动成功；也不给 SD 卡制造写放大。 */
static esp_err_t h_camera_snapshot(httpd_req_t *req)
{
    if (!camera_api_ready()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "camera 未初始化（SCCB 未探测到 sensor？）");
    }
    uint8_t *jpg = NULL;
    size_t len = 0;
    int w = 0, h = 0;
    if (camera_api_grab_jpeg(&jpg, &len, &w, &h) != ESP_OK || jpg == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "抓帧失败（fb 为空或编码失败；见串口 cam_hal）");
    }
    esp_err_t rc = httpd_resp_set_type(req, "image/jpeg");
    if (rc == ESP_OK) {
        rc = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }
    if (rc == ESP_OK) {
        rc = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    }
    if (rc == ESP_OK) {
        rc = httpd_resp_send(req, (const char *)jpg, len);
    }
    free(jpg);
    return rc;
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
    cfg.max_uri_handlers = 17;          /* 9 个 /api（keys/simulate/key/lcd/draw/camera/capture/camera/reinit/camera/snapshot）+ /media/list + 媒体通配 + /api/action */
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
        { .uri = "/api/simulate/key", .method = HTTP_POST, .handler = h_simulate_key },
        { .uri = "/api/lcd/draw",     .method = HTTP_POST, .handler = h_lcd_draw },
        { .uri = "/api/camera/capture", .method = HTTP_POST, .handler = h_camera_capture },
        { .uri = "/api/camera/reinit",  .method = HTTP_POST, .handler = h_camera_reinit },
        { .uri = "/api/camera/snapshot", .method = HTTP_POST, .handler = h_camera_snapshot },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        rc = httpd_register_uri_handler(s_httpd, &uris[i]);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败：%s", uris[i].uri, esp_err_to_name(rc));
        }
    }
    ESP_LOGI(TAG, "本地验证面已启动：http://<设备IP>:%u/api/{ping,status,selftest,keys,simulate/key}", (unsigned)port);
    /* 媒体与动作（/media/list、/media/<alias>/<path>、POST /api/action）注册到同一实例 */
    (void)media_api_register(s_httpd);

    /* 预览面：MJPEG 流跑在**独立 httpd 实例**（端口 81）——流是长连接，跑在主实例上会把
     * /api/status 等轮询请求全堵住（面板会显示"面板服务不可达"）。启动失败不视为错误：
     * 面板会自动回落到 POST /api/camera/snapshot 的单帧轮询预览。 */
    if (camera_api_ready()) {
        esp_err_t src = camera_api_stream_start(0);
        if (src != ESP_OK) {
            ESP_LOGW(TAG, "MJPEG 预览流未启动（%s）：面板将用单帧轮询预览", esp_err_to_name(src));
        }
    }
    ESP_LOGW(TAG, "提示：该 API 仅为台面/联调验证面，不是云端设备面契约；量产应置 CONFIG_ONEYE_FW_ENABLE_PANEL_API=n");
    return ESP_OK;
}

bool panel_api_is_running(void)
{
    return s_httpd != NULL;
}
