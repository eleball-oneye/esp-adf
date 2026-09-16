/*
 * camera_api.h —— 板载摄像头（OV3660，SCCB 0x3c）本地验证面
 *
 * ⚠️ 边界（与 panel_api/lcd_ui 同口径）：只服务**台面/联调验证**（抓一帧、落盘、网页可看），
 * 不是云端设备面契约的一部分：不新增 topic/影子键/能力位；抓到的图落 SD 卡 `cam/` 目录，
 * 走既有 `/media/<alias>/<path>` 只读面（带 Range）给网页显示。
 *
 * 引脚与型号来源：board_def.h 的 CAM_PIN_*（XCLK=40/SIOD=17/SIOC=18/D0..D7=13,47,14,3,12,42,41,39/
 * VSYNC=21/HREF=38/PCLK=11）；寄存器口径照抄 ADF 例程 `examples/display/lcd_camera`
 * （xclk 40 MHz、FRAMESIZE_QVGA、JPEG quality 12）——同轮真机已用该例程实测识别到
 * `Camera PID=0x3660 / Detected OV3660 camera / Detected camera at address=0x3c`。
 */

#ifndef _CAMERA_API_H_
#define _CAMERA_API_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 输出像素格式（JPEG = sensor 内编码；RGB565 = 原始帧 + 软件转 JPEG 落盘） */
typedef enum {
    CAMERA_OUT_JPEG = 0,
    CAMERA_OUT_RGB565 = 1,
} camera_fmt_t;

/** 抓帧结果（供 /api/camera/capture 与 /api/status 序列化） */
typedef struct {
    bool     ok;
    char     path[96];     /* 设备内路径（形如 /sdcard/cam/cap-00001.jpg） */
    size_t   bytes;
    int      width;
    int      height;
    uint32_t ms;           /* 本次抓帧 + 落盘耗时（设备运行毫秒差） */
    char     err[128];     /* 失败原因（成功为空串）——失败时字段必须可读，禁止让调用方拿到未初始化内存 */
} camera_capture_t;

/** 摄像头状态快照 */
typedef struct {
    bool     inited;
    bool     sd_ok;         /* 抓到 SD 卡（否则 SPIFFS 兜底） */
    char     sensor[16];    /* 型号名（如 OV3660），未识别为 "" */
    int      pid;           /* sensor PID（0x3660 等；未识别为 -1） */
    uint32_t frames;        /* 累计成功抓帧数 */
    uint32_t errors;        /* 累计失败数 */
    size_t   last_bytes;
    int      last_width;
    int      last_height;
    uint32_t last_ms;       /* 最近一次抓帧时刻（设备运行毫秒） */
    char     last_path[96];
    char     root[24];      /* 落盘根目录（/sdcard/cam 或 /spiffs/cam） */
    char     last_err[128]; /* 最近一次失败原因（未失败为空串） */
    /* 生效中的配置（本地验证面排障用：NO-SOI 类问题靠这几个旋钮定位） */
    char     format[8];     /* "jpeg" | "rgb565" */
    char     fb_loc[8];     /* "dram" | "psram" */
    int      fb_count;      /* 1..3 */
    char     grab[12];      /* "when_empty" | "latest" */
    int      xclk_mhz;      /* 10/20/40 */
    int      quality;       /* JPEG quality 0..63 */
    int      psram_dma;     /* 1 = 帧缓冲直接作 DMA 目标（省内部 DMA 缓冲）；0 = 走内部 dma_buffer */
    /* MJPEG 预览流（本地验证面：面板「开始预览」用） */
    int      stream_port;   /* 0 = 未启动 */
    uint32_t stream_frames; /* 已发送帧数 */
    int      stream_clients;/* 当前连接的预览客户端数（0/1） */
} camera_state_t;

/** 重配参数：任一项 <0 / 非法 = 保持当前值（见 camera_api_apply） */
typedef struct {
    int fb_location;    /* 0=DRAM 1=PSRAM；-1=不改 */
    int pixel_format;   /* camera_fmt_t；-1=不改 */
    int fb_count;       /* 1..3；-1=不改 */
    int grab_mode;      /* 0=WHEN_EMPTY 1=LATEST；-1=不改 */
    int xclk_mhz;       /* 10/20/40；-1=不改 */
    int jpeg_quality;   /* 0..63；-1=不改 */
    int psram_dma;      /* 0/1；-1=不改 */
} camera_cfg_t;

/** 初始化摄像头（power on + SCCB 探测 + 配置）；失败返回非 0（错误码即"探测失败"依据） */
esp_err_t camera_api_init(void);

/** 以当前默认值 + 覆盖项重新初始化（会 deinit 再 init；越界项按"不改"处理） */
esp_err_t camera_api_apply(const camera_cfg_t *cfg);

/** 是否已就绪 */
bool camera_api_ready(void);

/** 抓一帧并落盘（JPEG）；path_out 为空时自动命名 cap-%05u.jpg */
esp_err_t camera_api_capture(camera_capture_t *out);

/** 抓一帧并**就地**编码为 JPEG（不落盘）；成功时 *out 为 `malloc` 缓冲，调用方负责 `free()` */
esp_err_t camera_api_grab_jpeg(uint8_t **out, size_t *out_len, int *width, int *height);

/** 启动 MJPEG 预览流（**独立 httpd 实例**，避免长时间占住主验证面服务器的任务）：
 *  `GET http://<设备>:<port>/stream`，`multipart/x-mixed-replace`，浏览器 `<img>` 直接显示。
 *  port=0 用缺省 81。失败返回错误码（面板据此回落到「单帧轮询预览」）。 */
esp_err_t camera_api_stream_start(uint16_t port);

/** 预览流状态（未启动时 port=0） */
void camera_api_stream_status(uint16_t *port, uint32_t *frames, int *clients);

/** 状态快照 */
void camera_api_get_state(camera_state_t *out);

#ifdef __cplusplus
}
#endif

#endif /* _CAMERA_API_H_ */
