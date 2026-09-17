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
static void selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    panel_min_note("[selftest] 自动收音 %d ms（台面自检，非按键路径）",
                   CONFIG_ONEYE_LLM_SELFTEST_TURN_MS);
    if (voice_io_capture_start(pcm_uplink_cb, NULL) != ESP_OK) {
        panel_min_note("[selftest] 采集启动失败");
        vTaskDelete(NULL);
        return;
    }
    s_turn_start_us = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(CONFIG_ONEYE_LLM_SELFTEST_TURN_MS));
    (void)voice_io_capture_stop();
    panel_min_note("[selftest] 自动提交本轮（上行 %u B）", (unsigned)voice_io_captured_bytes());
    (void)llm_client_commit();
    vTaskDelete(NULL);
}
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
    /* 事件任务上下文：只投递任务 */
    (void)xTaskCreate(net_ready_task, "net_ready", 6144, NULL, 5, NULL);
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
