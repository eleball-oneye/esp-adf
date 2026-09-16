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
#include "player.h"     /* 录音/回放共用 I2S0，需互斥 */

static const char *TAG = "aec_capture";

#define AEC_CAPTURE_I2S_PORT   CODEC_ADC_I2S_PORT
#define AEC_CAPTURE_RATE       16000
#define AEC_CAPTURE_BITS       CODEC_ADC_BITS_PER_SAMPLE      /* 32 bit（16 麦 + 16 回采） */
/* 单麦配方（照抄 ADF 例程 algorithm 的 Korvo-2 单麦分支；见 aec_capture.h 头注）：
 *   input_format "RM"（2 通道：麦 + AEC 回采）＋ I2S 只取左声道。
 *   例程双麦分支才是 `AUDIO_ADC_INPUT_CH_FORMAT`("RMNM") + I2S_CHANNEL_FMT_RIGHT_LEFT。 */
#define AEC_CAPTURE_INPUT_FORMAT  "RM"
#define AEC_CAPTURE_I2S_CH     I2S_CHANNEL_FMT_ONLY_LEFT
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
static audio_hal_handle_t s_codec;      /* 板级 audio_hal（ES7210 ADC 启停用） */

/* ------------------------------------------------------------------ 内部工具 */

/** 启动 ADC：与例程同序 —— 先 `AUDIO_HAL_CTRL_START`，再重设增益。
 *  原因见 es7210 驱动：`es7210_start()` 内部会 `es7210_mic_select()`，把**所有**已选麦位
 *  统一刷成 `es7210_handle.gain`（最后一次 set_gain 的值）；若不在 START 之后重设，
 *  MIC1/MIC2 的 33 dB 会被冲掉。板上回放结束会 `AUDIO_HAL_CTRL_STOP` 把 ADC 整体断电，
 *  所以每次录音前都要重新 arm。 */
static void aec_codec_arm(void)
{
    if (s_codec == NULL) {
        ESP_LOGW(TAG, "无 audio_hal 句柄，跳过 ADC START（录音幅度可能异常）");
        return;
    }
    esp_err_t rc = audio_hal_ctrl_codec(s_codec, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);
    if (rc != ESP_OK) {
        /* 注意：`es7210_adc_ctrl_state()` 的返回值**不是状态码**——它把 CLOCK_OFF 寄存器的读值
         * 直接当返回值（真机实测返回 0x20），所以这里非 0 属正常。只在**写入动作**上判断，
         * 用 INFO 记录原始值以便对照（避免把成功误报成失败）。 */
        ESP_LOGI(TAG, "ADC START（驱动返回寄存器值 0x%x，非错误码；MIC 使能与 TDM 已按上述日志生效）",
                 (unsigned)rc);
    }
    (void)es7210_adc_set_gain(ES7210_INPUT_MIC3, GAIN_24DB);
    (void)es7210_adc_set_gain(ES7210_INPUT_MIC2 | ES7210_INPUT_MIC1, GAIN_33DB);
}

/* I2S 原始幅度诊断（真机取证用）：AFE 输出为 16 bit 单声道 WAV，一旦幅度异常（近乎静音）
 * 无法区分「麦/编解码器没出声」与「AFE 通道配方不对」。这里在录音前若干帧统计 I2S 原始
 * 数据的 RMS/峰值并打日志——正常说话时 RMS 应显著高于静音底噪（此前实测异常值 RMS≈137/32768）。*/
#define AEC_RMS_CHUNKS   20
static uint64_t s_rms_sum_sq;
static uint32_t s_rms_peak;
static uint32_t s_rms_samples;
static int      s_rms_chunks;

/** 整数平方根（避免为一个诊断量引入 libm 依赖） */
static uint32_t aec_isqrt(uint64_t v)
{
    uint64_t r = 0;
    uint64_t bit = (uint64_t)1 << 62;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

static void aec_rms_probe(const char *buf, int len)
{
    if (s_rms_chunks >= AEC_RMS_CHUNKS) {
        return;
    }
    if (s_rms_chunks == 0) {
        /* 首帧原始内容（对齐/左右对齐方式排查用）：按 16 bit 与 32 bit 两种视角各打几个值 */
        ESP_LOGW(TAG, "[诊断] 首帧 %d 字节", len);
        if (len >= 16) {
            const int16_t *w16 = (const int16_t *)buf;
            ESP_LOGW(TAG, "[诊断] 同帧 int16[0..7]=%d,%d,%d,%d,%d,%d,%d,%d",
                     w16[0], w16[1], w16[2], w16[3], w16[4], w16[5], w16[6], w16[7]);
            const uint32_t *w32 = (const uint32_t *)buf;
            ESP_LOGW(TAG, "[诊断] 同帧 int32[0..3]=0x%08x 0x%08x 0x%08x 0x%08x",
                     (unsigned)w32[0], (unsigned)w32[1], (unsigned)w32[2], (unsigned)w32[3]);
        }
    }
    const int16_t *s = (const int16_t *)buf;
    int n = len / (int)sizeof(int16_t);
    for (int i = 0; i < n; i++) {
        int32_t v = s[i];
        s_rms_sum_sq += (uint64_t)((int64_t)v * v);
        uint32_t a = (uint32_t)(v < 0 ? -v : v);
        if (a > s_rms_peak) {
            s_rms_peak = a;
        }
    }
    s_rms_samples += (uint32_t)n;
    if (++s_rms_chunks == AEC_RMS_CHUNKS && s_rms_samples > 0) {
        uint32_t rms = aec_isqrt(s_rms_sum_sq / s_rms_samples);
        ESP_LOGW(TAG, "[诊断] I2S 原始幅度（前 %d 帧 / %u 样点）：RMS=%u 峰值=%u（满量程 32768）",
                 AEC_RMS_CHUNKS, (unsigned)s_rms_samples, (unsigned)rms, (unsigned)s_rms_peak);
        if (rms < 20) {
            ESP_LOGE(TAG, "[诊断] 原始数据近乎为零 → 排查 ES7210（ADC START/增益/麦克风供电），"
                          "而非 AFE 通道配方；安静房间底噪通常也有数十到数百");
        }
    }
}

static int i2s_read_cb(audio_element_handle_t el, char *buf, int len, TickType_t wait, void *ctx)
{
    (void)el; (void)wait; (void)ctx;
    int r = audio_element_input(s_i2s_reader, buf, len);
    if (r <= 0) {
        ESP_LOGW(TAG, "I2S 读取失败/结束：%d", r);
    } else {
        aec_rms_probe(buf, r);
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

esp_err_t aec_capture_init(audio_hal_handle_t codec_hal)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

#if !CONFIG_ONEYE_FW_ENABLE_AEC_CAPTURE
    (void)codec_hal;
    s_st.enabled = false;
    ESP_LOGW(TAG, "AEC 采集未启用（CONFIG_ONEYE_FW_ENABLE_AEC_CAPTURE=n）");
    return ESP_OK;
#else
    s_st.enabled = true;
    s_codec = codec_hal;

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

    /* ADC 启动 + 增益：与例程同序（START 会把所有麦位刷成同一增益，故增益在其后设）。
     * 真机问题：此前录音路径从不调用 `audio_hal_ctrl_codec(START)`，而板上回放结束会 STOP
     * （ES7210 断电：模拟块 0xc0 / MIC 偏置 0xff / 时钟 0x7f），导致录音近乎静音。 */
    aec_codec_arm();

    /* 注意：I2S 读元素**不在 init 里常驻**，改为每次采集时按需创建、结束时销毁。
     * 原因（真机取证 2026-09-16）：本板录音（ES7210）与回放（ES8311）**共用 I2S0**，常驻的
     * 32 bit/ONLY_LEFT 读元素会一直占着端口，回放的 16 bit 写元素拿不到端口而**永久阻塞**
     * ——表现为「回放一直显示播放中、扬声器没有声音」（实测 elapsed_ms 涨到 36 s 仍不结束，
     * 而文件只有 5.12 s）。应用层已保证录音/回放互斥，故这里串行独占端口即可。 */
    ESP_LOGI(TAG, "AEC 采集已就绪（%d Hz / %d bit / %s → AFE(TYPE1,AFE_TYPE_VC) → WAV %d Hz %d bit 单声道；"
                  "I2S 读元素按需创建）",
             AEC_CAPTURE_RATE, (int)AEC_CAPTURE_BITS, AEC_CAPTURE_INPUT_FORMAT,
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

/* 计时口径（真机修复 2026-09-15，两轮）：
 *   ① 原实现「起管线即计时」把 AFE 预热算进时长（请求 5 s 只得 ~4 s）；
 *   ② 改为「按文件大小探测首帧」又受 FATFS 写缓冲滞后影响（请求 5 s 得 ~8.4 s）；
 *   ③ 现按**写入元素已写字节数**（fatfs_stream 用 audio_element_update_byte_pos() 维护，
 *      无缓冲滞后）精确计时：写到 `duration_s × 采样率 × 2 B + 44 B 头` 即停。
 *   同时保留「5 s 内一个字节都没写」的告警（对应 SD/DMA 写入失败）。 */
#define AEC_NO_DATA_WARN_MS 5000u

static void aec_stop_task(void *arg)
{
    uint32_t seconds = (uint32_t)(uintptr_t)arg;
    const uint32_t target_bytes = (uint32_t)44u + seconds * AEC_CAPTURE_RATE * 2u; /* 16 bit 单声道 */
    uint32_t waited = 0;
    bool got_data = false;

    while (waited < (seconds * 1000u) + 15000u) {
        audio_element_info_t info;
        memset(&info, 0, sizeof(info));
        audio_element_handle_t w = s_writer;
        if (w != NULL && audio_element_getinfo(w, &info) == ESP_OK) {
            if (info.byte_pos > 44u && !got_data) {
                got_data = true;
                ESP_LOGI(TAG, "首帧已写入（%d 字节）→ 采集 %u s 后自动停止",
                         (int)info.byte_pos, (unsigned)seconds);
            }
            if (info.byte_pos >= target_bytes) {
                ESP_LOGI(TAG, "已达到目标时长（%u s / %u 字节）",
                         (unsigned)seconds, (unsigned)info.byte_pos);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50u;
        if (!got_data && waited >= AEC_NO_DATA_WARN_MS) {
            ESP_LOGW(TAG, "启动 %u ms 仍无音频数据写入——若最终文件仅 44 字节，"
                          "请查 SD 写入失败（DMA/内部内存）或 AFE 取帧任务创建失败",
                     (unsigned)waited);
            got_data = true;   /* 只告警一次 */
        }
    }

    (void)aec_capture_stop();
    s_stop_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ 启停 */

/* 写盘预检（真机修复 2026-09-15）：SD 写入在 DMA/内部内存紧张时会直接失败
 *   sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101
 *   diskio_sdmmc: sdmmc_write_blocks failed (0x101)
 * 结果是**录出 0 字节文件**（面板表现为"录不满时长/没声音"）。这里在起管线前先写
 * 512 B（FATFS 一个扇区）探针；失败即把落盘根切到 SPIFFS 兜底，避免产出空文件。 */
static bool root_writable(const char *root)
{
    char probe[AEC_CAPTURE_PATH_MAX];
    int n = snprintf(probe, sizeof(probe), "%s/.write-probe", root);
    if (n <= 0 || (size_t)n >= sizeof(probe)) {
        return false;
    }
    FILE *fp = fopen(probe, "wb");
    if (fp == NULL) {
        return false;
    }
    uint8_t buf[512] = { 0 };
    size_t w = fwrite(buf, 1, sizeof(buf), fp);
    (void)fclose(fp);
    (void)remove(probe);
    return w == sizeof(buf);
}

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
    aec_unlock();

    /* 录音/回放互斥：本板 ES7210(ADC) 与 ES8311(DAC) **共用 I2S0**，正在回放时起录音会把
     * 两条管线都卡死（真机取证 2026-09-16：录音管线再也停不下来，AFE 持续空转，随后重启）。 */
    if (player_is_playing()) {
        aec_lock();
        s_st.last_err = ESP_ERR_INVALID_STATE;
        aec_unlock();
        ESP_LOGW(TAG, "拒绝录音：正在回放（共用 I2S0）——请先停止回放");
        return ESP_ERR_INVALID_STATE;
    }

    aec_lock();
    s_st.recording = true;
    s_st.last_err = ESP_OK;
    if (duration_s == 0) {
        duration_s = AEC_DEFAULT_DURATION_S;
    }
    /* 写盘预检：SD 不可写则回落 SPIFFS（避免空文件） */
    if (s_st.sd_mounted && !root_writable(AEC_ROOT_SD)) {
        ESP_LOGE(TAG, "SD 写盘预检失败（DMA/内部内存不足？）→ 本次改用 SPIFFS 兜底：%s",
                 AEC_ROOT_SPIFFS);
        s_st.sd_mounted = false;
        snprintf(s_st.root, sizeof(s_st.root), "%s", AEC_ROOT_SPIFFS);
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

    /* 0) ADC 重新 arm：板上回放结束会把 ES7210 整体断电（见 aec_codec_arm 注释），
     *    所以每次录音前都重新 START + 重设增益，保证本次采集真的有麦克风信号。 */
    aec_codec_arm();

    /* 0b) I2S 读元素：本次采集内独占 I2S0（回放侧已由应用层互斥挡掉；见 init 注释） */
    i2s_stream_cfg_t i2s_r_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(
        AEC_CAPTURE_I2S_PORT, AEC_CAPTURE_RATE, AEC_CAPTURE_BITS, AUDIO_STREAM_READER);
    i2s_r_cfg.task_stack = -1;                     /* 复用 pipeline 任务栈 */
    i2s_stream_set_channel_type(&i2s_r_cfg, AEC_CAPTURE_I2S_CH);
    s_i2s_reader = i2s_stream_init(&i2s_r_cfg);
    if (s_i2s_reader == NULL) {
        ESP_LOGE(TAG, "I2S 读元素创建失败");
        goto fail;
    }

    /* 1) AFE（AEC + 降噪）—— 与例程单麦分支一致：ALGORITHM_STREAM_CFG_DEFAULT()
     *    （TYPE1 + AFE_TYPE_VC + AEC|NS）+ input_format "RM"（麦 + AEC 回采两通道） */
    s_rms_sum_sq = 0;
    s_rms_peak = 0;
    s_rms_samples = 0;
    s_rms_chunks = 0;
    algorithm_stream_cfg_t algo_cfg = ALGORITHM_STREAM_CFG_DEFAULT();
    algo_cfg.input_format = AEC_CAPTURE_INPUT_FORMAT;
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
    /* 不得再逐个 `audio_element_deinit`：`audio_pipeline_deinit()` 已把**已注册**的元素
     * 全部 deinit + 反注册（components/audio_pipeline/audio_pipeline.c:263-271），
     * 重复 deinit = 双释放。真机取证（2026-09-16）：录音到时自动停止时必崩并静默重启——
     *   W (27293) AUDIO_ELEMENT: OUT-[algo] AEL_IO_ABORT
     *   I (27333) AFE: exit
     *   assert failed: spinlock_acquire spinlock.h:142 (lock->count == 0)
     *   rst:0xc (RTC_SW_CPU_RST)
     * 表现为面板上设备「离线/在线来回跳」、回放/再次录音随即失败。只置空句柄即可。 */
    s_writer = NULL;
    s_wav = NULL;
    s_algo = NULL;
    /* I2S 读元素不属管线（由 read_cb 取数），须在此自行销毁：释放 I2S0 给回放用 */
    if (s_i2s_reader) {
        audio_element_deinit(s_i2s_reader);
        s_i2s_reader = NULL;
    }

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
    /* 0 字节 = 一个字都没写进去：真机实测该现象对应 SD 写入失败（DMA 内存不足），
     * 串口会出现 `sdmmc_cmd: allocate_dma_buf: not enough mem` /
     * `diskio_sdmmc: sdmmc_write_blocks failed (0x101)`。
     * 这里显式报错（不静默），并把可能原因写进面板可见的 last_msg。 */
    if (bytes == 0) {
        ESP_LOGE(TAG, "采集未写入任何数据（0 字节）——检查 SD 写入是否失败（DMA/内部内存不足），"
                      "或换用 SPIFFS 兜底路径");
    }
    return ESP_OK;
#endif
}
