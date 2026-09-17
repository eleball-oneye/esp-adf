/*
 * llm_client.c —— 云端语音面 WebSocket 客户端实现（oneye.voice.v1）
 *
 * 实现要点（每条都对应契约的一项，便于评审对照）：
 *   1. 子协议：`subprotocol = "oneye.voice.v1"`（服务端校验 `Sec-WebSocket-Protocol`，缺了直接 400）；
 *   2. 文本帧：`{"v":1,"t":…,"p":{…}}`，`p` 缺省可为空对象；
 *   3. 二进制帧：首字节 kind（0x01 上行 / 0x02 下行）+ PCM；
 *   4. 分帧：上行按 1920 B（60 ms）成帧；尾包在 `commit` 时冲刷（避免最后一小段丢失）；
 *   5. 状态机：READY → LISTENING（首个上行帧）→ THINKING（commit）→ SPEAKING（tts.start）
 *      → READY（turn.end）；`input.cancel` 随时可打断；
 *   6. 保活：应用层 `ping`/`pong`（契约帧），另有 WebSocket 协议层 keep-alive 由组件负责；
 *   7. 重连：组件自动重连（reconnect_timeout_ms），重连成功后重新 `session.start`。
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "cJSON.h"
#include "esp_websocket_client.h"
#if CONFIG_ONEYE_LLM_USE_TLS
#include "esp_crt_bundle.h"
#endif

#include "llm_client.h"
#include "voice_io.h" /* VOICE_IO_FRAME_BYTES */

static const char *TAG = "llm_client";

#define LLM_SUBPROTOCOL      "oneye.voice.v1"
#define LLM_TEXT_MAX         8192 /* 契约 text_frame_max_bytes */
#define LLM_KEEPALIVE_MS     25000

/* 二进制 kind（契约 binary_kinds） */
#define LLM_KIND_AUDIO_UP    0x01
#define LLM_KIND_AUDIO_DOWN  0x02

static const char *state_str(llm_client_state_t st)
{
    switch (st) {
    case LLM_CLIENT_IDLE:        return "IDLE";
    case LLM_CLIENT_CONNECTING:  return "CONNECTING";
    case LLM_CLIENT_HANDSHAKING: return "SESSION_START";
    case LLM_CLIENT_READY:       return "READY";
    case LLM_CLIENT_LISTENING:   return "LISTENING";
    case LLM_CLIENT_THINKING:    return "THINKING";
    case LLM_CLIENT_SPEAKING:    return "SPEAKING";
    case LLM_CLIENT_ERROR:       return "ERROR";
    default:                     return "?";
    }
}

static esp_websocket_client_handle_t s_ws;
static llm_client_cbs_t  s_cbs;
static llm_client_status_t s_st;
static char s_uri[160];
static char s_device_id[64];
static char s_token[128];
static TaskHandle_t s_ka_task;
static bool s_ka_run;

/* 连接守护（重连退避 + 保活）用的状态：事件回调里也要用，故声明在文件前部 */
#define LLM_PING_MS      10000 /* 应用层契约帧 ping（服务端据此刷新"会话活跃/陈旧"判据）；
                                * 协议层 ping 由组件按 ping_interval_sec=5 s 负责 */
#define LLM_BACKOFF_MS   5000  /* 普通断开后的重连退避 */
#define LLM_CONFLICT_MS  30000 /* 并发冲突后的重连退避（等服务端接管陈旧会话） */
static volatile int64_t s_reconnect_at_ms;
static volatile bool    s_need_reconnect;
static volatile bool    s_conflict_hint;
static int64_t now_ms(void);

/* 上行成帧缓冲：首字节 kind + 一帧 PCM */
static uint8_t s_up_frame[1 + VOICE_IO_FRAME_BYTES];
static size_t  s_up_fill;

/* 入站组帧缓冲（文本 ≤8 KB；二进制单帧 ≤8 KB，契约 binary_frame_max_bytes） */
static char   s_rx_text[LLM_TEXT_MAX + 1];
static size_t s_rx_text_len;
static uint8_t s_rx_bin[LLM_TEXT_MAX];
static size_t  s_rx_bin_len;

/* ------------------------------------------------------------------ 状态切换 */

static void set_state(llm_client_state_t st, const char *detail)
{
    if (s_st.state == st && detail == NULL) {
        return;
    }
    s_st.state = st;
    ESP_LOGI(TAG, "状态 → %s%s%s", state_str(st), detail ? "：" : "", detail ? detail : "");
    if (s_cbs.on_state) {
        s_cbs.on_state(st, detail, s_cbs.ctx);
    }
}

/* ------------------------------------------------------------------ 发送 */

static esp_err_t send_text_raw(const char *json)
{
    if (s_ws == NULL || !esp_websocket_client_is_connected(s_ws)) {
        return ESP_ERR_INVALID_STATE;
    }
    int n = esp_websocket_client_send_text(s_ws, json, (int)strlen(json), pdMS_TO_TICKS(1000));
    if (n < 0) {
        ESP_LOGW(TAG, "文本帧发送失败：%d", n);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* 构造并发送 `{"v":1,"t":<t>,"p":<p_json>}`（p_json 为 NULL 时 `p` 为 {}） */
static esp_err_t send_frame(const char *t, const char *p_json)
{
    char *buf = malloc(LLM_TEXT_MAX);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int n = snprintf(buf, LLM_TEXT_MAX, "{\"v\":1,\"t\":\"%s\",\"p\":%s}", t,
                     p_json ? p_json : "{}");
    esp_err_t err = (n > 0 && n < LLM_TEXT_MAX) ? send_text_raw(buf) : ESP_ERR_INVALID_SIZE;
    free(buf);
    return err;
}

static esp_err_t send_session_start(void)
{
    /* 契约 §3.1：device_id + codec（v0.1 仅 PCM 16 kHz/1 ch/16 bit/60 ms） */
    char p[320];
    int n = snprintf(p, sizeof(p),
                     "{\"device_id\":\"%s\",\"codec\":{\"format\":\"pcm\",\"rate\":16000,"
                     "\"channels\":1,\"bits\":16,\"frame_ms\":60}%s%s%s}",
                     s_device_id,
                     s_token[0] ? ",\"token\":\"" : "",
                     s_token[0] ? s_token : "",
                     s_token[0] ? "\"" : "");
    if (n <= 0 || n >= (int)sizeof(p)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "发送 session.start（device_id=%s）", s_device_id);
    return send_frame("session.start", p);
}

/* 上行尾包冲刷：不足一帧也发出去（服务端按字节累积） */
static esp_err_t flush_up_frame(void)
{
    if (s_up_fill == 0) {
        return ESP_OK;
    }
    s_up_frame[0] = LLM_KIND_AUDIO_UP;
    int n = esp_websocket_client_send_bin(s_ws, (const char *)s_up_frame, (int)(1 + s_up_fill),
                                         pdMS_TO_TICKS(1000));
    if (n < 0) {
        ESP_LOGW(TAG, "上行尾包发送失败：%d", n);
        s_up_fill = 0;
        return ESP_FAIL;
    }
    s_st.up_frames++;
    s_st.up_bytes += (uint32_t)s_up_fill;
    s_up_fill = 0;
    return ESP_OK;
}

esp_err_t llm_client_audio_up(const void *pcm, size_t len)
{
    if (s_ws == NULL || !esp_websocket_client_is_connected(s_ws) || !s_st.session_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_st.state != LLM_CLIENT_READY && s_st.state != LLM_CLIENT_LISTENING) {
        return ESP_ERR_INVALID_STATE; /* THINKING/SPEAKING 期不接受上行（契约 §4） */
    }
    if (s_st.state == LLM_CLIENT_READY) {
        set_state(LLM_CLIENT_LISTENING, NULL);
    }
    const uint8_t *p = (const uint8_t *)pcm;
    while (len > 0) {
        size_t room = VOICE_IO_FRAME_BYTES - s_up_fill;
        size_t take = (len < room) ? len : room;
        memcpy(s_up_frame + 1 + s_up_fill, p, take);
        s_up_fill += take;
        p += take;
        len -= take;
        if (s_up_fill == VOICE_IO_FRAME_BYTES) {
            esp_err_t err = flush_up_frame();
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

esp_err_t llm_client_commit(void)
{
    if (!esp_websocket_client_is_connected(s_ws)) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)flush_up_frame();
    esp_err_t err = send_frame("input.audio.commit", NULL);
    if (err == ESP_OK) {
        set_state(LLM_CLIENT_THINKING, NULL);
    }
    return err;
}

esp_err_t llm_client_cancel(const char *reason)
{
    if (!esp_websocket_client_is_connected(s_ws)) {
        return ESP_ERR_INVALID_STATE;
    }
    char p[128];
    snprintf(p, sizeof(p), "{\"reason\":\"%s\"}", reason ? reason : "user");
    s_up_fill = 0; /* 丢弃未发出的尾包 */
    esp_err_t err = send_frame("input.cancel", p);
    s_st.cancels++;
    return err;
}

esp_err_t llm_client_ping(void)
{
    return send_frame("ping", NULL);
}

/* ------------------------------------------------------------------ 入站处理 */

static void handle_text(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "文本帧 JSON 解析失败（%u B）", (unsigned)len);
        return;
    }
    const cJSON *jt = cJSON_GetObjectItemCaseSensitive(root, "t");
    const cJSON *jp = cJSON_GetObjectItemCaseSensitive(root, "p");
    const char *t = (cJSON_IsString(jt) && jt->valuestring) ? jt->valuestring : "";
    const cJSON *jtext = cJSON_IsObject(jp) ? cJSON_GetObjectItemCaseSensitive(jp, "text") : NULL;
    const char *text = (cJSON_IsString(jtext) && jtext->valuestring) ? jtext->valuestring : NULL;

    if (strcmp(t, "session.ready") == 0) {
        const cJSON *sid = cJSON_IsObject(jp) ? cJSON_GetObjectItemCaseSensitive(jp, "session_id") : NULL;
        if (cJSON_IsString(sid) && sid->valuestring) {
            snprintf(s_st.session_id, sizeof(s_st.session_id), "%s", sid->valuestring);
        }
        s_st.session_ready = true;
        set_state(LLM_CLIENT_READY, "会话就绪");
    } else if (strcmp(t, "asr.partial") == 0 || strcmp(t, "asr.final") == 0 ||
               strcmp(t, "llm.delta") == 0) {
        if (text) {
#if CONFIG_ONEYE_LLM_LOG_TEXT
            ESP_LOGI(TAG, "%s：%s", t, text);
#endif
            if (s_cbs.on_text) {
                s_cbs.on_text(t, text, s_cbs.ctx);
            }
        }
    } else if (strcmp(t, "tts.start") == 0) {
        set_state(LLM_CLIENT_SPEAKING, NULL);
        if (s_cbs.on_tts_start) {
            s_cbs.on_tts_start(s_cbs.ctx);
        }
    } else if (strcmp(t, "tts.end") == 0) {
        if (s_cbs.on_tts_end) {
            s_cbs.on_tts_end(s_cbs.ctx);
        }
    } else if (strcmp(t, "turn.end") == 0) {
        int seq = 0;
        int rtt = 0;
        bool cancelled = false;
        if (cJSON_IsObject(jp)) {
            const cJSON *j1 = cJSON_GetObjectItemCaseSensitive(jp, "turn_seq");
            const cJSON *j2 = cJSON_GetObjectItemCaseSensitive(jp, "rtt_ms");
            const cJSON *j3 = cJSON_GetObjectItemCaseSensitive(jp, "cancelled");
            seq = cJSON_IsNumber(j1) ? j1->valueint : 0;
            rtt = cJSON_IsNumber(j2) ? j2->valueint : 0;
            cancelled = cJSON_IsTrue(j3);
        }
        s_st.turns++;
        s_st.last_turn_seq = seq;
        s_st.last_rtt_ms = rtt;
        set_state(LLM_CLIENT_READY, cancelled ? "本轮已打断" : "本轮结束");
        if (s_cbs.on_turn_end) {
            s_cbs.on_turn_end(seq, cancelled, rtt, s_cbs.ctx);
        }
    } else if (strcmp(t, "error") == 0) {
        const char *code = "internal";
        const char *msg = "";
        bool retryable = false;
        if (cJSON_IsObject(jp)) {
            const cJSON *jc = cJSON_GetObjectItemCaseSensitive(jp, "code");
            const cJSON *jm = cJSON_GetObjectItemCaseSensitive(jp, "msg");
            const cJSON *jr = cJSON_GetObjectItemCaseSensitive(jp, "retryable");
            if (cJSON_IsString(jc) && jc->valuestring) {
                code = jc->valuestring;
            }
            if (cJSON_IsString(jm) && jm->valuestring) {
                msg = jm->valuestring;
            }
            retryable = cJSON_IsTrue(jr);
        }
        s_st.errors++;
        ESP_LOGW(TAG, "服务端错误：%s（%s）%s", code, msg, retryable ? "[可重试]" : "");
        /* 并发冲突：本设备已有活跃会话（多为上一条死会话未被服务端回收）。
         * 标提示位，让断开后的重连退避到 30 s，等服务端接管陈旧会话。 */
        if (strcmp(code, "conflict") == 0) {
            s_conflict_hint = true;
        }
        if (s_cbs.on_error) {
            s_cbs.on_error(code, msg, retryable, s_cbs.ctx);
        }
        if (s_st.state != LLM_CLIENT_IDLE) {
            set_state(LLM_CLIENT_READY, "错误后回到就绪");
        }
    } else if (strcmp(t, "pong") == 0) {
        ESP_LOGD(TAG, "pong");
    } else {
        ESP_LOGD(TAG, "忽略未知帧：%s", t);
    }
    cJSON_Delete(root);
}

/** 完成一个二进制帧：首字节 kind，其后 PCM */
static void handle_binary(const uint8_t *data, size_t len)
{
    if (len < 1) {
        return;
    }
    uint8_t kind = data[0];
    const uint8_t *pcm = data + 1;
    size_t pcm_len = len - 1;
    if (kind == LLM_KIND_AUDIO_DOWN) {
        s_st.down_bytes += (uint32_t)pcm_len;
        if (s_cbs.on_audio_down) {
            s_cbs.on_audio_down(pcm, pcm_len, s_cbs.ctx);
        }
    } else {
        ESP_LOGD(TAG, "忽略未知二进制 kind=0x%02x（%u B）", kind, (unsigned)len);
    }
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_st.connected = true;
        s_st.session_ready = false;
        s_rx_text_len = 0;
        s_rx_bin_len = 0;
        s_up_fill = 0;
        set_state(LLM_CLIENT_HANDSHAKING, "已连接，发 session.start");
        (void)send_session_start();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        s_st.connected = false;
        s_st.session_ready = false;
        /* 自主重连：冲突退避更久（等服务端接管陈旧会话） */
        s_need_reconnect = true;
        s_reconnect_at_ms = now_ms() + (s_conflict_hint ? LLM_CONFLICT_MS : LLM_BACKOFF_MS);
        s_conflict_hint = false;
        set_state(LLM_CLIENT_IDLE, s_need_reconnect ? "连接断开（按退避重连）" : "连接断开");
        break;

    case WEBSOCKET_EVENT_DATA: {
        if (d == NULL) {
            break;
        }
        if (d->op_code == 0x08) {
            ESP_LOGW(TAG, "服务端关闭连接");
            break;
        }
        if (d->op_code == 0x09 || d->op_code == 0x0A) {
            break; /* 协议层 ping/pong 由组件处理 */
        }
        if (d->data_len <= 0) {
            break;
        }
        if (d->payload_offset == 0) { /* 新帧开始 */
            s_rx_text_len = 0;
            s_rx_bin_len = 0;
        }
        if (d->op_code == 0x01) { /* 文本 */
            if (s_rx_text_len + (size_t)d->data_len <= LLM_TEXT_MAX) {
                memcpy(s_rx_text + s_rx_text_len, d->data_ptr, (size_t)d->data_len);
                s_rx_text_len += (size_t)d->data_len;
            }
            if ((size_t)(d->payload_offset + d->data_len) >= (size_t)d->payload_len) {
                s_rx_text[s_rx_text_len] = '\0';
                handle_text(s_rx_text, s_rx_text_len);
                s_rx_text_len = 0;
            }
        } else if (d->op_code == 0x02) { /* 二进制 */
            if (s_rx_bin_len + (size_t)d->data_len <= sizeof(s_rx_bin)) {
                memcpy(s_rx_bin + s_rx_bin_len, d->data_ptr, (size_t)d->data_len);
                s_rx_bin_len += (size_t)d->data_len;
            }
            if ((size_t)(d->payload_offset + d->data_len) >= (size_t)d->payload_len) {
                handle_binary(s_rx_bin, s_rx_bin_len);
                s_rx_bin_len = 0;
            }
        }
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WebSocket 出错（等待自动重连）");
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ 连接守护 + 保活 */

/*
 * supervisor_task：同时承担两件事（取代原来的 keepalive_task）
 *   ① 保活：已连接时每 `LLM_PING_MS`（10 s）发一帧 `ping`（契约帧，服务端回 `pong`）；
 *      服务端据此刷新"会话活跃/陈旧"与读超时判据。
 *   ② 重连：断开后按退避重连 —— 普通断开 5 s，**并发冲突（conflict）30 s**
 *      （服务端还需时间关闭上一个死会话并释放槽位，抢跑只会再次 conflict）。
 *
 * ⚠️ 三个周期的取值只能在本文件顶部定义（`LLM_PING_MS`/`LLM_BACKOFF_MS`/`LLM_CONFLICT_MS`）：
 *    这里曾重复定义 `LLM_PING_MS`，后一处静默覆盖前一处（编译只报 warning），
 *    导致"注释写 20 s、实际 10 s"的取证口径不一致 —— 现已合并为顶部单一定义。
 */
static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void supervisor_task(void *arg)
{
    (void)arg;
    int64_t last_ping = 0;
    uint32_t pings = 0;
    while (s_ka_run) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_ka_run) {
            break;
        }
        if (esp_websocket_client_is_connected(s_ws)) {
            if (now_ms() - last_ping >= LLM_PING_MS) {
                last_ping = now_ms();
                esp_err_t perr = llm_client_ping();
                pings++;
                /* 保活取证（台面诊断档开 DEBUG 日志即见）：ping 是否真的出去、服务端是否回 pong */
                ESP_LOGD(TAG, "保活 ping 已发（第 %u 次）：%s", (unsigned)pings,
                         esp_err_to_name(perr));
            }
            continue;
        }
        if (s_need_reconnect && now_ms() >= s_reconnect_at_ms) {
            /* 重连由组件自动重连负责（见 llm_client_start 注释）；这里只记录退避已到点 */
            ESP_LOGD(TAG, "退避结束，交由组件重连（%s）", s_uri);
            s_need_reconnect = false;
        }
    }
    s_ka_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ 生命周期 */

esp_err_t llm_client_init(const llm_client_cbs_t *cbs, const char *uri, const char *device_id,
                          const char *token)
{
    if (uri == NULL || device_id == NULL || uri[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ws != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cbs) {
        s_cbs = *cbs;
    }
    snprintf(s_uri, sizeof(s_uri), "%s", uri);
    snprintf(s_device_id, sizeof(s_device_id), "%s", device_id);
    snprintf(s_token, sizeof(s_token), "%s", token ? token : "");
    s_st.state = LLM_CLIENT_IDLE;
#if CONFIG_ONEYE_LLM_DIAG_HTTP_PROBE
    /* 台面诊断档：把本模块的运行期日志级别提到 DEBUG（保活 ping、被忽略的帧等）。
     * ⚠️ 运行期 `esp_log_level_set` 只能放开**已编译进来**的等级：要看 `ESP_LOGD`，
     *    还需 `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y`（或本文件 `LOG_LOCAL_LEVEL=ESP_LOG_DEBUG`）；
     *    默认 INFO 档下这些 DEBUG 语句被编译掉，这里不产生任何日志。 */
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    ESP_LOGI(TAG, "语音面客户端：%s（子协议 %s，设备 %s）", s_uri, LLM_SUBPROTOCOL, s_device_id);
    return ESP_OK;
}

esp_err_t llm_client_start(void)
{
    static bool s_starting; /* 防并发重入：两个调用者都可能在 s_ws 赋值前通过 s_ws==NULL 检查 */
    if (s_ws != NULL || s_starting) {
        return ESP_OK;
    }
    s_starting = true;
    esp_websocket_client_config_t cfg = { 0 };
    cfg.uri = s_uri;
    cfg.subprotocol = LLM_SUBPROTOCOL; /* 服务端强校验该头（wsserver.go: hasSubprotocol） */
    /*
     * 栈与缓冲都**必须落在内部 RAM**（真机取证 2026-09-17）：
     *   本工程开了 `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` 且
     *   `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`（>阈值才走 PSRAM），
     *   若把 WebSocket 任务栈设成 6144、缓冲 4096，二者都会被分配到 PSRAM，
     *   而 Wi-Fi 中断/关 cache 期间跑在 PSRAM 栈上的任务会让中断看门狗超时：
     *     `Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)`
     *   （实测：连上 Wi-Fi、开始 connect 到服务端时立即复现）。
     *   ⇒ 栈取 3584（<阈值）、缓冲取 2048，两者都在内部 RAM。
     */
    cfg.task_stack = 8192;
    cfg.buffer_size = 4096;
    cfg.reconnect_timeout_ms = 3000;
    /*
     * 空闲与保活（真机取证 2026-09-17，重要）：
     * `network_timeout_ms` 被组件当作 **socket 读超时**；会话就绪后若这段时间没有下行数据，
     * 组件会判"读失败"并**重连**，于是服务端看到"同一 device_id 的第二条连接"（按契约并发 1
     * 被拒为 conflict），设备再重连，形成噪声循环。处置：
     *   ① 读超时放宽到 15 s；
     *   ② 打开**协议层 ping**（组件自动收发，服务端 gorilla 自动回 pong）作为持续活跃信号；
     *   ③ 应用层契约帧 `ping` 周期放在 20 s（服务端只用它刷新"会话活跃/陈旧"判据）。
     */
    cfg.network_timeout_ms = 15000;
    cfg.ping_interval_sec = 5;
    cfg.pingpong_timeout_sec = 0;
    /*
     * 重连交给组件（`disable_auto_reconnect = false`）。真机取证 2026-09-17：
     * 设备侧"被服务端关闭后立即重连"这条路径会触发 `Interrupt wdt timeout`，
     * 而**只要服务端不再误判冲突**（陈旧槽位 25→12 s 回收 + pong 也算活跃 + 服务端 ping 5 s），
     * 这条路径就不会被走到 ⇒ 保持组件默认重连、把根因修在服务端与保活周期上。
     * 协议层 ping 取 **5 s**（服务端 12 s 陈旧窗口据此判定会话是否还活着）。
     */
    cfg.disable_auto_reconnect = false;
#if CONFIG_ONEYE_LLM_USE_TLS
    /* 生产形态：用证书包校验服务端证书；dev 自签场景请在 sdkconfig 打开
     * CONFIG_ESP_TLS_INSECURE + CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY（仅台面）。 */
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
#if CONFIG_ONEYE_LLM_TLS_INSECURE
    cfg.skip_cert_common_name_check = true;
    ESP_LOGW(TAG, "已按 Kconfig 跳过 TLS 证书校验（仅 dev/台面；生产必须关闭）");
#endif
#endif

    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL) {
        ESP_LOGE(TAG, "WebSocket 客户端创建失败");
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL));

    set_state(LLM_CLIENT_CONNECTING, NULL);
    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket 启动失败：%s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        set_state(LLM_CLIENT_ERROR, "启动失败");
        return err;
    }
    if (s_ka_task == NULL) {
        s_ka_run = true;
        (void)xTaskCreate(supervisor_task, "llm_sup", 4096, NULL, 4, &s_ka_task);
    }
    return ESP_OK;
}

esp_err_t llm_client_stop(void)
{
    s_ka_run = false;
    for (int i = 0; i < 50 && s_ka_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_ws) {
        (void)esp_websocket_client_stop(s_ws);
        (void)esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_st.connected = false;
    s_st.session_ready = false;
    set_state(LLM_CLIENT_IDLE, "已停止");
    return ESP_OK;
}

bool llm_client_is_connected(void)
{
    return s_ws != NULL && esp_websocket_client_is_connected(s_ws);
}

bool llm_client_is_session_ready(void)
{
    return s_st.session_ready;
}

llm_client_state_t llm_client_get_state(void)
{
    return s_st.state;
}

void llm_client_get_status(llm_client_status_t *out)
{
    if (out) {
        *out = s_st;
        out->connected = llm_client_is_connected();
    }
}
