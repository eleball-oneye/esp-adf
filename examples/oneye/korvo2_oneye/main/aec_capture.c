/*
 * aec_capture.c —— AEC 采集实现（口径见 aec_capture.h）
 *
 * 管线（与 ADF 上游 algorithm 例程同构，故行为可对照）：
 *   i2s_stream(reader, 16k/32bit/RIGHT_LEFT) → algorithm_stream(AFE_TYPE_SR 双麦, 输入 "RMNM")
 *     → wav_encoder(16k/16bit/1ch) → fatfs_stream(writer) → /sdcard/rec/aec-N.wav | /spiffs/rec/aec-N.wav
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_mem.h"
#include "driver/i2s.h"          /* 传统 I2S 枚举 I2S_CHANNEL_FMT_*（与 ADF 例程同口径；ADF 的 i2s_stream_set_channel_type 接受之） */
#include "i2s_stream.h"
#include "algorithm_stream.h"
#include "wav_encoder.h"
#include "fatfs_stream.h"
#include "audio_hal.h"
#include "board.h"
#include "es7210.h"

#include "aec_capture.h"

static const char *TAG = "aec_capture";

#define AEC_CAPTURE_I2S_PORT   CODEC_ADC_I2S_PORT
#define AEC_CAPTURE_RATE       16000
#define AEC_CAPTURE_BITS       CODEC_ADC_BITS_PER_SAMPLE      /* 32 bit（16 麦 + 16 回采） */
#define AEC_CAPTURE_I2S_CH     I2S_CHANNEL_FMT_RIGHT_LEFT
#define AEC_CAPTURE_WAV_CH     1
#define AEC_CAPTURE_WAV_BITS   16

#define AEC_ROOT_SD            "/sdcard/rec"
#define AEC_ROOT_SPIFFS        "/spiffs/rec"

#ifndef CONFIG_ONEYE_FW_AEC_DEFAULT_S
#define AEC_DEFAULT_DURATION_S 5
#else
#define AEC_DEFAULT_DURATION_S CONFIG_ONEYE_FW_AEC_DEFAULT_S
#endif

static SemaphoreHandle_t  s_lock;
static audio_element_handle_t s_i2s_reader;
static audio_pipeline_handle_t s_pipeline;
static audio_element_handle_t s_algo, s_wav, s_writer;
static TaskHandle_t       s_stop_task;
static aec_capture_status_t s_st;

/* ------------------------------------------------------------------ 内部工具 */

static int i2s_read_cb(audio_element_handle_t el, char *buf, int len, TickType_t wait, void *ctx)
{
    (void)el; (void)wait; (void)ctx;
    int r = audio_element_input(s_i2s_reader, buf, len);
    if (r <= 0) {
        ESP_LOGW(TAG, "I2S 读取失败/结束：%d", r);
    }
    return r;
}

static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        (void)mkdir(path, 0775);
    }
}

static void aec_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void aec_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/* ------------------------------------------------------------------ 初始化 */

esp_err_t aec_capture_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

#if !CONFIG_ONEYE_FW_ENABLE_AEC_CAPTURE
    s_st.enabled = false;
    ESP_LOGW(TAG, "AEC 采集未启用（CONFIG_ONEYE_FW_ENABLE_AEC_CAPTURE=n）");
    return ESP_OK;
#else
    s_st.enabled = true;

    /* SPIFFS 兜底存储（无 SD 卡时用）；已注册则忽略错误 */
    esp_vfs_spiffs_conf_t spiffs = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t rc = esp_vfs_spiffs_register(&spiffs);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS 挂载失败：%s（无 SD 卡时将无法落盘）", esp_err_to_name(rc));
    } else {
        size_t total = 0, used = 0;
        (void)esp_spiffs_info("storage", &total, &used);
        ESP_LOGI(TAG, "SPIFFS 兜底存储：total=%u KB, used=%u KB", (unsigned)(total / 1024), (unsigned)(used / 1024));
    }

    /* ADC 增益：与 ADF algorithm 例程的本板口径一致（MIC1/2 = 33 dB，MIC3(AEC 回采) = 24 dB） */
    (void)es7210_adc_set_gain(ES7210_INPUT_MIC2 | ES7210_INPUT_MIC1, GAIN_33DB);
    (void)es7210_adc_set_gain(ES7210_INPUT_MIC3, GAIN_24DB);

    /* I2S 读元素（常驻；采集时由 pipeline 拉起） */
    i2s_stream_cfg_t i2s_r_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(
        AEC_CAPTURE_I2S_PORT, AEC_CAPTURE_RATE, AEC_CAPTURE_BITS, AUDIO_STREAM_READER);
    i2s_r_cfg.task_stack = -1;                     /* 复用 pipeline 任务栈 */
    i2s_stream_set_channel_type(&i2s_r_cfg, AEC_CAPTURE_I2S_CH);
    s_i2s_reader = i2s_stream_init(&i2s_r_cfg);
    if (s_i2s_reader == NULL) {
        s_st.last_err = ESP_FAIL;
        ESP_LOGE(TAG, "I2S 读元素创建失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AEC 采集已就绪（%d Hz / %d bit / %s → AFE → WAV %d Hz %d bit 单声道）",
             AEC_CAPTURE_RATE, (int)AEC_CAPTURE_BITS, AUDIO_ADC_INPUT_CH_FORMAT,
             AEC_CAPTURE_RATE, AEC_CAPTURE_WAV_BITS);
    return ESP_OK;
#endif
}

bool aec_capture_enabled(void)
{
    return s_st.enabled;
}

bool aec_capture_is_recording(void)
{
    bool r;
    aec_lock();
    r = s_st.recording;
    aec_unlock();
    return r;
}

const char *aec_capture_root(void)
{
    return s_st.root[0] ? s_st.root : AEC_ROOT_SPIFFS;
}

void aec_capture_get_status(aec_capture_status_t *out)
{
    if (out == NULL) {
        return;
    }
    aec_lock();
    *out = s_st;
    aec_unlock();
}

/** SD 是否已挂载（以 /sdcard 是否可访问为准） */
void aec_capture_set_sd_mounted(bool mounted)
{
    aec_lock();
    s_st.sd_mounted = mounted;
    snprintf(s_st.root, sizeof(s_st.root), "%s", mounted ? AEC_ROOT_SD : AEC_ROOT_SPIFFS);
    aec_unlock();
}

/* ------------------------------------------------------------------ 采集线程 */

static void aec_stop_task(void *arg)
{
    uint32_t seconds = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    ESP_LOGI(TAG, "采集时长（%u s）已到，自动停止", (unsigned)seconds);
    (void)aec_capture_stop();
    s_stop_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ 启停 */

esp_err_t aec_capture_start(uint32_t duration_s, char *out_path, size_t cap)
{
#if !CONFIG_ONEYE_FW_ENABLE_AEC_CAPTURE
    (void)duration_s; (void)out_path; (void)cap;
    return ESP_ERR_NOT_SUPPORTED;
#else
    aec_lock();
    if (s_st.recording) {
        aec_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_st.recording = true;
    s_st.last_err = ESP_OK;
    if (duration_s == 0) {
        duration_s = AEC_DEFAULT_DURATION_S;
    }
    char path[AEC_CAPTURE_PATH_MAX];
    ensure_dir(s_st.sd_mounted ? "/sdcard" : "/spiffs");
    ensure_dir(aec_capture_root());
    snprintf(path, sizeof(path), "%s/aec-%05u.wav", aec_capture_root(),
             (unsigned)(++s_st.total_files));
    snprintf(s_st.last_file, sizeof(s_st.last_file), "%s", path);
    aec_unlock();

    if (out_path && cap) {
        snprintf(out_path, cap, "%s", path);
    }
    int64_t t0 = esp_timer_get_time();

    /* 1) AFE（AEC + 降噪）—— AFE_TYPE_VC（语音通信型，AEC 为强项；不含唤醒词/命令词模型）
     *    参数与输入格式沿用本板口径：`AUDIO_ADC_INPUT_CH_FORMAT` = "RMNM"（2 麦 + AEC 回采） */
    algorithm_stream_cfg_t algo_cfg = ALGORITHM_STREAM_CFG_DEFAULT();
    algo_cfg.input_format = AUDIO_ADC_INPUT_CH_FORMAT;
    algo_cfg.task_prio = 5;
    algo_cfg.sample_rate = AEC_CAPTURE_RATE;
    algo_cfg.out_rb_size = 256;
    s_algo = algo_stream_init(&algo_cfg);
    if (s_algo == NULL) {
        goto fail;
    }
    audio_element_set_music_info(s_algo, AEC_CAPTURE_RATE, 1, ALGORITHM_STREAM_DEFAULT_SAMPLE_BIT);
    audio_element_set_read_cb(s_algo, i2s_read_cb, NULL);
    audio_element_set_input_timeout(s_algo, portMAX_DELAY);

    /* 2) WAV 编码 */
    wav_encoder_cfg_t wav_cfg = DEFAULT_WAV_ENCODER_CONFIG();
    s_wav = wav_encoder_init(&wav_cfg);
    if (s_wav == NULL) {
        goto fail;
    }

    /* 3) 文件写入 */
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_WRITER;
    fatfs_cfg.task_prio = 5;
    fatfs_cfg.task_core = 1;
    s_writer = fatfs_stream_init(&fatfs_cfg);
    if (s_writer == NULL) {
        goto fail;
    }
    audio_element_set_uri(s_writer, path);

    audio_element_info_t info = { 0 };
    audio_element_getinfo(s_writer, &info);
    info.sample_rates = AEC_CAPTURE_RATE;
    info.bits = AEC_CAPTURE_WAV_BITS;
    info.channels = AEC_CAPTURE_WAV_CH;
    audio_element_setinfo(s_writer, &info);

    /* 4) 管线 */
    audio_pipeline_cfg_t pipe_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipeline = audio_pipeline_init(&pipe_cfg);
    if (s_pipeline == NULL) {
        goto fail;
    }
    audio_pipeline_register(s_pipeline, s_algo, "algo");
    audio_pipeline_register(s_pipeline, s_wav, "wav");
    audio_pipeline_register(s_pipeline, s_writer, "file");
    const char *link[3] = { "algo", "wav", "file" };
    if (audio_pipeline_link(s_pipeline, link, 3) != ESP_OK) {
        goto fail;
    }
    if (audio_pipeline_run(s_pipeline) != ESP_OK) {
        goto fail;
    }

    ESP_LOGI(TAG, "开始采集：%s（%u s）", path, (unsigned)duration_s);
    (void)xTaskCreate(aec_stop_task, "aec_stop", 3072, (void *)(uintptr_t)duration_s, 4, &s_stop_task);
    return ESP_OK;

fail:
    aec_lock();
    s_st.recording = false;
    s_st.last_err = ESP_FAIL;
    aec_unlock();
    ESP_LOGE(TAG, "采集启动失败（见上）；请检查 SD/SPIFFS 与 esp-sr 模型配置");
    (void)aec_capture_stop();
    return ESP_FAIL;
#endif
}

esp_err_t aec_capture_stop(void)
{
#if !CONFIG_ONEYE_FW_ENABLE_AEC_CAPTURE
    return ESP_ERR_NOT_SUPPORTED;
#else
    aec_lock();
    bool was = s_st.recording;
    s_st.recording = false;
    char path[AEC_CAPTURE_PATH_MAX];
    snprintf(path, sizeof(path), "%s", s_st.last_file);
    aec_unlock();
    if (!was && s_pipeline == NULL) {
        return ESP_OK;
    }

    if (s_pipeline) {
        (void)audio_pipeline_stop(s_pipeline);
        (void)audio_pipeline_wait_for_stop(s_pipeline);
        (void)audio_pipeline_deinit(s_pipeline);
        s_pipeline = NULL;
    }
    if (s_writer) { audio_element_deinit(s_writer); s_writer = NULL; }
    if (s_wav)    { audio_element_deinit(s_wav);    s_wav = NULL; }
    if (s_algo)   { audio_element_deinit(s_algo);   s_algo = NULL; }

    /* 落盘结果（大小 = 文件实际长度；WAV 头由 wav_encoder 在结束时回填） */
    struct stat st;
    uint32_t bytes = 0;
    if (path[0] && stat(path, &st) == 0) {
        bytes = (uint32_t)st.st_size;
    }
    aec_lock();
    s_st.last_bytes = bytes;
    s_st.last_duration_ms = (uint32_t)(esp_timer_get_time() / 1000 % 1000000);
    aec_unlock();
    ESP_LOGI(TAG, "采集结束：%s（%u 字节）；可用面板 /media/list 播放", path, (unsigned)bytes);
    return ESP_OK;
#endif
}
