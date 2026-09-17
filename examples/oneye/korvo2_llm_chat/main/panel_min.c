/*
 * panel_min.c —— 最小状态面板实现（串口日志）
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "llm_client.h"
#include "voice_io.h"
#include "key_talk.h"
#include "prov_service.h"
#include "panel_min.h"

static const char *TAG = "panel";

static char             s_state[24] = "BOOT";
static char             s_detail[64];
static uint32_t         s_events;
static TaskHandle_t     s_beat_task;

static void heartbeat_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        char json[384];
        panel_min_status_json(json, sizeof(json));
        ESP_LOGI(TAG, "[panel] %s", json);
    }
}

void panel_min_init(void)
{
    ESP_LOGI(TAG, "[panel] ===============================================");
    ESP_LOGI(TAG, "[panel] korvo2_llm_chat —— 长按 REC 说话 → 云端 LLM → 喇叭回放");
    ESP_LOGI(TAG, "[panel] 语音面：%s://%s:%d%s（子协议 oneye.voice.v1）",
             LLM_CLIENT_URI_SCHEME, CONFIG_ONEYE_LLM_SERVER_HOST,
             CONFIG_ONEYE_LLM_SERVER_PORT, CONFIG_ONEYE_LLM_WS_PATH);
    ESP_LOGI(TAG, "[panel] 长按阈值 %d ms；单轮上限 %d ms；回放音量 %d",
             CONFIG_ONEYE_LLM_TALK_MIN_PRESS_MS, CONFIG_ONEYE_LLM_TURN_MAX_MS,
             CONFIG_ONEYE_LLM_PLAY_VOLUME);
    ESP_LOGI(TAG, "[panel] ===============================================");
    if (s_beat_task == NULL) {
        (void)xTaskCreate(heartbeat_task, "panel_beat", 3072, NULL, 3, &s_beat_task);
    }
}

void panel_min_state(const char *state, const char *detail)
{
    s_events++;
    snprintf(s_state, sizeof(s_state), "%s", state ? state : "?");
    snprintf(s_detail, sizeof(s_detail), "%s", detail ? detail : "");
    ESP_LOGI(TAG, "[panel] 状态=%s %s", s_state, s_detail);
}

void panel_min_note(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "[panel] %s", buf);
}

void panel_min_status_json(char *out, size_t cap)
{
    if (out == NULL || cap < 32) {
        return;
    }
    llm_client_status_t ls;
    llm_client_get_status(&ls);
    voice_io_status_t vs;
    voice_io_get_status(&vs);
    key_talk_status_t ks;
    key_talk_get_status(&ks);
    prov_status_t ps;
    prov_service_get_status(&ps);

    snprintf(out, cap,
             "{\"state\":\"%s\",\"detail\":\"%s\",\"llm\":{\"connected\":%s,\"session\":%s,"
             "\"up_frames\":%u,\"up_bytes\":%u,\"down_bytes\":%u,\"turns\":%u,\"cancels\":%u,"
             "\"errors\":%u,\"last_turn\":%d,\"last_rtt_ms\":%d},"
             "\"io\":{\"capturing\":%s,\"playing\":%s,\"down_drops\":%u},"
             "\"key\":{\"talking\":%s,\"talks\":%u,\"shorts\":%u},"
             "\"net\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"source\":\"%s\","
             "\"ble\":%s,\"smartconfig\":%s},\"events\":%u,\"uptime_ms\":%lld}",
             s_state, s_detail, ls.connected ? "true" : "false", ls.session_ready ? "true" : "false",
             (unsigned)ls.up_frames, (unsigned)ls.up_bytes, (unsigned)ls.down_bytes,
             (unsigned)ls.turns, (unsigned)ls.cancels, (unsigned)ls.errors, ls.last_turn_seq,
             ls.last_rtt_ms, vs.capturing ? "true" : "false", vs.playing ? "true" : "false",
             (unsigned)vs.down_drops, ks.talking ? "true" : "false", (unsigned)ks.talk_count,
             (unsigned)ks.short_count, ps.connected ? "true" : "false", ps.ssid, ps.ip, ps.source,
             ps.ble_active ? "true" : "false", ps.smartconfig ? "true" : "false",
             (unsigned)s_events, (long long)(esp_timer_get_time() / 1000));
}
