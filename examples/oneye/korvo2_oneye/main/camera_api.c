/*
 * camera_api.c —— 板载摄像头抓帧（JPEG）+ 落盘 + 状态上报
 *
 * 寄存器与引脚口径照抄 ADF 例程 `examples/display/lcd_camera`（同一板级 CAM_PIN_*、
 * xclk 40 MHz、QVGA、JPEG quality 12）；差异只在用途：本模块把帧写进 SD（或 SPIFFS 兜底）
 * 而不是刷 LCD，从而经既有 `/media/<alias>/<path>` 在网页里看到 —— 抓拍与网页显示复用同一
 * 套只读面，不新增对外形态。
 *
 * 排障记录（2026-09-16 真机）：
 *   首次烧写用 `fb_count=1 + CAMERA_GRAB_LATEST + PIXFORMAT_JPEG + fb 在 PSRAM`，
 *   串口持续 `cam_hal: NO-SOI - JPEG start marker missing` → `Failed to get frame: timeout`
 *   ⇒ esp_camera_fb_get() 恒返回 NULL。上游 ADF 例程实测可用配置是
 *   `PIXFORMAT_RGB565 + fb_count=2 + CAMERA_GRAB_WHEN_EMPTY + fb 在 DRAM（未指定 fb_location）`，
 *   故本模块：① 默认值与上游对齐（DRAM + RGB565，编码交给软件 frame2jpg）；
 *   ② 保留 `/api/camera/reinit` 运行时旋钮（fb_location / 像素格式 / fb_count / grab_mode /
 *      xclk / quality）用于在**同一块板子上**对比定位，不靠反复烧写猜。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_cache.h"

#include "esp_camera.h"
#include "img_converters.h"     /* frame2jpg（RGB565 → JPEG，软件编码） */

#include "board.h"          /* CAM_PIN_*（board_def.h） */
#include "camera_api.h"

static const char *TAG = "camera_api";

/* 与上游 ADF 例程一致：QVGA=320×240；帧编码 JPEG quality 12 */
#define CAM_FRAME_SIZE   FRAMESIZE_QVGA
#define CAM_JPEG_QUALITY 12
/* ⚠️ XCLK 10 MHz（**不是**上游例程的 40 MHz）——真机实测定下的口径：
 *   40 MHz → PCLK 20 MHz（≈40 MB/s）时，**只要面板在并发轮询 /api/status**（约每 600 ms 一次 HTTP）
 *   抓到的帧就会出现水平彩带（DVP 数据在 DVP→PSRAM 路径上被丢字节 ⇒ 整行错位 + 假彩色）；
 *   面板停止轮询时同一配置干净 ⇒ 与"并发网络负载 + 高数据率"强相关。
 *   逐档实测（都在并发 HTTP 下）：QVGA/40 花、QVGA/20 花、QQVGA/40 花、QQVGA/20 干净、**QVGA/10 干净**。
 *   ⇒ 保留 320×240 分辨率，把 XCLK 降到 10 MHz（PCLK 5 MHz ≈10 MB/s）即可稳定；
 *   代价是帧率下降（抓帧/预览仍够验证用）。旋钮：/api/camera/reinit?xclk=10|20|40。 */
#define CAM_XCLK_HZ      10000000

/* 抓帧策略（第二十轮引入，用于抑制真机间歇性水平噪带；详见文件末「抓帧对策」注释）：
 * capture/snapshot 连采 3 帧挑最干净的一帧；stream 连采 2 帧（帧率 7.8 → ~4 帧/s 的取舍）。
 * 评分低于 CAM_NOISE_GOOD 即认为足够干净，提前结束采样。 */
#define CAM_GRAB_TRIES_CAPTURE 3
#define CAM_GRAB_TRIES_STREAM  2
#define CAM_NOISE_GOOD         60

static esp_err_t cam_grab_jpeg_tries(uint8_t **out, size_t *out_len, int *width, int *height,
                                     int tries);

/* sensor PID → 型号名（esp32-camera 的 sensor.h 里 PID 常量；此处只做展示用映射） */
static const char *cam_pid_name(int pid)
{
    switch (pid) {
    case 0x3660: return "OV3660";
    case 0x2640: return "OV2640";
    case 0x5640: return "OV5640";
    case 0x7725: return "OV7725";
    case 0x2145: return "GC2145";
    case 0x232a: return "GC032A";
    case 0x9b:   return "GC0308";
    case 0x1410: return "NT99141";
    case 0x30:   return "BF3005";
    default:     return "UNKNOWN";
    }
}

static bool            s_inited;
static bool            s_sd_ok = true;          /* 默认按 SD 可用；失败后转 SPIFFS */
static char            s_root[24] = "/sdcard/cam";
static uint32_t        s_frames;
static uint32_t        s_errors;
static uint32_t        s_seq;
static camera_state_t  s_last;                  /* 最近一次成功结果（用于状态上报） */

/* MJPEG 预览流状态（独立 httpd 实例；见文件末「预览流」一节） */
static httpd_handle_t    s_stream_httpd;
static uint16_t          s_stream_port;
static volatile uint32_t s_stream_frames;
static volatile int      s_stream_clients;
static char              s_stream_end_reason[64];   /* 上一次流结束的原因（正常/发送失败） */
static uint32_t          s_stream_end_count;

/* 生效配置（可经 camera_api_apply 改）——默认口径见文件头排障记录：
 * RGB565 原始帧（上游 lcd_camera 在本板实测可用的像素格式）+ fb 在 PSRAM（不占内部 DRAM 带宽/容量，
 * 因为本固件还要跑 Wi-Fi/云链路/AEC/HTTP），落盘前用 frame2jpg 做软件 JPEG 编码。 */
static int s_fb_location = CAMERA_FB_IN_PSRAM;
static int s_pixel_format = PIXFORMAT_RGB565;
static int s_fb_count = 2;
static int s_grab_mode = CAMERA_GRAB_WHEN_EMPTY;
static int s_xclk_hz = CAM_XCLK_HZ;
static int s_quality = CAM_JPEG_QUALITY;
/*
 * PSRAM DMA（帧缓冲直接作 DMA 目标）：**默认开启**，理由是内部 DMA 内存不够。
 * 真机实证（2026-09-16）：`cam_hal: PSRAM DMA mode disabled` 时驱动会额外申请
 * `dma_buffer_size=30720 B` 的**内部 DMA** 缓冲（见 cam_hal.c:522，仅 psram_mode=false 分支），
 * 加上 Wi-Fi/AEC/云链路后内部 RAM 只剩 ~18 KB、最大连续块 7.6 KB，
 * 于是 SDK 上云直接 `base start 失败：out of memory`（串口实证）。
 * 开启后帧缓冲直接在 PSRAM 上被 DMA 写，内部 DMA 缓冲退回（实测 internal_free 18 KB → 约 48 KB）。
 */
static int s_psram_dma = 1;
/* 帧尺寸：真机实测在**并发负载**下 QVGA(320×240) 会间歇出现水平噪带（PSRAM/DVP 争用），
 * QQVGA(160×120) 数据量只有 1/4，明显更稳；默认仍是 QVGA（清晰度优先），面板可用
 * /api/camera/reinit?size=qqvga 切换，抓帧质量与"预览稳定性"可现场权衡。 */
static int s_frame_size = FRAMESIZE_QVGA;

static esp_err_t cam_mkdir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return ESP_OK;
    }
    if (mkdir(path, 0777) == 0) {
        return ESP_OK;
    }
    /* 仍失败：交给 fopen 判定（有些 VFS 不实现 stat/mkdir 组合语义） */
    return ESP_FAIL;
}

static void cam_snapshot_cfg(void)
{
    snprintf(s_last.format, sizeof(s_last.format), "%s",
             s_pixel_format == PIXFORMAT_JPEG ? "jpeg" : "rgb565");
    snprintf(s_last.fb_loc, sizeof(s_last.fb_loc), "%s",
             s_fb_location == CAMERA_FB_IN_PSRAM ? "psram" : "dram");
    s_last.fb_count = s_fb_count;
    snprintf(s_last.grab, sizeof(s_last.grab), "%s",
             s_grab_mode == CAMERA_GRAB_LATEST ? "latest" : "when_empty");
    s_last.xclk_mhz = s_xclk_hz / 1000000;
    s_last.quality = s_quality;
    s_last.psram_dma = s_psram_dma;
    switch (s_frame_size) {
    case FRAMESIZE_QQVGA: snprintf(s_last.size, sizeof(s_last.size), "%s", "qqvga"); break;
    case FRAMESIZE_VGA:   snprintf(s_last.size, sizeof(s_last.size), "%s", "vga");   break;
    default:              snprintf(s_last.size, sizeof(s_last.size), "%s", "qvga");  break;
    }
}

esp_err_t camera_api_apply(const camera_cfg_t *cfg)
{
    if (s_inited) {
        (void)esp_camera_deinit();
        s_inited = false;
    }

    if (cfg != NULL) {
        if (cfg->fb_location == CAMERA_FB_IN_DRAM || cfg->fb_location == CAMERA_FB_IN_PSRAM) {
            s_fb_location = cfg->fb_location;
        }
        if (cfg->pixel_format == PIXFORMAT_JPEG || cfg->pixel_format == PIXFORMAT_RGB565) {
            s_pixel_format = cfg->pixel_format;
        }
        if (cfg->fb_count >= 1 && cfg->fb_count <= 3) {
            s_fb_count = cfg->fb_count;
        }
        if (cfg->grab_mode == CAMERA_GRAB_WHEN_EMPTY || cfg->grab_mode == CAMERA_GRAB_LATEST) {
            s_grab_mode = cfg->grab_mode;
        }
        if (cfg->xclk_mhz == 10 || cfg->xclk_mhz == 20 || cfg->xclk_mhz == 40) {
            s_xclk_hz = cfg->xclk_mhz * 1000000;
        }
        if (cfg->jpeg_quality >= 0 && cfg->jpeg_quality <= 63) {
            s_quality = cfg->jpeg_quality;
        }
        if (cfg->psram_dma == 0 || cfg->psram_dma == 1) {
            s_psram_dma = cfg->psram_dma;
        }
        if (cfg->frame_size >= 0) {
            s_frame_size = cfg->frame_size;
        }
    }

    camera_config_t c = { 0 };
    c.pin_pwdn = CAM_PIN_PWDN;
    c.pin_reset = CAM_PIN_RESET;
    c.pin_xclk = CAM_PIN_XCLK;
    c.pin_sccb_sda = CAM_PIN_SIOD;
    c.pin_sccb_scl = CAM_PIN_SIOC;
    c.pin_d7 = CAM_PIN_D7;
    c.pin_d6 = CAM_PIN_D6;
    c.pin_d5 = CAM_PIN_D5;
    c.pin_d4 = CAM_PIN_D4;
    c.pin_d3 = CAM_PIN_D3;
    c.pin_d2 = CAM_PIN_D2;
    c.pin_d1 = CAM_PIN_D1;
    c.pin_d0 = CAM_PIN_D0;
    c.pin_vsync = CAM_PIN_VSYNC;
    c.pin_href = CAM_PIN_HREF;
    c.pin_pclk = CAM_PIN_PCLK;
    c.xclk_freq_hz = s_xclk_hz;
    c.ledc_timer = LEDC_TIMER_0;
    c.ledc_channel = LEDC_CHANNEL_0;
    c.pixel_format = (pixformat_t)s_pixel_format;
    c.frame_size = (framesize_t)s_frame_size;
    c.jpeg_quality = s_quality;
    c.fb_count = s_fb_count;
    c.fb_location = (camera_fb_location_t)s_fb_location;
    c.grab_mode = (camera_grab_mode_t)s_grab_mode;

    esp_err_t rc = esp_camera_init(&c);
    cam_snapshot_cfg();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "摄像头初始化失败（%s）：SCCB 探测不到 sensor ⇒ 按「未装配/接线问题」取证（不得声明 video.*）",
                 esp_err_to_name(rc));
        s_last.pid = -1;
        snprintf(s_last.sensor, sizeof(s_last.sensor), "%s", "UNKNOWN");
        s_last.inited = false;
        return rc;
    }

    sensor_t *s = esp_camera_sensor_get();
    s_last.pid = (s != NULL) ? s->id.PID : -1;
    snprintf(s_last.sensor, sizeof(s_last.sensor), "%s", cam_pid_name(s_last.pid));

    /* 显式打开自动控制（AWB/AEC/AGC）：反复 reinit 后若不显式打开，偶发出现整体偏色
     * （真机现象：白条变粉、灰面偏绿）——传感器需要几帧才收敛，且部分寄存器会被重配流程复位。
     * 与上游 CameraWebServer 例程一致：白平衡 / 白平衡增益 / 曝光 / 增益 全开。 */
    if (s != NULL) {
        (void)s->set_whitebal(s, 1);
        (void)s->set_awb_gain(s, 1);
        (void)s->set_exposure_ctrl(s, 1);
        (void)s->set_gain_ctrl(s, 1);
        (void)s->set_brightness(s, 0);
        (void)s->set_saturation(s, 0);
    }

    /* PSRAM DMA 模式：驱动内部只在 psram_mode=false 时申请 30 KB 内部 DMA 缓冲，
     * 故此处按 s_psram_dma 显式设置（esp_camera_set_psram_mode 会用已保存配置重配一次）。 */
    esp_err_t pd_rc = esp_camera_set_psram_mode(s_psram_dma != 0);
    if (pd_rc != ESP_OK) {
        ESP_LOGW(TAG, "PSRAM DMA 模式设置失败：%s（继续按当前模式运行）", esp_err_to_name(pd_rc));
    }

    ESP_LOGI(TAG, "摄像头就绪：%s（PID=0x%04x），320x240 fmt=%s fb=%s×%d grab=%s xclk=%dMHz q=%d psram_dma=%d",
             s_last.sensor, (unsigned)s_last.pid, s_last.format, s_last.fb_loc, s_fb_count,
             s_last.grab, s_last.xclk_mhz, s_quality, s_psram_dma);

    (void)cam_mkdir("/sdcard/cam");
    if (cam_mkdir("/sdcard/cam") != ESP_OK) {
        s_sd_ok = false;
        snprintf(s_root, sizeof(s_root), "/spiffs/cam");
        (void)cam_mkdir(s_root);
        ESP_LOGW(TAG, "SD 卡 cam/ 不可用 ⇒ 落 SPIFFS（%s）", s_root);
    }
    snprintf(s_last.root, sizeof(s_last.root), "%s", s_root);
    s_inited = true;
    s_last.inited = true;
    return ESP_OK;
}

esp_err_t camera_api_init(void)
{
    return camera_api_apply(NULL);
}

bool camera_api_ready(void)
{
    return s_inited;
}

/* 落盘：把一段内存写到 path（SD → SPIFFS 兜底），返回实写字节数 */
static esp_err_t cam_write(const char *name, const uint8_t *data, size_t len,
                           char *path_out, size_t path_cap, size_t *written_out)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", s_root, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        snprintf(path, sizeof(path), "%s/%s", s_sd_ok ? "/spiffs/cam" : "/sdcard/cam", name);
        (void)cam_mkdir(s_sd_ok ? "/spiffs/cam" : "/sdcard/cam");
        f = fopen(path, "wb");
    }
    if (f == NULL) {
        ESP_LOGW(TAG, "落盘失败（%s 不可写）", path);
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != len) {
        ESP_LOGW(TAG, "落盘不完整：%u/%u B", (unsigned)written, (unsigned)len);
        return ESP_FAIL;
    }
    snprintf(path_out, path_cap, "%s", path);
    if (written_out != NULL) {
        *written_out = written;
    }
    return ESP_OK;
}

esp_err_t camera_api_capture(camera_capture_t *out)
{
    /* 先清零：失败分支也必须让调用方拿到可读结果（首版漏掉 ⇒ 网页读到未初始化栈内存） */
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (!s_inited) {
        if (out != NULL) {
            snprintf(out->err, sizeof(out->err), "not inited");
        }
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t rc = ESP_OK;
    char name[32];
    char path[96] = "";
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    uint8_t *jpg = NULL;          /* cam_grab_jpeg_tries 交回的 JPEG（调用方 free） */
    size_t payload_len = 0;
    int gw = 0, gh = 0;

    /* 统一走「连采 N 帧挑最干净一帧」：真机间歇性水平噪带的对策（见本文件顶部排障记录） */
    if (cam_grab_jpeg_tries(&jpg, &payload_len, &gw, &gh, CAM_GRAB_TRIES_CAPTURE) != ESP_OK ||
        jpg == NULL) {
        ESP_LOGW(TAG, "抓帧失败：fb 为空或编码失败（fmt=%s fb=%s×%d grab=%s xclk=%dMHz；"
                      "cam_hal 若刷 NO-SOI 见 camera_api.c 排障记录）",
                 s_last.format, s_last.fb_loc, s_fb_count, s_last.grab, s_last.xclk_mhz);
        if (out != NULL) {
            snprintf(out->err, sizeof(out->err), "%s",
                     s_last.last_err[0] ? s_last.last_err : "grab failed");
        }
        return ESP_FAIL;
    }

    s_seq++;
    snprintf(name, sizeof(name), "cap-%05u.jpg", (unsigned)s_seq);

    size_t written = payload_len;
    if (cam_write(name, jpg, payload_len, path, sizeof(path), &written) != ESP_OK) {
        rc = ESP_FAIL;
    }
    free(jpg);
    jpg = NULL;

    uint32_t el = (uint32_t)(esp_timer_get_time() / 1000) - t0;
    ESP_LOGI(TAG, "抓帧：%dx%d 编码后 %u B → %s（%u ms，fmt=%s，取样 %d 帧，彩噪评分 %u）",
             gw, gh, (unsigned)payload_len, path, (unsigned)el,
             s_last.format, s_last.last_grabs, (unsigned)s_last.last_noise);

    s_last.frames = ++s_frames;
    s_last.last_bytes = payload_len;
    s_last.last_width = gw;
    s_last.last_height = gh;
    s_last.last_ms = (uint32_t)(esp_timer_get_time() / 1000);
    snprintf(s_last.last_path, sizeof(s_last.last_path), "%s", path);
    s_last.sd_ok = (strncmp(path, "/sdcard", 7) == 0);

    if (rc == ESP_OK) {
        s_last.last_err[0] = '\0';
    } else {
        s_errors++;
        snprintf(s_last.last_err, sizeof(s_last.last_err), "write failed (%.*s)",
                 (int)sizeof(path) - 1, path);
    }

    if (out != NULL) {
        out->ok = (rc == ESP_OK);
        snprintf(out->path, sizeof(out->path), "%s", path);
        out->bytes = written;
        out->width = s_last.last_width;
        out->height = s_last.last_height;
        out->ms = el;
        if (rc != ESP_OK) {
            snprintf(out->err, sizeof(out->err), "%s", s_last.last_err);
        }
    }
    return rc;
}

void camera_api_get_state(camera_state_t *out)
{
    if (out == NULL) {
        return;
    }
    cam_snapshot_cfg();                                  /* 先刷新配置快照，再整体拷贝 */
    snprintf(s_last.root, sizeof(s_last.root), "%s", s_root);
    s_last.stream_port = (int)s_stream_port;
    s_last.stream_frames = s_stream_frames;
    s_last.stream_clients = s_stream_clients;
    s_last.stream_ends = s_stream_end_count;
    snprintf(s_last.stream_end_reason, sizeof(s_last.stream_end_reason), "%s", s_stream_end_reason);
    *out = s_last;
    out->inited = s_inited;
    out->frames = s_frames;
    out->errors = s_errors;
}

/* ---------------------------------------------------------------- 预览：抓帧 → 内存 JPEG（不落盘） */

/*
 * 真机实测（2026-09-16 第二十轮）：PSRAM-DMA 模式下抓到的帧会**间歇性**出现水平噪带
 * （≈24 行的整块数据不对 = 一个 DMA 半缓冲的量），且**与并发负载强相关**：
 * 面板停止轮询时抓帧干净，面板 600 ms 轮询 + MQTT 在跑时同一配置就会出现噪带；
 * 串口**没有** `FB-OVF`（不是 FIFO 溢出），XCLK 40→20 MHz 也无效。⇒ 判为
 * 「PSRAM 直写 + CPU 经 cache 读回」这条路径在系统繁忙时可见性不稳。
 * 两条对策（都在本文件）：
 *   ① 读之前显式 `esp_cache_msync(M2C|INVALIDATE)`（对驱动侧失效做兜底）；
 *   ② 连采 N 帧、用**彩噪评分**挑最干净的一帧（评分只看"亮度接近但色相跳变"的相邻像素对，
 *      噪带会把它打满，而普通彩色画面不会）。
 */
static void cam_invalidate_psram(const void *addr, size_t len)
{
    if (addr == NULL || len == 0) {
        return;
    }
    const size_t line = 32;                                   /* ESP32-S3 DCache 行 = 32 B */
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(line - 1);
    size_t head = (uintptr_t)addr - start;
    size_t sync_len = (len + head + line - 1) & ~(size_t)(line - 1);
    esp_cache_msync((void *)start, sync_len,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

/* 彩噪评分：RGB565 下"相邻像素亮度接近、色相跳变"的对数（越大越像花屏） */
static uint32_t cam_noise_score(const camera_fb_t *fb)
{
    if (fb == NULL || fb->format != PIXFORMAT_RGB565 || fb->buf == NULL) {
        return 0;
    }
    const uint16_t *px = (const uint16_t *)fb->buf;
    const int w = (int)fb->width;
    const int h = (int)fb->height;
    uint32_t noisy = 0;
    for (int y = 0; y < h; y += 2) {
        const uint16_t *row = px + (size_t)y * (size_t)w;
        for (int x = 2; x < w; x += 2) {
            const uint16_t a = row[x - 2], b = row[x];
            const int ra = (a >> 11) & 0x1f, ga = (a >> 5) & 0x3f, ba = a & 0x1f;
            const int rb = (b >> 11) & 0x1f, gb = (b >> 5) & 0x3f, bb = b & 0x1f;
            const int lum = (ra + ga + ba) - (rb + gb + bb);
            const int chr = (ra > rb ? ra - rb : rb - ra) + (ga > gb ? ga - gb : gb - ga) +
                            (ba > bb ? ba - bb : bb - ba);
            if (lum > -12 && lum < 12 && chr > 24) {
                noisy++;
            }
        }
    }
    return noisy;
}

/* 真机默认抓 3 帧挑最干净的一帧；stream 路径连采 2 帧（见文件顶部宏定义） */
static esp_err_t cam_grab_jpeg_tries(uint8_t **out, size_t *out_len, int *width, int *height, int tries)
{
    if (out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_len = 0;
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tries < 1) {
        tries = 1;
    }

    uint8_t *best = NULL;
    size_t best_len = 0;
    uint32_t best_score = UINT32_MAX;
    int best_w = 0, best_h = 0;
    uint32_t scores[8];
    int n_scores = 0;

    for (int i = 0; i < tries; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            s_errors++;
            snprintf(s_last.last_err, sizeof(s_last.last_err), "fb timeout (NO-SOI?)");
            if (best == NULL) {
                return ESP_FAIL;
            }
            break;
        }
        cam_invalidate_psram(fb->buf, fb->len);           /* 兜底：DMA 直写 PSRAM 后的 cache 可见性 */

        const uint32_t score = cam_noise_score(fb);
        uint8_t *jpg = NULL;
        size_t len = 0;
        bool enc_ok;
        if (fb->format == PIXFORMAT_JPEG) {
            jpg = (uint8_t *)malloc(fb->len);
            if (jpg != NULL) {
                memcpy(jpg, fb->buf, fb->len);
                len = fb->len;
            }
            enc_ok = (jpg != NULL);
        } else {
            enc_ok = frame2jpg(fb, s_quality, &jpg, &len);
        }
        if (n_scores < (int)(sizeof(scores) / sizeof(scores[0]))) {
            scores[n_scores++] = score;
        }
        if (enc_ok && jpg != NULL && len > 0) {
            if (score < best_score) {                     /* 取最干净的一帧 */
                free(best);
                best = jpg;
                best_len = len;
                best_score = score;
                best_w = (int)fb->width;
                best_h = (int)fb->height;
            } else {
                free(jpg);
            }
        } else {
            free(jpg);
            s_errors++;
            snprintf(s_last.last_err, sizeof(s_last.last_err), "frame2jpg failed");
        }
        esp_camera_fb_return(fb);

        /* 第一帧已经足够干净就不再多采（省时间）；否则继续采到 tries 次 */
        if (best_score <= CAM_NOISE_GOOD) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    if (best == NULL) {
        return ESP_FAIL;
    }
    if (width != NULL)  { *width = best_w; }
    if (height != NULL) { *height = best_h; }
    ESP_LOGD(TAG, "抓帧评分：%u/%u/%u（样本 %d 帧）", (unsigned)scores[0],
             (unsigned)(n_scores > 1 ? scores[1] : 0), (unsigned)best_score, n_scores);
    *out = best;
    *out_len = best_len;
    s_last.last_noise = best_score;
    s_last.last_grabs = n_scores;
    return ESP_OK;
}

esp_err_t camera_api_grab_jpeg(uint8_t **out, size_t *out_len, int *width, int *height)
{
    return cam_grab_jpeg_tries(out, out_len, width, height, CAM_GRAB_TRIES_CAPTURE);
}

/* ---------------------------------------------------------------- MJPEG 预览流（独立 httpd 实例） */

#define CAM_STREAM_BOUNDARY "oneyeframe"

static esp_err_t h_cam_stream(httpd_req_t *req)
{
    if (!s_inited) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "camera 未就绪（SCCB 未探测到 sensor）");
    }
    if (s_stream_clients > 0) {
        /* 单客户端：多路预览会互相抢帧缓冲/内部 DMA，本机也扛不住 */
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "已有预览客户端（本面只支持 1 路）");
    }
    s_stream_clients++;
    esp_err_t rc = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" CAM_STREAM_BOUNDARY);
    if (rc == ESP_OK) { rc = httpd_resp_set_hdr(req, "Cache-Control", "no-store"); }
    if (rc == ESP_OK) { rc = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); }

    uint32_t sent = 0;
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while (rc == ESP_OK) {
        uint8_t *jpg = NULL;
        size_t len = 0;
        int w = 0, h = 0;
        /* 流路径也做「连采 2 帧挑最干净」：帧率约 7.8 → 4 帧/s，但画面上噪带明显更少（真机权衡） */
        if (cam_grab_jpeg_tries(&jpg, &len, &w, &h, CAM_GRAB_TRIES_STREAM) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));      /* 单帧失败不结束流（真机偶发 fb timeout） */
            continue;
        }        /* multipart 分段按 RFC 2046 写规范（真机踩过"画面花屏/错位"，故把边界字节写死并核对）：
         *   --boundary CRLF headers CRLF CRLF <jpeg> CRLF   （下一段的 --boundary 紧跟其后）
         * 即：**每段数据后补一个 CRLF**，段前**不再**加 CRLF（否则会多出空行 = 双 CRLF）。 */
        char head[128];
        int n = snprintf(head, sizeof(head),
                         "--" CAM_STREAM_BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
                         "Content-Length: %u\r\n\r\n", (unsigned)len);
        rc = httpd_resp_send_chunk(req, head, (size_t)n);
        if (rc == ESP_OK) {
            rc = httpd_resp_send_chunk(req, (const char *)jpg, len);
        }
        if (rc == ESP_OK) {
            rc = httpd_resp_send_chunk(req, "\r\n", 2);
        }
        free(jpg);
        if (rc != ESP_OK) {
            break;                               /* 客户端断开：正常收尾，不回 500 */
        }
        sent++;
        s_stream_frames++;
        vTaskDelay(pdMS_TO_TICKS(20));           /* 编码本身 ~110 ms ⇒ 实测约 7–8 帧/s */
    }
    s_stream_clients--;
    uint32_t el = (uint32_t)(esp_timer_get_time() / 1000) - t0;
    /* 结束原因要能上报：浏览器/网络中途不再收（send 失败）与客户端主动停止，现象完全不同，
     * 但页面上的"预览中"字样是启动时写死的 ⇒ 必须靠这个字段把真相带到面板（真机踩过"花屏不恢复"）。 */
    snprintf(s_stream_end_reason, sizeof(s_stream_end_reason), "%s (send=%s, %u 帧/%u ms)",
             (rc == ESP_OK) ? "client-gone" : "send-error", esp_err_to_name(rc),
             (unsigned)sent, (unsigned)el);
    s_stream_end_count++;
    ESP_LOGI(TAG, "预览流结束：%u 帧 / %u ms（send=%s）", (unsigned)sent, (unsigned)el, esp_err_to_name(rc));
    return ESP_OK;
}

esp_err_t camera_api_stream_start(uint16_t port)
{
    if (s_stream_httpd != NULL) {
        return ESP_OK;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (port == 0) {
        port = 81;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port;
    cfg.ctrl_port = (uint16_t)(port + 1000);     /* 第二个实例必须用不同的控制端口 */
    cfg.max_uri_handlers = 2;
    cfg.stack_size = 5120;                       /* 流任务：抓帧 + 编码 + 分块发送 */
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 20;                  /* 浏览器渲染繁忙时可能短暂不读 socket：别急着掐断流 */
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t rc = httpd_start(&s_stream_httpd, &cfg);
    if (rc != ESP_OK) {
        /* 常见原因：内部 RAM 不足（流任务栈）、端口被占。降级：面板回落到「单帧轮询预览」 */
        ESP_LOGW(TAG, "预览流服务启动失败：%s（端口 %u；面板将回落单帧轮询预览）",
                 esp_err_to_name(rc), (unsigned)port);
        s_stream_httpd = NULL;
        return rc;
    }
    static const httpd_uri_t uri_stream = {
        .uri = "/stream", .method = HTTP_GET, .handler = h_cam_stream,
    };
    rc = httpd_register_uri_handler(s_stream_httpd, &uri_stream);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "预览流路由注册失败：%s", esp_err_to_name(rc));
        httpd_stop(s_stream_httpd);
        s_stream_httpd = NULL;
        return rc;
    }
    s_stream_port = port;
    ESP_LOGI(TAG, "预览流就绪：http://<设备IP>:%u/stream（MJPEG，multipart/x-mixed-replace）",
             (unsigned)port);
    return ESP_OK;
}

void camera_api_stream_status(uint16_t *port, uint32_t *frames, int *clients)
{
    if (port != NULL)    { *port = (s_stream_httpd != NULL) ? s_stream_port : 0; }
    if (frames != NULL)  { *frames = s_stream_frames; }
    if (clients != NULL) { *clients = s_stream_clients; }
}
