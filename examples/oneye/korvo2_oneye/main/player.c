/*
 * player.c —— SD 卡媒体板上回放实现（口径见 player.h）
 *
 * 结构与 ADF 上游 examples/player/pipeline_play_sdcard_music 同构（fatfs → decoder → i2s），
 * 但只播单文件、按需建管线、播完自动回收，便于被本地验证面按文件调用。
 *
 * 并发口径（避免 use-after-free）：**管线与事件接口由播放任务独占销毁**
 *   - 外部 `player_stop()` 只置停止标志 + 等任务回收（最多 3 s），不直接拆管线；
 *   - 播放任务在事件循环里以 500 ms 超时轮询停止标志，退出前完成回收并清句柄；
 *   - 无任务在跑时（启动失败/已结束）由调用方直接回收。
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_mem.h"
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "wav_decoder.h"
#include "mp3_decoder.h"
#include "board.h"          /* CODEC_ADC_I2S_PORT（板级 I2S 端口） */

#include "player.h"

static const char *TAG = "player";

#define PLAYER_I2S_PORT      CODEC_ADC_I2S_PORT
#define PLAYER_I2S_BITS      I2S_DATA_BIT_WIDTH_16BIT
#define PLAYER_DEFAULT_RATE  48000

static SemaphoreHandle_t  s_lock;
static audio_hal_handle_t s_codec;
static audio_pipeline_handle_t s_pipeline;
static audio_element_handle_t  s_reader, s_decoder, s_writer;
static audio_event_iface_handle_t s_evt;
static TaskHandle_t s_task;
static volatile bool s_stop_req;
static player_status_t s_st;
static int64_t s_start_ms;

static void pl_lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void pl_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static void set_msg_locked(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_st.last_msg, sizeof(s_st.last_msg), fmt, ap);
    va_end(ap);
}

static bool path_ok(const char *path)
{
    return path != NULL && strncmp(path, "/sdcard/", 8) == 0 && strstr(path, "..") == NULL;
}

static const char *ext_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

/* 仅在没有播放任务时调用（任务运行中由任务自己回收） */
static void teardown_no_task(void)
{
    if (s_pipeline) {
        (void)audio_pipeline_stop(s_pipeline);
        (void)audio_pipeline_wait_for_stop(s_pipeline);
        if (s_evt) {
            audio_pipeline_remove_listener(s_pipeline);
        }
        /* ⚠️ ADF 所有权口径：audio_pipeline_deinit() 会**逐个 deinit 已注册元素**
         * （components/audio_pipeline/audio_pipeline.c:263-271：audio_element_deinit + unregister）。
         * 因此这里**不得**再对元素调用 audio_element_deinit —— 真机实测重复销毁会导致
         * audio_element_deinit → audio_element_stop → xEventGroupSetBits 在已释放的事件组加锁，
         * 触发 `assert failed: spinlock_acquire (lock->count == 0)` → 设备重启（停止播放必崩）。
         * 元素句柄随管线销毁一并失效，这里只置空。 */
        (void)audio_pipeline_deinit(s_pipeline);
        s_pipeline = NULL;
        s_writer = NULL;
        s_decoder = NULL;
        s_reader = NULL;
    }
    if (s_evt)     { audio_event_iface_destroy(s_evt); s_evt = NULL; }
    if (s_codec) {
        (void)audio_hal_ctrl_codec(s_codec, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_STOP);
    }
    s_start_ms = 0;
}

esp_err_t player_init(audio_hal_handle_t codec_hal)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_codec = codec_hal;
    pl_lock();
    s_st.last_err = ESP_OK;
    s_st.volume = 80;
    s_st.volume_known = true;
    snprintf(s_st.codec, sizeof(s_st.codec), "-");
    set_msg_locked("就绪");
    pl_unlock();
    return ESP_OK;
}

bool player_is_playing(void)
{
    bool r;
    pl_lock();
    r = s_st.playing;
    pl_unlock();
    return r;
}

void player_get_status(player_status_t *out)
{
    if (out == NULL) {
        return;
    }
    pl_lock();
    *out = s_st;
    if (s_st.playing && s_start_ms > 0) {
        out->elapsed_ms = (uint32_t)((esp_timer_get_time() / 1000) - s_start_ms);
    }
    pl_unlock();
}

esp_err_t player_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 100) {
        volume = 100;
    }
    if (s_codec == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t rc = audio_hal_set_volume(s_codec, volume);
    pl_lock();
    s_st.volume = (uint8_t)volume;
    pl_unlock();
    return rc;
}

esp_err_t player_stop(void)
{
    pl_lock();
    bool was = s_st.playing;
    bool has_task = (s_task != NULL);
    pl_unlock();

    if (has_task) {
        /* 交给播放任务回收（它独占管线与事件接口） */
        s_stop_req = true;
        for (int i = 0; i < 30 && s_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_task != NULL) {
            ESP_LOGW(TAG, "播放任务未在 3 s 内退出（继续等待由其自行回收）");
        }
    } else {
        teardown_no_task();
    }

    pl_lock();
    s_st.playing = false;
    if (was) {
        set_msg_locked("已停止");
    }
    pl_unlock();
    return ESP_OK;
}

/* 播放任务：事件循环 + 停止标志轮询；退出前独占回收并清句柄 */
static void player_task(void *arg)
{
    (void)arg;
    while (!s_stop_req) {
        audio_event_iface_msg_t msg;
        esp_err_t rc = audio_event_iface_listen(s_evt, &msg, pdMS_TO_TICKS(500));
        if (rc != ESP_OK) {
            continue;
        }
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)s_decoder &&
            msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t info = { 0 };
            audio_element_getinfo(s_decoder, &info);
            if (info.sample_rates > 0) {
                ESP_LOGI(TAG, "解码信息：%d Hz / %d bit / %d ch", info.sample_rates, info.bits, info.channels);
                (void)i2s_stream_set_clk(s_writer, info.sample_rates,
                                         info.bits > 0 ? info.bits : 16,
                                         info.channels > 0 ? info.channels : 1);
                pl_lock();
                s_st.rate_hz = (uint32_t)info.sample_rates;
                s_st.channels = (uint8_t)(info.channels > 0 ? info.channels : 1);
                pl_unlock();
            }
            continue;
        }
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)s_writer &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
            (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGI(TAG, "播放结束");
            pl_lock();
            set_msg_locked("播放结束");
            pl_unlock();
            break;
        }
    }

    teardown_no_task();
    pl_lock();
    s_st.playing = false;
    s_task = NULL;
    pl_unlock();
    vTaskDelete(NULL);
}

esp_err_t player_play(const char *path)
{
    if (!path_ok(path)) {
        pl_lock();
        s_st.last_err = ESP_ERR_INVALID_ARG;
        set_msg_locked("路径非法（须为 /sdcard/ 下且不含 ..）");
        pl_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    const char *ext = ext_of(path);
    bool is_wav = (strcasecmp(ext, "wav") == 0);
    bool is_mp3 = (strcasecmp(ext, "mp3") == 0);
    if (!is_wav && !is_mp3) {
        pl_lock();
        s_st.last_err = ESP_ERR_NOT_SUPPORTED;
        set_msg_locked("格式不支持：.%s（本轮支持 wav/mp3）", ext);
        pl_unlock();
        ESP_LOGW(TAG, "不支持的扩展名：.%s", ext);
        return ESP_ERR_NOT_SUPPORTED;
    }

    (void)player_stop();        /* 先停掉上一次（并等待其回收） */
    s_stop_req = false;

    /* codec 启动 + 音量（回放走 DAC 通路） */
    if (s_codec != NULL) {
        (void)audio_hal_ctrl_codec(s_codec, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);
        (void)audio_hal_set_volume(s_codec, s_st.volume ? s_st.volume : 80);
    }

    fatfs_stream_cfg_t fs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fs_cfg.type = AUDIO_STREAM_READER;
    s_reader = fatfs_stream_init(&fs_cfg);
    if (s_reader == NULL) {
        goto fail;
    }
    audio_element_set_uri(s_reader, path);

    if (is_wav) {
        wav_decoder_cfg_t cfg = DEFAULT_WAV_DECODER_CONFIG();
        s_decoder = wav_decoder_init(&cfg);
    } else {
        mp3_decoder_cfg_t cfg = DEFAULT_MP3_DECODER_CONFIG();
        s_decoder = mp3_decoder_init(&cfg);
    }
    if (s_decoder == NULL) {
        goto fail;
    }

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(
        PLAYER_I2S_PORT, PLAYER_DEFAULT_RATE, PLAYER_I2S_BITS, AUDIO_STREAM_WRITER);
    i2s_cfg.task_stack = -1;
    s_writer = i2s_stream_init(&i2s_cfg);
    if (s_writer == NULL) {
        goto fail;
    }

    audio_pipeline_cfg_t pipe_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipeline = audio_pipeline_init(&pipe_cfg);
    if (s_pipeline == NULL) {
        goto fail;
    }
    audio_pipeline_register(s_pipeline, s_reader, "file");
    audio_pipeline_register(s_pipeline, s_decoder, "decoder");
    audio_pipeline_register(s_pipeline, s_writer, "i2s");
    const char *link[3] = { "file", "decoder", "i2s" };
    if (audio_pipeline_link(s_pipeline, link, 3) != ESP_OK) {
        goto fail;
    }

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    s_evt = audio_event_iface_init(&evt_cfg);
    if (s_evt == NULL) {
        goto fail;
    }
    audio_pipeline_set_listener(s_pipeline, s_evt);

    pl_lock();
    s_st.playing = true;
    s_st.last_err = ESP_OK;
    snprintf(s_st.path, sizeof(s_st.path), "%s", path);
    snprintf(s_st.codec, sizeof(s_st.codec), "%s", is_wav ? "wav" : "mp3");
    s_st.rate_hz = 0;
    s_st.channels = 0;
    s_start_ms = esp_timer_get_time() / 1000;
    set_msg_locked("播放中");
    pl_unlock();

    if (audio_pipeline_run(s_pipeline) != ESP_OK || xTaskCreate(player_task, "player_evt", 4096, NULL, 4, &s_task) != pdPASS) {
        goto fail;
    }
    ESP_LOGI(TAG, "开始播放：%s（%s）", path, is_wav ? "wav" : "mp3");
    return ESP_OK;

fail:
    teardown_no_task();
    pl_lock();
    s_st.playing = false;
    s_st.last_err = ESP_FAIL;
    set_msg_locked("播放启动失败（文件不存在或解码器初始化失败）");
    pl_unlock();
    ESP_LOGE(TAG, "播放启动失败：%s", path);
    return ESP_FAIL;
}
