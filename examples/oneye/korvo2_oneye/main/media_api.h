/*
 * media_api.h —— 本地验证面的**媒体与动作**部分（验证面板用）
 *
 * 路由（注册到 panel_api 的同一 httpd 实例）：
 *   GET  /media/list            列出可播放文件（SD 与 SPIFFS 兜底存储）
 *   GET  /media/<alias>/<path>  文件下载/流式播放，**支持 Range**（浏览器可拖动进度）
 *                               alias = sdcard | spiffs（映射到 /sdcard 与 /spiffs）
 *   POST /api/action            {"op":"aec_start","duration_s":N} | {"op":"aec_stop"}
 *
 * 边界：与 /api 下的只读路由同属**本地验证面**（非云端契约）；写动作（录音启停）**仅**用于台面验证，
 *       量产固件应整体关闭（CONFIG_ONEYE_FW_ENABLE_PANEL_API=n）。
 */

#ifndef _MEDIA_API_H_
#define _MEDIA_API_H_

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 把媒体路由（/media/list 与 /media/&lt;alias&gt;/&lt;path&gt;）及 /api/action 注册到已有 httpd（由 panel_api_start 调用） */
esp_err_t media_api_register(httpd_handle_t httpd);

#ifdef __cplusplus
}
#endif

#endif /* _MEDIA_API_H_ */
