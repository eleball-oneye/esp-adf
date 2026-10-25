/*
 * gnss_atgm336x.h —— ATGM336H-5N GNSS 模组（中科微 AT6558）UART/NMEA 接收与解析
 * =============================================================================
 * 适用范围：**仅子设备（SUBDEV）角色**、且 `CONFIG_ONEYE_GNSS_ENABLE=y`。
 *
 * ★ 硬件口径（2026-10-25 实测资料：E:\hardware\ATGM336H-5N模块资料）：
 *   · 模组底板 5 针：VCC / GND / TXD / RXD / PPS；板载 RT9193 3.3 V LDO（可吃 3.3–5 V），
 *     IO 为 3.3 V TTL ⇒ 与 ESP32-C6 直连，**不需要电平转换**；
 *   · 串口默认 **9600 8N1**、无流控、**上电即输出 NMEA 1 Hz**（收数据不依赖 ESP 发任何命令）；
 *   · 模组 TXD 是**输出**（接 ESP 的 RX）、RXD 是**输入**（接 ESP 的 TX，仅改配置时用）。
 *
 * ★ 引脚口径（微雪 ESP32-C6-ZERO，方案 A，用户 2026-10-25 已按此接线）：
 *   · ESP TX = **GPIO20** → 模组 RXD
 *   · ESP RX = **GPIO21** ← 模组 TXD     ← 本模块的主数据线
 *   · PPS    = **GPIO22**（可选；本模块只计数，不参与定位解析）
 *   避开：strapping GPIO4/5/8/9/15、USB GPIO12/13、板上 WS2812=GPIO8、BOOT=GPIO9；
 *         UART0（GPIO16/17）仍是 IDF console 主通道 ⇒ 本模块固定用 **UART1**。
 *
 * ★ 解析范围（只取业务需要的字段，其余语句忽略）：
 *   · `$xxRMC` —— 定位有效性（A/V）、经纬度、地速、UTC 时间/日期；
 *   · `$xxGGA` —— 定位质量、参与定位卫星数、海拔；
 *   · `$xxGSA` —— 2D/3D 定位类型；
 *   · `$xxGSV` —— 可见卫星数（未定位时用它给"可见星数"一个读数）。
 *   ⚠️ 未定位时 RMC 状态=V、经纬度/速度为**空字段**，GGA 质量=0、卫星数=00、海拔为空
 *      —— 这是 NMEA 的合法形态，⛔ 不能当成"帧坏了"。
 *
 * ★ 线程模型：UART 接收 + 解析在**本模块自己的任务**里；`gnss_get()` 用临界区拷一份快照，
 *   ⛔ 调用方不会被 UART/解析阻塞。
 * =============================================================================
 */
#ifndef GNSS_ATGM336X_H
#define GNSS_ATGM336X_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一帧定位快照（单位在字段名里写死，⛔ 不要在别处再乘一次缩放） */
typedef struct {
    bool     valid;       /* 本快照是否曾收到过合法 NMEA（与"是否定位成功"无关） */
    bool     fix;         /* 定位是否有效（RMC=A 或 GGA 质量>0） */
    uint8_t  fix_type;    /* 0=未定位 / 1=2D / 2=3D（GSA 优先，GGA 质量兜底） */
    uint8_t  sats;        /* 参与定位卫星数（GGA）或可见卫星数（GSV）兜底 */
    int32_t  lat_e7;      /* 纬度 ×1e7（南纬为负） */
    int32_t  lon_e7;      /* 经度 ×1e7（西经为负） */
    int16_t  alt_m;       /* 海拔（米，GGA 第 9 字段） */
    uint16_t speed_cms;   /* 地速（0.01 m/s，RMC 节速换算） */
    uint16_t course_cdeg; /* 航向（0.01°，RMC 第 8 字段；未定位时为 0） */
    uint32_t utc_hhmmss;  /* UTC 时:分:秒（hhmmss，来自 RMC；未定位也可能有效） */
    uint32_t date_ddmmyy; /* UTC 日/月/年（ddmmyy，来自 RMC） */
    uint32_t pps_count;   /* PPS 上升沿累计（未接/未配置时恒 0） */
    int64_t  updated_ms;  /* 最近一条**合法** NMEA 的接收时刻（esp_log_timestamp 口径） */
    int64_t  fix_ms;      /* 最近一次"定位有效"的时刻（0 = 本次上电后从未定位） */
    uint32_t nmea_lines;  /* 累计解析的 NMEA 行数（自证"UART 在收数据"） */
} gnss_fix_t;

/*
 * 起 UART1 + 接收/解析任务（幂等：重复调用只返回当前状态）。
 * 返回 ESP_OK = 任务已起（不代表已收到数据）；其余 = 参数/驱动失败（已打印原因）。
 */
esp_err_t gnss_start(void);

/*
 * 拷一份最新快照（线程安全，不阻塞调用方）。
 * @return true = 结构体已填充（可能 fix=false）；false = 模块未启动或从未收到合法 NMEA。
 */
bool gnss_get(gnss_fix_t *out);

/* UART 是否在最近 5 s 内收到过字节（"线接对了"的判据；与是否定位无关）。 */
bool gnss_nmea_alive(void);

#ifdef __cplusplus
}
#endif

#endif /* GNSS_ATGM336X_H */
