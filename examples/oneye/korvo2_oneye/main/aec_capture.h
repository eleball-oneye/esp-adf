/*
 * aec_capture.h —— AEC 采集（esp-sr AFE：AEC + 降噪）→ WAV 落盘，供验证面板在 web 播放
 *
 * 口径（**逐项照抄** ADF 上游 `examples/advanced_examples/algorithm` 在本板
 * `CONFIG_ESP32_S3_KORVO2_V3_BOARD` + `RECORD_HARDWARE_AEC == true` 下的**单麦**分支）：
 *   I2S0（CODEC_ADC_I2S_PORT）16 kHz / 32 bit / `I2S_CHANNEL_FMT_ONLY_LEFT`
 *     → algorithm_stream（`ALGORITHM_STREAM_CFG_DEFAULT()` = TYPE1 + AFE_TYPE_VC + AEC|NS，
 *        `input_format = "RM"` 即 2 通道：1 路麦 + 1 路 AEC 回采）
 *     → wav_encoder（16 kHz / 16 bit / 单声道）
 *     → fatfs_stream 落盘
 *
 * 说明：本板 `board_def.h` 的 `RECORD_HARDWARE_AEC (true)` 使例程走单麦分支；其**双麦**分支才用
 * `"RMNM"`（4 通道）+ `I2S_CHANNEL_FMT_RIGHT_LEFT` + `AFE_TYPE_SR`。两者混用（4 通道 I2S 喂
 * 单麦 AFE）会让 AFE 报 "only support single microphone channel" 并只取首通道，易录到近乎静音。
 *
 * 另：录音前必须 `audio_hal_ctrl_codec(..., AUDIO_HAL_CTRL_START)`（例程同款）。ES7210 的 STOP
 * 会把 MIC 偏置/模拟块整体断电（`es7210_stop()`：0x47~0x4A=0xff、REG40=0xc0、REG01=0x7f），
 * 板上回放结束（player teardown）会 STOP，故每次录音前都要重新 START 并重设增益。
 *
 * 存储：SD 卡可用时 `/sdcard/rec/aec-<seq>.wav`；不可用时 SPIFFS 兜底 `/spiffs/rec/aec-<seq>.wav`
 *      （两处都由 `GET /media/list` 与 `GET /media/<path>` 提供 web 播放/下载）。
 *
 * 边界：采集控制与媒体面属**本地验证面**（配合验证面板使用），不是云端设备面契约。
 */

#ifndef _AEC_CAPTURE_H_
#define _AEC_CAPTURE_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "audio_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AEC_CAPTURE_PATH_MAX 64

typedef struct {
    bool     enabled;          /* 编译期是否启用（Kconfig） */
    bool     recording;        /* 正在采集 */
    bool     sd_mounted;       /* SD 可用（否则用 SPIFFS 兜底） */
    char     root[24];         /* 当前落盘根目录（/sdcard/rec 或 /spiffs/rec） */
    char     last_file[AEC_CAPTURE_PATH_MAX]; /* 最近一次录音的完整路径（"" = 尚无） */
    uint32_t last_bytes;       /* 最近一次录音字节数 */
    uint32_t last_duration_ms; /* 最近一次录音耗时 */
    uint32_t total_files;      /* 累计录音文件数（本次上电） */
    esp_err_t last_err;        /* 最近一次失败原因（ESP_OK = 无） */
} aec_capture_status_t;

/** 初始化：启动编解码器（ADC START）、设置 ADC 增益、创建 I2S 读元素、挂载 SPIFFS
 *  @param codec_hal 板级 audio_hal 句柄（`audio_board_init()->audio_hal`）；与例程一致，
 *                   为 NULL 时仅告警（此时录音幅度可能异常，面板可见 last_msg） */
esp_err_t aec_capture_init(audio_hal_handle_t codec_hal);

bool aec_capture_enabled(void);
bool aec_capture_is_recording(void);

/** 开始采集；duration_s=0 取 Kconfig 缺省（到时自动停止并落盘）
 *  @param out_path 若非空，回写本次落盘路径（供 /api/action 立即回执） */
esp_err_t aec_capture_start(uint32_t duration_s, char *out_path, size_t cap);

/** 手动停止（未到时长时）；重复调用安全 */
esp_err_t aec_capture_stop(void);

void aec_capture_get_status(aec_capture_status_t *out);

/** SD 挂载状态（由主流程在尝试挂载后告知；决定落盘根目录与兜底策略） */
void aec_capture_set_sd_mounted(bool mounted);

/** 采集根目录（供媒体列表 API 使用） */
const char *aec_capture_root(void);

#ifdef __cplusplus
}
#endif

#endif /* _AEC_CAPTURE_H_ */
