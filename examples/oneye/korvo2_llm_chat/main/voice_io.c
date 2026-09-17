/*
 * voice_io.c —— 音频出入口实现（I2S + AFE 采集 / PCM 回放）
 *
 * 采集配方**逐项照抄** examples/oneye/korvo2_oneye/main/aec_capture.c（同板真机取证）：
 *   I2S0 16 kHz/32 bit/ONLY_LEFT → algorithm_stream（TYPE1 + AFE_TYPE_VC + AEC|NS，
 *   input_format "RM"）→ raw_stream（写入侧，本模块以 1920 B 粒度取走并回调）。
 * 回放配方照抄同工程 player.c（raw → i2s writer + codec START + set_volume），
 * 但**自行把单声道扩成立体声**（S3 上 ADF 的 i2s_mono_fix 不参与编译，见 voice_io.h 头注）。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "board.h"
#include "audio_hal.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_mem.h"
#include "i2s_stream.h"
#include "algorithm_stream.h"
#include "raw_stream.h"
#include "es7210.h"

#include "voice_io.h"

static const char *TAG = "voice_io";

#define VOICE_RATE            16000
#define VOICE_CAP_I2S_BITS    CODEC_ADC_BITS_PER_SAMPLE /* 32 bit（16 麦 + 16 回采） */
#define VOICE_CAP_INPUT_FMT   "RM"                      /* 单麦：麦 + AEC 回采两通道 */
#define VOICE_CAP_I2S_CH      I2S_CHANNEL_TYPE_ONLY_LEFT
#define VOICE_FRAME_SAMPLES   (VOICE_IO_FRAME_BYTES / 2)
#define VOICE_STEREO_BYTES    (VOICE_IO_FRAME_BYTES * 2)
#define VOICE_READ_TIMEOUT_MS 200
#define VOICE_WRITE_TIMEOUT_MS 100

static SemaphoreHandle_t s_lock;
static audio_hal_handle_t s_codec;

/* 采集侧 */
static audio_element_handle_t s_cap_i2s;   /* 不属管线（由 read_cb 取数），须自行销毁 */
static audio_element_handle_t s_cap_algo;
static audio_element_handle_t s_cap_raw;   /* 属管线：由 pipeline_deinit 统一销毁 */
static audio_pipeline_handle_t s_cap_pipe;
static TaskHandle_t s_cap_task;
static volatile bool s_capturing;
static voice_io_pcm_cb_t s_cap_cb;
static void *s_cap_ctx;
static size_t s_cap_bytes;

/* 回放侧 */
static audio_element_handle_t s_play_raw;
static audio_element_handle_t s_play_i2s;
static audio_pipeline_handle_t s_play_pipe;
static volatile bool s_playing;
static uint8_t s_stereo_buf[VOICE_STEREO_BYTES];

static voice_io_status_t s_st;

/* ------------------------------------------------------------------ 小工具 */

static void vio_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void vio_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/** 启动 ADC：先 `AUDIO_HAL_CTRL_START`，再重设增益。
 *  ES7210 的 STOP 会把 MIC 偏置/模拟块整体断电，板上回放结束会 STOP，故每次采集前都要重新 arm。
 *  注意：`es7210_adc_ctrl_state()` 的返回值**不是状态码**（它把寄存器读值当返回值），
 *  所以这里只在写入动作上判断，非 0 仅记 INFO。 */
static void vio_codec_arm_adc(void)
{
    if (s_codec == NULL) {
        ESP_LOGW(TAG, "无 audio_hal 句柄，跳过 ADC START（录音幅度可能异常）");
        return;
    }
    esp_err_t rc = audio_hal_ctrl_codec(s_codec, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);
    if (rc != ESP_OK) {
        ESP_LOGI(TAG, "ADC START 驱动返回寄存器值 0x%x（非错误码）", (unsigned)rc);
    }
    (void)es7210_adc_set_gain(ES7210_INPUT_MIC3, GAIN_24DB);
    (void)es7210_adc_set_gain(ES7210_INPUT_MIC2 | ES7210_INPUT_MIC1, GAIN_33DB);
}

/** AFE 的读回调：从 I2S 读元素取原始数据（i2s 元素 task_stack = -1，无自有任务） */
static int vio_i2s_read_cb(audio_element_handle_t el, char *buf, int len, TickType_t wait, void *ctx)
{
    (void)el;
    (void)wait;
    (void)ctx;
    int r = audio_element_input(s_cap_i2s, buf, len);
    return r;
}

/* ------------------------------------------------------------------ 采集任务 */

static void vio_cap_task(void *arg)
{
    (void)arg;
    char *buf = audio_malloc(VOICE_IO_FRAME_BYTES);
    if (buf == NULL) {
        ESP_LOGE(TAG, "采集缓冲分配失败");
        s_capturing = false;
        s_cap_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "采集开始（每帧 %d B = 60 ms @16 kHz/16 bit/单声道）", VOICE_IO_FRAME_BYTES);
    while (s_capturing) {
        int r = raw_stream_read(s_cap_raw, buf, VOICE_IO_FRAME_BYTES);
        if (r > 0) {
            s_cap_bytes += (size_t)r;
            if (s_cap_cb) {
                s_cap_cb(buf, (size_t)r, s_cap_ctx);
            }
            continue;
        }
        if (r == AEL_IO_TIMEOUT) {
            continue; /* 正常：等待更多数据（给 stop 留出检查窗口） */
        }
        ESP_LOGW(TAG, "采集读失败/结束：%d", r);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    audio_free(buf);
    ESP_LOGI(TAG, "采集任务退出（累计 %u B）", (unsigned)s_cap_bytes);
    s_cap_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ 初始化 / 状态 */

esp_err_t voice_io_init(audio_hal_handle_t codec_hal)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_codec = codec_hal;
    s_st.volume = CONFIG_ONEYE_LLM_PLAY_VOLUME;
    s_st.last_err = ESP_OK;
    ESP_LOGI(TAG, "音频出入口就绪（回放音量 %d）", s_st.volume);
    return ESP_OK;
}

bool voice_io_is_capturing(void)
{
    return s_capturing;
}

bool voice_io_is_playing(void)
{
    return s_playing;
}

size_t voice_io_captured_bytes(void)
{
    return s_cap_bytes;
}

void voice_io_get_status(voice_io_status_t *out)
{
    if (out == NULL) {
        return;
    }
    vio_lock();
    *out = s_st;
    out->capturing = s_capturing;
    out->playing = s_playing;
    out->up_bytes = (uint32_t)s_cap_bytes;
    vio_unlock();
}

esp_err_t voice_io_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 100) {
        volume = 100;
    }
    vio_lock();
    s_st.volume = volume;
    vio_unlock();
    if (s_codec) {
        return audio_hal_set_volume(s_codec, volume);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ 采集（上行） */

esp_err_t voice_io_capture_start(voice_io_pcm_cb_t cb, void *ctx)
{
    if (s_capturing) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 录音/回放互斥：本板 ES7210(ADC) 与 ES8311(DAC) 共用 I2S0；
     * 同跑会让两条管线都卡死（korvo2_oneye 真机取证 2026-09-16）。 */
    if (s_playing) {
        ESP_LOGW(TAG, "拒绝采集：正在回放（共用 I2S0）——请先打断回放");
        return ESP_ERR_INVALID_STATE;
    }

    s_cap_cb = cb;
    s_cap_ctx = ctx;
    s_cap_bytes = 0;

    vio_codec_arm_adc();

    /* 1) I2S 读元素（task_stack = -1：由 AFE 的读回调驱动，不单独占任务） */
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(
        CODEC_ADC_I2S_PORT, VOICE_RATE, VOICE_CAP_I2S_BITS, AUDIO_STREAM_READER);
    i2s_cfg.task_stack = -1;
    i2s_stream_set_channel_type(&i2s_cfg, VOICE_CAP_I2S_CH);
    s_cap_i2s = i2s_stream_init(&i2s_cfg);
    if (s_cap_i2s == NULL) {
        ESP_LOGE(TAG, "I2S 读元素创建失败");
        goto fail;
    }

    /* 2) AFE：单麦配方（ALGORITHM_STREAM_CFG_DEFAULT = TYPE1 + AFE_TYPE_VC + AEC|NS） */
    algorithm_stream_cfg_t algo_cfg = ALGORITHM_STREAM_CFG_DEFAULT();
    algo_cfg.input_format = VOICE_CAP_INPUT_FMT;
    algo_cfg.sample_rate = VOICE_RATE;
    algo_cfg.task_prio = 5;
    algo_cfg.out_rb_size = VOICE_IO_FRAME_BYTES * 4;
    s_cap_algo = algo_stream_init(&algo_cfg);
    if (s_cap_algo == NULL) {
        ESP_LOGE(TAG, "AFE 元素创建失败（检查 `model` 分区是否已烧写 srmodels.bin）");
        goto fail;
    }
    audio_element_set_music_info(s_cap_algo, VOICE_RATE, 1, ALGORITHM_STREAM_DEFAULT_SAMPLE_BIT);
    audio_element_set_read_cb(s_cap_algo, vio_i2s_read_cb, NULL);
    audio_element_set_input_timeout(s_cap_algo, portMAX_DELAY);

    /* 3) raw 收口：本模块从它的输入环形缓冲取走 PCM */
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = VOICE_IO_FRAME_BYTES * 4;
    s_cap_raw = raw_stream_init(&raw_cfg);
    if (s_cap_raw == NULL) {
        ESP_LOGE(TAG, "raw 元素创建失败");
        goto fail;
    }
    audio_element_set_input_timeout(s_cap_raw, pdMS_TO_TICKS(VOICE_READ_TIMEOUT_MS));

    /* 4) 管线：algo → raw */
    audio_pipeline_cfg_t pipe_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_cap_pipe = audio_pipeline_init(&pipe_cfg);
    if (s_cap_pipe == NULL) {
        goto fail;
    }
    audio_pipeline_register(s_cap_pipe, s_cap_algo, "afe");
    audio_pipeline_register(s_cap_pipe, s_cap_raw, "raw");
    const char *link[2] = { "afe", "raw" };
    if (audio_pipeline_link(s_cap_pipe, link, 2) != ESP_OK) {
        goto fail;
    }

    s_capturing = true;
    if (audio_pipeline_run(s_cap_pipe) != ESP_OK) {
        s_capturing = false;
        goto fail;
    }
    if (xTaskCreate(vio_cap_task, "vio_cap", 4096, NULL, 5, &s_cap_task) != pdPASS) {
        s_capturing = false;
        ESP_LOGE(TAG, "采集任务创建失败");
        goto fail;
    }
    vio_lock();
    s_st.last_err = ESP_OK;
    vio_unlock();
    return ESP_OK;

fail:
    (void)voice_io_capture_stop();
    vio_lock();
    s_st.last_err = ESP_FAIL;
    vio_unlock();
    return ESP_FAIL;
}

esp_err_t voice_io_capture_stop(void)
{
    bool was = s_capturing;
    s_capturing = false;

    /* 等采集任务退出（最多 1 s），避免它继续访问即将销毁的 raw 元素 */
    for (int i = 0; i < 100 && s_cap_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_cap_task != NULL) {
        ESP_LOGW(TAG, "采集任务未在预期时间内退出，强制继续拆卸");
    }

    if (s_cap_pipe) {
        (void)audio_pipeline_stop(s_cap_pipe);
        (void)audio_pipeline_wait_for_stop(s_cap_pipe);
        /* ⚠️ `audio_pipeline_deinit()` 已把**已注册**的元素全部 deinit + 反注册，
         * 这里**不得**再逐个 deinit（重复释放，korvo2_oneye 真机取证 2026-09-16 会 panic）。 */
        (void)audio_pipeline_deinit(s_cap_pipe);
        s_cap_pipe = NULL;
        s_cap_algo = NULL;
        s_cap_raw = NULL;
    }
    /* I2S 读元素不属管线，须自行销毁：把 I2S0 让给回放 */
    if (s_cap_i2s) {
        audio_element_deinit(s_cap_i2s);
        s_cap_i2s = NULL;
    }
    if (was) {
        ESP_LOGI(TAG, "采集已停止（本次 %u B）", (unsigned)s_cap_bytes);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ 回放（下行） */

esp_err_t voice_io_playback_open(void)
{
    if (s_playing) {
        return ESP_OK; /* 幂等：同一轮下行音频可能分多帧到达 */
    }
    if (s_capturing) {
        ESP_LOGW(TAG, "拒绝回放：正在采集（共用 I2S0）");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_codec != NULL) {
        (void)audio_hal_ctrl_codec(s_codec, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);
        (void)audio_hal_set_volume(s_codec, s_st.volume);
    }

    /* 注入侧：raw（本模块 raw_stream_write 推入 PCM）→ i2s 写元素 */
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    raw_cfg.out_rb_size = VOICE_IO_FRAME_BYTES * 8; /* ≈0.5 s 缓冲，抗抖动 */
    s_play_raw = raw_stream_init(&raw_cfg);
    if (s_play_raw == NULL) {
        ESP_LOGE(TAG, "raw 元素创建失败");
        goto fail;
    }

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(
        CODEC_ADC_I2S_PORT, VOICE_RATE, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_WRITER);
    /* 注意：**不要**设 task_stack = -1。写元素必须有自己的任务（缺了会「显示播放中但无声」，
     * 见 korvo2_oneye player.c 真机取证 2026-09-16）。 */
    s_play_i2s = i2s_stream_init(&i2s_cfg);
    if (s_play_i2s == NULL) {
        ESP_LOGE(TAG, "I2S 写元素创建失败");
        goto fail;
    }
    /* 立体声：本模块已把单声道复制成两声道，故 info.channels = 2 */
    audio_element_info_t info = { 0 };
    audio_element_getinfo(s_play_i2s, &info);
    info.sample_rates = VOICE_RATE;
    info.bits = 16;
    info.channels = 2;
    audio_element_setinfo(s_play_i2s, &info);

    audio_pipeline_cfg_t pipe_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_play_pipe = audio_pipeline_init(&pipe_cfg);
    if (s_play_pipe == NULL) {
        goto fail;
    }
    audio_pipeline_register(s_play_pipe, s_play_raw, "raw");
    audio_pipeline_register(s_play_pipe, s_play_i2s, "i2s");
    const char *link[2] = { "raw", "i2s" };
    if (audio_pipeline_link(s_play_pipe, link, 2) != ESP_OK) {
        goto fail;
    }
    if (audio_pipeline_run(s_play_pipe) != ESP_OK) {
        goto fail;
    }
    s_playing = true;
    ESP_LOGI(TAG, "回放打开（16 kHz/16 bit → 立体声 → ES8311）");
    return ESP_OK;

fail:
    (void)voice_io_playback_close();
    vio_lock();
    s_st.last_err = ESP_FAIL;
    vio_unlock();
    return ESP_FAIL;
}

esp_err_t voice_io_playback_write(const void *pcm, size_t len)
{
    if (!s_playing || s_play_raw == NULL || pcm == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 单声道 → 立体声（L = R）。S3 上 ADF 不会自动处理（见 voice_io.h 头注）。 */
    size_t off = 0;
    const int16_t *src = (const int16_t *)pcm;
    size_t samples = len / sizeof(int16_t);
    while (off < samples) {
        size_t chunk = samples - off;
        if (chunk > VOICE_FRAME_SAMPLES) {
            chunk = VOICE_FRAME_SAMPLES;
        }
        int16_t *dst = (int16_t *)s_stereo_buf;
        for (size_t i = 0; i < chunk; i++) {
            dst[2 * i] = src[off + i];
            dst[2 * i + 1] = src[off + i];
        }
        size_t bytes = chunk * 2 * sizeof(int16_t);
        int w = raw_stream_write(s_play_raw, (char *)s_stereo_buf, (int)bytes);
        if (w <= 0) {
            vio_lock();
            s_st.down_drops++;
            vio_unlock();
            ESP_LOGW(TAG, "回放写入超时（缓冲满），丢弃 %u B", (unsigned)bytes);
            return ESP_ERR_TIMEOUT;
        }
        off += chunk;
    }
    vio_lock();
    s_st.down_bytes += (uint32_t)len;
    vio_unlock();
    return ESP_OK;
}

esp_err_t voice_io_playback_close(void)
{
    bool was = s_playing;
    s_playing = false;

    if (s_play_pipe) {
        (void)audio_pipeline_stop(s_play_pipe);
        (void)audio_pipeline_wait_for_stop(s_play_pipe);
        (void)audio_pipeline_deinit(s_play_pipe); /* 已注册元素一并销毁（勿重复 deinit） */
        s_play_pipe = NULL;
        s_play_raw = NULL;
        s_play_i2s = NULL;
    }
    if (s_codec != NULL) {
        /* 与 ADF 播放例程同口径：回放结束停编解码器（下次采集前会重新 arm ADC） */
        (void)audio_hal_ctrl_codec(s_codec, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_STOP);
    }
    if (was) {
        ESP_LOGI(TAG, "回放已关闭（本轮下行 %u B）", (unsigned)s_st.down_bytes);
    }
    return ESP_OK;
}
