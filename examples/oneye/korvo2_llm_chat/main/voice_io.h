/*
 * voice_io.h —— 音频出入口（ESP-ADF）：AFE 采集上行 / PCM 回放下行
 *
 * 职责边界：
 *   - **只做音频**：不做 WebSocket、不做按键、不做配网；
 *   - 上行：I2S0(ES7210 ADC) → AFE（AEC + 降噪）→ 单声道 16 kHz/16 bit PCM 回调；
 *   - 下行：16 kHz/16 bit 单声道 PCM → I2S0(ES8311 DAC) → 喇叭（内部扩为立体声）。
 *
 * 板级口径（**照抄** examples/oneye/korvo2_oneye 已真机取证的单麦分支，勿臆改）：
 *   - 本板 ES7210(ADC) 与 ES8311(DAC) **共用 I2S0** ⇒ 采集与回放**互斥**（本模块强制）；
 *   - 采集：I2S 16 kHz / 32 bit / `I2S_CHANNEL_FMT_ONLY_LEFT` + AFE `input_format = "RM"`；
 *   - 每次采集前必须重新 `audio_hal_ctrl_codec(ENCODE, START)` 并重设增益
 *     （回放结束会 STOP 编解码器，把 ES7210 整体断电）。
 *
 * 契约口径（backend/contracts/api/ws/voice-frames.json）：PCM 16 kHz / 16 bit / 单声道 / 60 ms
 *   ⇒ 单帧 1920 B（VOICE_IO_FRAME_BYTES）。
 *
 * ⚠️ 已知限制：ESP32-S3 上 ADF 的 `i2s_mono_fix()` 不参与编译（`#ifdef CONFIG_IDF_TARGET_ESP32`），
 *   故回放**由本模块自行**把单声道复制成立体声（不能依赖 i2s_stream 自动处理）。
 */

#ifndef _VOICE_IO_H_
#define _VOICE_IO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "audio_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 契约单帧字节数：16 kHz × 16 bit × 1 ch × 60 ms / 1000 = 1920 B */
#define VOICE_IO_FRAME_BYTES 1920

/* 上行 PCM 回调（在采集任务上下文执行；**禁止阻塞**——回调内只做入队/发送） */
typedef void (*voice_io_pcm_cb_t)(const void *pcm, size_t len, void *ctx);

typedef struct {
    bool     capturing;        /* 正在采集 */
    bool     playing;          /* 正在回放 */
    uint32_t up_frames;        /* 上行回调次数 */
    uint32_t up_bytes;         /* 上行累计字节 */
    uint32_t down_bytes;       /* 下行累计写入字节（单声道口径） */
    uint32_t down_drops;       /* 下行写入超时丢弃次数（回放缓冲满） */
    int      volume;           /* 当前音量 */
    esp_err_t last_err;        /* 最近一次错误 */
} voice_io_status_t;

/** 初始化：仅记录音频句柄与创建互斥量（不解码、不开管线） */
esp_err_t voice_io_init(audio_hal_handle_t codec_hal);

/* ------------------------------------------------------------------ 上行（采集） */

/** 开始采集：内部创建 I2S → AFE → raw 管线，并按 VOICE_IO_FRAME_BYTES 粒度回调
 *  @note 回放中调用返回 ESP_ERR_INVALID_STATE（共用 I2S0） */
esp_err_t voice_io_capture_start(voice_io_pcm_cb_t cb, void *ctx);

/** 停止采集并释放管线；重复调用安全 */
esp_err_t voice_io_capture_stop(void);

bool voice_io_is_capturing(void);
size_t voice_io_captured_bytes(void);

/* ------------------------------------------------------------------ 下行（回放） */

/** 打开回放：启动编解码器 DAC、创建 raw → I2S 管线 */
esp_err_t voice_io_playback_open(void);

/** 写一段单声道 PCM（内部扩立体声；缓冲满则超时丢弃并计数） */
esp_err_t voice_io_playback_write(const void *pcm, size_t len);

/** 关闭回放（停管线、停编解码器）；重复调用安全 */
esp_err_t voice_io_playback_close(void);

bool voice_io_is_playing(void);

/* ------------------------------------------------------------------ 其他 */

esp_err_t voice_io_set_volume(int volume);
void voice_io_get_status(voice_io_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* _VOICE_IO_H_ */
