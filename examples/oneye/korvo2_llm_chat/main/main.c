/*
 * main.c —— korvo2_llm_chat 例程主流程
 *
 * 闭环（用户需求）：
 *   ① 上电 → 配网（凭据文件 / BLE 自研 GATT / SmartConfig，先成功者胜出）
 *   ② 联网后连接云端语音小服务 chatd（`oneye.voice.v1`）并发 `session.start`
 *   ③ **长按 REC ≥ 600 ms** → 开始收音（AFE：AEC + 降噪）→ 60 ms/帧二进制上行（kind 0x01）
 *   ④ 松开 → `input.audio.commit` → 服务端 ASR → LLM → TTS
 *   ⑤ 下行 PCM（kind 0x02）经喇叭播放；播放中再长按 = `input.cancel` 打断（≤200 ms 停止下行）
 *
 * 分层（本工程的文件边界）：
 *   main.c        编排（状态机 + 互斥 + 兜底）
 *   voice_io      音频出入口（I2S/AFE/PCM，唯一碰 ADF 音频栈的地方）
 *   llm_client    云端语音面协议（唯一碰 WebSocket 的地方）
 *   key_talk      按键语义（长按/短按）
 *   prov_service  Wi-Fi 配网（文件/BLE/SmartConfig）与本地面节点/组域
 *   lan_link      本地面 lan 信道数据面（UDP 发现 + HTTP 帧面）
 *   panel_min     串口状态呈现（非契约）
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"

#include "board.h"
#include "audio_hal.h"
#include "esp_peripherals.h"

#include "voice_io.h"
#include "llm_client.h"
#include "key_talk.h"
#include "prov_service.h"
#include "panel_min.h"

static const char *TAG = "llm_chat";

static esp_periph_set_handle_t s_periph_set;
static audio_board_handle_t    s_board;
static int64_t                 s_turn_start_us;
static bool                    s_gate_ready; /* 语音面就绪（可开始新一轮） */
/* SET 键在"会话未就绪"时被按下的待生效标记（见 on_new_conversation 与 on_llm_state）。 */
static volatile bool           s_new_conv_pending;

/* 采集回调（前置声明：定义在文件后部） */
static void pcm_uplink_cb(const void *pcm, size_t len, void *ctx);

/* ------------------------------------------------------------------ 台面自检（可选） */

/*
 * `ONEYE_LLM_SELFTEST_TURN_MS > 0` 时，语音面就绪后自动走一轮完整话轮
 * （等价于「按住 REC 该时长后松开」），用于无人值守验证整条链路：
 *   上行采集 → 服务端 ASR/LLM/TTS → 下行 PCM → 喇叭回放 → turn.end。
 * 走的是与按键**完全相同**的代码路径（voice_io + llm_client），不新增任何协议/端点。
 */
#if CONFIG_ONEYE_LLM_SELFTEST_TURN_MS > 0
/* selftest_run_turn 走一轮：采集固定时长 → 提交（与按键路径**完全相同**的代码，只是触发源不同）。
 * 抽成函数是因为 `SELFTEST_TURN_AFTER_NEWCONV` 要在新对话里再走一轮。 */
static void selftest_run_turn(const char *why)
{
    panel_min_note("[selftest] 自动收音 %d ms（台面自检，非按键路径）：%s",
                   CONFIG_ONEYE_LLM_SELFTEST_TURN_MS, why);
    /* 内存取证（BLE 配网打开后内部 RAM 明显变紧，音频管线会被挤掉）：
     * 打开 BLE 后实测过 `E voice_io: I2S 读元素创建失败` → 采集启动失败，
     * 因此每次采集前把"内部 RAM / PSRAM 余量"打出来，便于判定要腾哪一侧。 */
    panel_min_note("[selftest] 采集前内存：内部余 %u B / 总余 %u B / 最大块 %u B",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)esp_get_free_heap_size(),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (voice_io_capture_start(pcm_uplink_cb, NULL) != ESP_OK) {
        panel_min_note("[selftest] 采集启动失败");
        return;
    }
    s_turn_start_us = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(CONFIG_ONEYE_LLM_SELFTEST_TURN_MS));
    (void)voice_io_capture_stop();
    panel_min_note("[selftest] 自动提交本轮（上行 %u B）", (unsigned)voice_io_captured_bytes());
    (void)llm_client_commit();
}

static void selftest_task(void *arg)
{
    (void)arg;
    /* 等 20 s 再自检（原先 3 s）：真机取证 2026-09-17 发现 3 s 时正撞上
     * "AFE 模型加载 + BLE 广播 + WS 建连"三者叠加的内存峰值，
     * `voice_io` 的 4 KB 采集任务栈申请被拒（`采集任务创建失败`，当时内部余 44,111 B / 最大块 23,552 B），
     * 自检直接失败且不重试。延后到配网与模型都稳定后再跑，采集可正常创建。
     * 这只影响**台面自检**的时间点，不影响按键路径。 */
    vTaskDelay(pdMS_TO_TICKS(20000));
    selftest_run_turn("开机首轮");
    vTaskDelete(NULL);
}

/*
 * `ONEYE_LLM_SELFTEST_TURN_AFTER_NEWCONV=1`：新对话建立后再自动走一轮，
 * 用于**无人值守**验证"起新对话后在新对话里继续"（P3 验收项之一：
 * 新段应 turns 从 0 变 1、且**不继承**旧段上下文）。
 */
#if CONFIG_ONEYE_LLM_SELFTEST_TURN_AFTER_NEWCONV
static void selftest_turn_in_newconv_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500)); /* 等服务端把新段落定 */
    selftest_run_turn("新对话内的第二轮");
    vTaskDelete(NULL);
}
#endif

/*
 * `ONEYE_LLM_SELFTEST_NEW_CONV=1` 时，第 1 轮结束后自动发一次 `conv.new`
 * （等价 SET 键单击），用于无人值守验证"起新对话 + 在新对话里继续"：
 * 服务端应回 `conv.state{reason:"new"}` 且 conv_id 变化、turns 归零。
 */
#if CONFIG_ONEYE_LLM_SELFTEST_NEW_CONV
static void selftest_newconv_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000)); /* 等第 1 轮彻底收尾（turn.end 之后） */
    panel_min_note("[selftest] 模拟 SET 单击：请求开新对话");
    esp_err_t err = llm_client_new_conversation();
    if (err != ESP_OK) {
        panel_min_note("[selftest] 起新对话失败：%s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}
#endif
#endif

/* ------------------------------------------------------------------ 语音面回调 */

static void on_llm_state(llm_client_state_t st, const char *detail, void *ctx)
{
    (void)ctx;
    const char *name = "?";
    switch (st) {
    case LLM_CLIENT_IDLE:        name = "IDLE"; break;
    case LLM_CLIENT_CONNECTING:  name = "CONNECTING"; break;
    case LLM_CLIENT_HANDSHAKING: name = "SESSION_START"; break;
    case LLM_CLIENT_READY:       name = "READY"; break;
    case LLM_CLIENT_LISTENING:   name = "LISTENING"; break;
    case LLM_CLIENT_THINKING:    name = "THINKING"; break;
    case LLM_CLIENT_SPEAKING:    name = "SPEAKING"; break;
    case LLM_CLIENT_ERROR:       name = "ERROR"; break;
    }
    s_gate_ready = (st == LLM_CLIENT_READY);
    panel_min_state(name, detail);
    /* 会话就绪后补发"就绪前按下的 SET"（见 on_new_conversation 的说明）。 */
    if (st == LLM_CLIENT_READY && s_new_conv_pending) {
        s_new_conv_pending = false;
        ESP_LOGI(TAG, "会话已就绪 → 补发先前按下的 SET（conv.new）");
        (void)llm_client_new_conversation();
    }
#if CONFIG_ONEYE_LLM_SELFTEST_TURN_MS > 0
    static bool selftest_done;
    if (st == LLM_CLIENT_READY && !selftest_done) {
        selftest_done = true;
        (void)xTaskCreate(selftest_task, "selftest", 6144, NULL, 4, NULL);
    }
#endif
}

static void on_llm_text(const char *frame_t, const char *text, void *ctx)
{
    (void)ctx;
    /* PIPL：转写/模型文本**不落盘**；是否打串口由 Kconfig 控制（缺省 n） */
#if CONFIG_ONEYE_LLM_LOG_TEXT
    panel_min_note("%s: %s", frame_t, text);
#else
    (void)frame_t;
    (void)text;
#endif
}

static void on_llm_tts_start(void *ctx)
{
    (void)ctx;
    if (voice_io_playback_open() != ESP_OK) {
        panel_min_note("回放打开失败（下行音频将被丢弃）");
    }
}

static void on_llm_audio_down(const void *pcm, size_t len, void *ctx)
{
    (void)ctx;
    if (!voice_io_is_playing()) {
        return;
    }
    (void)voice_io_playback_write(pcm, len);
}

static void on_llm_tts_end(void *ctx)
{
    (void)ctx; /* 等 turn.end 再关闭（最后一帧可能仍在 I2S 缓冲里） */
}

static void on_llm_turn_end(int turn_seq, bool cancelled, int rtt_ms, void *ctx)
{
    (void)ctx;
    (void)voice_io_playback_close();
    panel_min_note("本轮结束：turn_seq=%d rtt=%d ms%s", turn_seq, rtt_ms,
                   cancelled ? "（已打断）" : "");
#if CONFIG_ONEYE_LLM_SELFTEST_TURN_MS > 0 && CONFIG_ONEYE_LLM_SELFTEST_NEW_CONV
    /* 第 1 轮结束后自动模拟一次 SET 单击（无人值守验证新对话语义） */
    static bool newconv_fired;
    if (turn_seq == 1 && !newconv_fired) {
        newconv_fired = true;
        (void)xTaskCreate(selftest_newconv_task, "selftest_conv", 4096, NULL, 4, NULL);
    }
#endif
}

static void on_llm_error(const char *code, const char *msg, bool retryable, void *ctx)
{
    (void)ctx;
    panel_min_note("服务端错误：code=%s msg=%s retryable=%s", code, msg,
                   retryable ? "true" : "false");
}

/* ------------------------------------------------------------------ 按键 → 话轮 */

static void on_press_down(void *ctx)
{
    (void)ctx;
    /* 回放中按下 = 立刻打断（不等长按达标），满足契约「打断 ≤200 ms」 */
    if (llm_client_get_state() == LLM_CLIENT_SPEAKING) {
        panel_min_note("检测到按下：打断当前回放");
        (void)llm_client_cancel("barge_in");
        (void)voice_io_playback_close();
    }
}

static void on_talk_start(void *ctx)
{
    (void)ctx;
    if (!s_gate_ready) {
        panel_min_note("语音面尚未就绪（未连上 chatd 或未 session.ready）——仍尝试上行");
    }
    if (!voice_io_is_capturing() && voice_io_is_playing()) {
        (void)voice_io_playback_close(); /* 共用 I2S0：先把回放腾出来 */
    }
    if (voice_io_capture_start(pcm_uplink_cb, NULL) != ESP_OK) {
        panel_min_note("采集启动失败（见 voice_io 日志）");
        return;
    }
    s_turn_start_us = esp_timer_get_time();
}

static void on_talk_stop(uint32_t held_ms, void *ctx)
{
    (void)ctx;
    (void)voice_io_capture_stop();
    if (held_ms < (uint32_t)CONFIG_ONEYE_LLM_TALK_MIN_PRESS_MS) {
        panel_min_note("按住 %u ms 未达阈值，本轮丢弃", (unsigned)held_ms);
        return;
    }
    if (!llm_client_is_session_ready()) {
        panel_min_note("语音面未就绪（未连上 chatd？），本轮不上传");
        return;
    }
    (void)llm_client_commit();
}

static void on_short_press(void *ctx)
{
    (void)ctx;
    if (llm_client_get_state() == LLM_CLIENT_SPEAKING) {
        (void)llm_client_cancel("short_press");
        (void)voice_io_playback_close();
    }
}

/*
 * SET 键单击 → 起新对话（契约 §12.1 `conv.new`）。
 *
 * 口径（为什么这么处理，真机排查时最容易被问到）：
 *   - 服务端**不会**因为重连就自动开新段（否则 10 段上限几次重连就被垃圾段占满）；
 *     "开新话题"必须由用户显式触发 —— 这就是 SET 键；
 *   - 本轮进行中（THINKING/SPEAKING）不允许切段（服务端回 conflict）：先在本地取消本轮，
 *     再发 conv.new，避免"正在生成的回答该写进哪一段"没有确定答案；
 *   - 结果以服务端 `conv.state{reason:"new"}` 为准（本回调只负责发起，不假定成功）。
 */
static void on_new_conversation(void *ctx)
{
    (void)ctx;
    if (!llm_client_is_session_ready()) {
        /* 真机取证 2026-09-17：开机后 3.4 s 就按 SET（会话在 4.26 s 才就绪）⇒ 原先直接拒绝，
         * 用户侧表现就是"按了没反应"（设备无屏，连提示都看不到）。
         * 现在记住这次意图，待会话就绪后立刻补发（见 on_llm_state 的 READY 分支）。
         * 只记一次（连按多次仍只开一段：一次新对话就是一次新对话）。 */
        s_new_conv_pending = true;
        panel_min_note("SET：语音面尚未就绪，已记住该动作，连上后自动开新对话");
        ESP_LOGI(TAG, "SET 在会话就绪前按下 → 记为待生效（READY 后补发 conv.new）");
        return;
    }
    llm_client_state_t st = llm_client_get_state();
    if (st == LLM_CLIENT_THINKING || st == LLM_CLIENT_SPEAKING) {
        panel_min_note("SET：本轮进行中，先打断再起新对话");
        (void)llm_client_cancel("new_conversation");
        (void)voice_io_playback_close();
        vTaskDelay(pdMS_TO_TICKS(200)); /* 给服务端收尾 turn.end 留一口气 */
    }
    esp_err_t err = llm_client_new_conversation();
    if (err != ESP_OK) {
        panel_min_note("SET：起新对话失败（%s）", esp_err_to_name(err));
        return;
    }
    panel_min_note("SET：已起新对话（等待服务端回执）");
}

/* conv.state：服务端告知"当前在接着哪一段聊、用的哪个模型"（设备无屏，只能靠日志/面板） */
static void on_llm_conv_state(const char *reason, int64_t conv_id, int turns,
                              const char *profile_id, void *ctx)
{
    (void)ctx;
    const char *what = "当前对话";
    if (reason && strcmp(reason, "new") == 0) {
        what = "新对话已建立";
    } else if (reason && strcmp(reason, "switched") == 0) {
        what = "模型已切换";
    }
    panel_min_note("%s：#%lld（已有 %d 轮，模型 %s）", what, (long long)conv_id, turns,
                   (profile_id && profile_id[0]) ? profile_id : "默认");
    ESP_LOGI(TAG, "对话组回执：reason=%s conv_id=%lld turns=%d profile=%s", reason ? reason : "?",
             (long long)conv_id, turns, (profile_id && profile_id[0]) ? profile_id : "(默认)");
#if CONFIG_ONEYE_LLM_SELFTEST_TURN_MS > 0 && CONFIG_ONEYE_LLM_SELFTEST_TURN_AFTER_NEWCONV
    /* 新对话一建立就在**新段内**再走一轮 ⇒ 无人值守验证"起新对话后在新对话里继续"。
     * 只认 reason=="new"（`conv.new` 的回执），不认 ready/switched —— 那两种不是"新话题"。 */
    if (reason != NULL && strcmp(reason, "new") == 0) {
        static bool in_newconv_fired;
        if (!in_newconv_fired) {
            in_newconv_fired = true;
            (void)xTaskCreate(selftest_turn_in_newconv_task, "selftest_t2", 4096, NULL, 4, NULL);
        }
    }
#endif
}

/* ------------------------------------------------------------------ 采集回调（上行） */

/** voice_io → llm_client：采集到的 60 ms PCM 直接进语音面成帧缓冲；未就绪时丢帧并降噪日志 */
static void pcm_uplink_cb(const void *pcm, size_t len, void *ctx)
{
    (void)ctx;
    esp_err_t err = llm_client_audio_up(pcm, len);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "上行丢帧（%u B）：%s", (unsigned)len, esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------ 联网就绪 → 连语音面 */

static void net_ready_task(void *arg)
{
    (void)arg;
    char uri[192];
    /* 等 Wi-Fi/lwIP 在拿到 IP 后稳定下来再起 WebSocket 客户端（真机取证 2026-09-17：
     * 在 IP 事件链上立即建客户端会触发 `Interrupt wdt timeout`）。 */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 诊断（台面，Kconfig 开关）：先用**普通 HTTP** 探一次同一个 host:port 的 /healthz。
     * 目的：把"socket/lwIP 层面通不通"与"WebSocket 组件是否有问题"分开
     * （真机取证 2026-09-17：建 WS 客户端即 `Interrupt wdt timeout on CPU1`）。 */
#if CONFIG_ONEYE_LLM_DIAG_HTTP_PROBE
    {
        char url[160];
        snprintf(url, sizeof(url), "http://%s:%d/healthz", CONFIG_ONEYE_LLM_SERVER_HOST,
                 CONFIG_ONEYE_LLM_SERVER_PORT);
        esp_http_client_config_t hc = { .url = url, .timeout_ms = 5000 };
        esp_http_client_handle_t h = esp_http_client_init(&hc);
        if (h != NULL) {
            esp_err_t e = esp_http_client_perform(h);
            int code = esp_http_client_get_status_code(h);
            panel_min_note("[diag] HTTP GET %s → %s（status=%d）", url, esp_err_to_name(e), code);
            esp_http_client_cleanup(h);
        } else {
            panel_min_note("[diag] HTTP 客户端创建失败");
        }
    }
#endif

    snprintf(uri, sizeof(uri), "%s://%s:%d%s", LLM_CLIENT_URI_SCHEME,
             CONFIG_ONEYE_LLM_SERVER_HOST, CONFIG_ONEYE_LLM_SERVER_PORT,
             CONFIG_ONEYE_LLM_WS_PATH);

    llm_client_cbs_t cbs = {
        .on_state = on_llm_state,
        .on_audio_down = on_llm_audio_down,
        .on_tts_start = on_llm_tts_start,
        .on_tts_end = on_llm_tts_end,
        .on_text = on_llm_text,
        .on_turn_end = on_llm_turn_end,
        .on_error = on_llm_error,
        .on_conv_state = on_llm_conv_state,
        .ctx = NULL,
    };
    if (llm_client_init(&cbs, uri, CONFIG_ONEYE_LLM_DEVICE_ID, CONFIG_ONEYE_LLM_TOKEN) != ESP_OK) {
        panel_min_note("语音面客户端初始化失败");
        vTaskDelete(NULL);
        return;
    }
    if (llm_client_start() != ESP_OK) {
        panel_min_note("语音面连接失败（将按 Kconfig 重连逻辑重试）");
    } else {
        panel_min_note("语音面连接中：%s", uri);
    }
    vTaskDelete(NULL);
}

static void on_net_ready(void)
{
    /* 幂等（真机取证 2026-09-17，关键）：
     * Wi-Fi 在启动早期可能产生**两次 GOT_IP**（STA 自动连接 + 我们显式 connect），
     * 若每次都建任务，就会在 `llm_client_start()` 的 `s_ws == NULL` 竞态下**起两个 WS 客户端**，
     * 结果是服务端看到"同一 device_id 两个会话" → `error{code:"conflict"}` 并关连接。 */
    static bool started;
    if (started) {
        ESP_LOGI(TAG, "网络就绪回调重复触发：忽略（语音面已在启动中）");
        return;
    }
    started = true;
    /* 栈 4096（**保持 ≤ `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`**）：该任务会调用
     * `esp_websocket_client_start()`；若栈落在 PSRAM，遇到关 cache 的窗口会触发
     * `Interrupt wdt timeout`（真机取证 2026-09-17）。 */
    (void)xTaskCreate(net_ready_task, "net_ready", 4096, NULL, 5, NULL);
}

/* ------------------------------------------------------------------ 单轮兜底 */

static void turn_watchdog_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (llm_client_get_state() == LLM_CLIENT_LISTENING && s_turn_start_us > 0) {
            int64_t held_ms = (esp_timer_get_time() - s_turn_start_us) / 1000;
            if (held_ms > CONFIG_ONEYE_LLM_TURN_MAX_MS) {
                panel_min_note("本轮超过上限 %d ms：强制提交（避免服务端 conflict）",
                               CONFIG_ONEYE_LLM_TURN_MAX_MS);
                (void)voice_io_capture_stop();
                (void)llm_client_commit();
                s_turn_start_us = 0;
            }
        }
    }
}

/* ------------------------------------------------------------------ 启动配网任务 */

/*
 * 配网启动放在独立任务里执行（不在 app_main 里直接调）：
 *   ① 启动早期 main 任务栈只有 3.5 KB（本工程已抬到 8 KB，但仍有其他大栈调用方）；
 *   ② 组网/配网涉及 Wi-Fi/BLE/帧组包等多层调用，用独立任务更易定位与限流。
 * ⚠️ 栈大小必须**克制**（真机取证 2026-09-17）：BLE(NimBLE) 真正初始化后内部 RAM 紧张，
 *   曾用 16 KB → `xTaskCreate` 直接失败（串口 `[panel] 配网任务创建失败`，设备停在 BOOT）。
 *   注意：SDK 组帧已改为**堆分配**（见 oneye-dev-sdk 53c337c），调用方不再需要 8 KB 级栈。
 */
static void prov_boot_task(void *arg)
{
    (void)arg;
    (void)prov_service_start();
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ app_main */

void app_main(void)
{
    panel_min_init();

    /* 1) 板级：外设集合 + 音频编解码器 */
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    s_periph_set = esp_periph_set_init(&periph_cfg);
    if (s_periph_set == NULL) {
        ESP_LOGE(TAG, "外设集合创建失败");
        return;
    }
    s_board = audio_board_init();
    if (s_board == NULL) {
        ESP_LOGE(TAG, "audio_board_init 失败（ES8311/ES7210）");
        return;
    }
    ESP_ERROR_CHECK(voice_io_init(s_board->audio_hal));
    (void)voice_io_set_volume(CONFIG_ONEYE_LLM_PLAY_VOLUME);

    /* 2) 本地面 + 配网（含 link 节点/组域、BLE 自研 GATT、SmartConfig、lan 信道） */
    ESP_ERROR_CHECK(prov_service_init(s_periph_set, on_net_ready));

    /* 3) 按键：长按说话 */
    key_talk_cbs_t kcbs = {
        .on_press_down = on_press_down,
        .on_talk_start = on_talk_start,
        .on_talk_stop = on_talk_stop,
        .on_short_press = on_short_press,
        .on_new_conversation = on_new_conversation,
        .ctx = NULL,
    };
    ESP_ERROR_CHECK(key_talk_init(s_periph_set, &kcbs));

    /* 4) 配网启动：文件 → Kconfig → (BLE + SmartConfig)；联网后会回调 on_net_ready 连语音面 */
    if (xTaskCreate(prov_boot_task, "prov_boot", 6144, NULL, 5, NULL) != pdPASS) {
        panel_min_note("配网任务创建失败（内部 RAM 不足？检查 Ble/WiFi/AFE 同时占用）");
    }

    (void)xTaskCreate(turn_watchdog_task, "turn_wd", 3072, NULL, 3, NULL);
    panel_min_note("就绪：长按 REC ≥ %d ms 说话，松开提交；播放中长按 = 打断",
                   CONFIG_ONEYE_LLM_TALK_MIN_PRESS_MS);
}
