/*
 * aec_capture.h —— AEC 采集（esp-sr AFE：AEC + 降噪）→ WAV 落盘，供验证面板在 web 播放
 *
 * 口径（与 ADF 上游 `examples/advanced_examples/algorithm` 对齐，参数取自本板文档）：
 *   I2S0（CODEC_ADC_I2S_PORT）16 kHz / 32 bit / RIGHT_LEFT  ← ES7210 四通道按 `"RMNM"` 排布
 *     → algorithm_stream（AFE_TYPE_SR 双麦，ALGORITHM_STREAM_CFG_SR_DUAL_MIC）
 *     → wav_encoder（16 kHz / 16 bit / 单声道）
 *     → fatfs_stream 落盘
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

/** 初始化：设置 ADC 增益、创建 I2S 读元素、挂载 SPIFFS（SD 由主流程按开关挂载） */
esp_err_t aec_capture_init(void);

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
