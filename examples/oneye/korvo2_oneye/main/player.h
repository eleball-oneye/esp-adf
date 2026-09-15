/*
 * player.h —— SD 卡媒体**板上回放**（验证面板"选文件 → 板上播放"的后端）
 *
 * 链路（与本板 codec 口径一致）：`/sdcard/<file>` → fatfs_stream(reader) → 解码器（按扩展名）
 *   → i2s_stream(writer) → ES8311 → NS4150 → 扬声器
 *
 * 支持范围（本轮）：
 *   - **存源**：仅 **SD 卡（FATFS）** —— ADF 的 fatfs_stream 读不了 SPIFFS；
 *     SPIFFS 里的 AEC 录音（无 SD 时的兜底路径）只能**网页播放/下载**，不上板播放；
 *   - **格式**：WAV / MP3（最常见）；其他扩展名返回 `ESP_ERR_NOT_SUPPORTED`（不静默失败）；
 *   - 采样率/声道以解码器上报的 `MUSIC_INFO` 为准，动态重设 I2S 时钟（避免 WAV 16 kHz 被当 48 kHz 播快）。
 *
 * 边界：与 /api、/media 同属**本地验证面**（非云端契约），写操作仅台面验证用。
 */

#ifndef _PLAYER_H_
#define _PLAYER_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "audio_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYER_PATH_MAX 96

typedef struct {
    bool     playing;         /* 是否正在播放 */
    bool     volume_known;
    uint8_t  volume;          /* 0–100 */
    char     path[PLAYER_PATH_MAX];   /* 当前/最近播放的文件（设备侧路径） */
    char     codec[8];        /* wav | mp3 | - */
    uint32_t rate_hz;         /* 解码器上报采样率 */
    uint8_t  channels;
    uint32_t elapsed_ms;      /* 本次播放已持续 */
    esp_err_t last_err;       /* ESP_OK = 无错误 */
    char     last_msg[48];    /* 人类可读状态/错误 */
} player_status_t;

/** 初始化（传入 board handle 以启停 codec；board 可为 NULL 时只建管线） */
esp_err_t player_init(audio_hal_handle_t codec_hal);

bool player_is_playing(void);

/** 播放指定文件（设备侧路径，必须位于 /sdcard/ 下）；正在播放时先停止再播放 */
esp_err_t player_play(const char *path);

/** 停止播放（幂等） */
esp_err_t player_stop(void);

/** 设置音量 0–100（作用于 codec） */
esp_err_t player_set_volume(int volume);

void player_get_status(player_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* _PLAYER_H_ */
