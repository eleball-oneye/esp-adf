/*
 * board_lcd.c —— ESP32-C6-GEEK 板载 1.14" ST7789（240x135 横屏）简易 GUI
 *
 * 用户 2026-10-25 逐字要求：
 *   「其上屏幕需要实现简易的GUI，显示连接的温感子设备的温度数据以及当前wifi连接状态
 *     （一个wifi图标展示是否连接以及信号强度），UI背景图在 E:\Oneye\logo\oneye1.png」
 *
 * 设计要点（⛔ 不引 LVGL，理由：C6 无 PSRAM，稳态内部 RAM 只剩 ~44 KB，LVGL 的
 *   draw buffer + 对象树会把余量吃光；本模块常驻 RAM 只有一个 240×15 的条带缓冲）：
 *   ① 背景图**不进固件**：tools/mk-lcd-bg.py 把 PNG 转成
 *      `{"OYLCDv1",w=240,h=135,fmt=0}` 16 B 头 + RGB565(LE) 像素，烧到 `model` 分区
 *      （partitions.csv 里那块 1 MB spiffs 本来本项目就不用）⇒ 固件按条带流式读 flash。
 *   ② 前景只有两类：WiFi 扇形图标（是否连接 + 信号强度分级）+ 子设备温度大数字
 *      （附机型/电量小字）。
 *   ③ 面板方向/镜像由 Kconfig `ONEYE_T31_LCD_ROT` 选（0..3），换向要重烧。
 *
 * ⚠️ 本模块只渲染：不读 WiFi、不碰 link-sdk。状态由 main.c 的 UI 任务填 lcd_status_t 传入。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "board_lcd.h"
#include "lcd_font.h"
#include "lcd_font_big.h"     /* 温度用的 16×24 大字（比 8×16 放大 4 倍清楚得多） */

static const char *TAG = "board_lcd";

/* ---------------------------------------------------------------- 板级常量 */
#define LCD_W 135
#define LCD_H 240

/* ESP32-C6-GEEK 官方 demo（ESP-IDF 06_lvgl_example_v9/main/LCD_Driver/lcd_st7789.h）的实测引脚 */
#define LCD_PIN_SCLK GPIO_NUM_1
#define LCD_PIN_MOSI GPIO_NUM_2
#define LCD_PIN_RST  GPIO_NUM_4
#define LCD_PIN_DC   GPIO_NUM_3
#define LCD_PIN_CS   GPIO_NUM_5
#define LCD_PIN_BL   GPIO_NUM_6

#define LCD_SPI_HOST    SPI2_HOST
#define LCD_PIXEL_CLK   (40 * 1000 * 1000)
/* 条带高度：135×10×2 = 2.7 KB / 条（C6 无 PSRAM，云起后内部堆只剩几十 KB ⇒ 条带越小越安全）。
 * 2026-10-25 从 20 → 10 行：**画面一模一样**（每条带先合成再推，只是多发 12 次 SPI 事务，
 * 整帧仍 ≈26 ms），但省出 2.7 KB 常驻内部 RAM —— 属于"不动 UI 效果"的纯内存优化。 */
#define LCD_BAND_H      10
#define LCD_BL_ON_LEVEL 1

/* 面板可见玻璃（**原生竖屏**）：135(W)×240(H) + 偏移 X=52 / Y=40。
 * 🔴 2026-10-25 真机照片实锤：**不要再想横屏 240×135**。
 *   第一版按横屏画（swap_xy + gap(40,52)）在真机上表现为「转动 90° + 每行错切/竖条纹」——
 *   因为这块玻璃的 CASET 只有 135 列：喂 240 像素/行会**每行溢出 105 像素折到下一行**
 *   ⇒ 斜切 + 重影。官方 demo（06_lvgl_example_v9）的已知可用配置就是：
 *   hres=135 / vres=240 / **不 swap** / mirror(x,y) / gap(52,40) / invert=true。 */
#define LCD_GAP_X 52
#define LCD_GAP_Y 40

/* 背景分区（复用 partitions.csv 的 model 分区，1 MB spiffs） */
#define BG_PART_LABEL "model"
#define BG_MAGIC      "OYLCDv1"
#define BG_HDR_BYTES  16

/* 配色（RGB565）。CREAM 取自 logo 主色 (241,223,198) */
#define COL_CREAM 0xF6F8u
#define COL_WHITE 0xFFFFu
#define COL_BLACK 0x0000u
#define COL_RED   0xF800u
#define COL_GREY  0x7BEFu

/* ---------------------------------------------------------------- 状态 */
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;
static bool                      s_ready;
static volatile bool             s_rendering;

static const esp_partition_t *s_bg_part;
static bool                   s_bg_ok;

static uint16_t s_band[LCD_W * LCD_BAND_H];

/*
 * 🔴 2026-10-25 真机照片（条纹/位置错乱）根因之一：**条带缓冲在 DMA 还没发完时就被下一段覆盖**。
 *   我们只有一块 s_band，`draw_bitmap()` 是**异步排队**（trans_queue_depth=3，不会等发完就返回），
 *   于是下一次循环把下一段画进同一块内存 → 屏上看到的是"旧数据新数据混在一起"⇒ 横向条纹/错位。
 *   修法（最小、且不占第二块 5.4 KB 内存）：注册 `on_color_trans_done`，每推完一段**等完成信号**
 *   再画下一段（单缓冲串行）。整帧 12 段 × ~1 ms，1 Hz 刷新完全够。
 */
static SemaphoreHandle_t s_flush_done;

static bool lcd_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t hp = pdFALSE;
    if (s_flush_done != NULL) {
        (void)xSemaphoreGiveFromISR(s_flush_done, &hp);
    }
    return hp == pdTRUE;
}
static int      s_band_y0;
static int      s_band_y1;

/* ---------------------------------------------------------------- 小工具 */
static inline uint16_t darken(uint16_t c)
{
    /* 亮度减半（保留色相）：每通道右移一位 */
    return (uint16_t)((c >> 1) & 0x7BEFu);
}

static const uint8_t *glyph_rows(char ch)
{
    for (size_t i = 0; i < LCD_FONT_COUNT; i++) {
        if (k_lcd_glyph[i].ch == ch) {
            return k_lcd_glyph[i].rows;
        }
    }
    return NULL;
}

static const uint8_t *big_glyph_rows(char ch)
{
    for (size_t i = 0; i < LCD_FONT_BIG_COUNT; i++) {
        if (k_lcd_glyph_big[i].ch == ch) {
            return k_lcd_glyph_big[i].rows;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- 像素/矩形 */
static void fb_px(int x, int y, uint16_t c)
{
    if (s_band_y0 == s_band_y1 || x < 0 || x >= LCD_W || y < s_band_y0 || y >= s_band_y1) {
        return;
    }
    s_band[(y - s_band_y0) * LCD_W + x] = c;
}

static void fb_fill(int x, int y, int w, int h, uint16_t c)
{
    int x1 = x + w;
    int y1 = y + h;

    if (x < 0) {
        x = 0;
    }
    if (y < s_band_y0) {
        y = s_band_y0;
    }
    if (x1 > LCD_W) {
        x1 = LCD_W;
    }
    if (y1 > s_band_y1) {
        y1 = s_band_y1;
    }
    for (int yy = y; yy < y1; yy++) {
        uint16_t *row = &s_band[(yy - s_band_y0) * LCD_W];
        for (int xx = x; xx < x1; xx++) {
            row[xx] = c;
        }
    }
}

/* 半透明压暗（给前景提供对比度，同时保留 logo 可见） */
/* 注：2026-10-25 改成卡片排版后，整块压暗不再使用（卡片内部走 `fb_panel` 的两级 darken），
 *   原 `fb_darken()` 已删除以免 `-Wunused-function`。若以后要做"整块压暗"，
 *   请照 `fb_panel` 的写法对每个像素调 `darken()`，⛔ 不要写 `(v >> n) & mask`（会串色）。 */

/* ---------------------------------------------------------------- 文字 */
static int fb_text(int x, int y, const char *s, int scale, uint16_t c)
{
    if (s == NULL || scale < 1) {
        return x;
    }
    for (const char *p = s; *p != '\0'; p++) {
        const uint8_t *rows = glyph_rows(*p);
        if (rows == NULL) {
            x += (LCD_FONT_W + 1) * scale;
            continue;
        }
        for (int gy = 0; gy < LCD_FONT_H; gy++) {
            uint8_t bits = rows[gy];
            if (bits == 0) {
                continue;
            }
            for (int gx = 0; gx < LCD_FONT_W; gx++) {
                if ((bits & (0x80u >> gx)) == 0) {
                    continue;
                }
                if (scale == 1) {
                    fb_px(x + gx, y + gy, c);
                } else {
                    fb_fill(x + gx * scale, y + gy * scale, scale, scale, c);
                }
            }
        }
        x += (LCD_FONT_W + 1) * scale;
    }
    return x;
}

static int fb_text_width(const char *s, int scale)
{
    if (s == NULL) {
        return 0;
    }
    return (int)strlen(s) * (LCD_FONT_W + 1) * scale;
}

/*
 * 大字（16×24 位图）——只用于温度（0-9 . -）。
 * 为什么不让 8×16 直接放大 4 倍：最近邻放大会把斜笔画放成台阶，真机上"36"看着是毛边块；
 * 16×24 再放大 2 倍（⇒ 32×48 像素）笔画是连续的。
 */
static int fb_text_big(int x, int y, const char *s, int scale, uint16_t c)
{
    if (s == NULL || scale < 1) {
        return x;
    }
    const int row_bytes = LCD_FONT_BIG_ROW_BYTES;
    for (const char *p = s; *p != '\0'; p++) {
        const uint8_t *rows = big_glyph_rows(*p);
        if (rows == NULL) {
            x += (LCD_FONT_BIG_W + 1) * scale;
            continue;
        }
        for (int gy = 0; gy < LCD_FONT_BIG_H; gy++) {
            const uint8_t *line = &rows[gy * row_bytes];
            for (int gx = 0; gx < LCD_FONT_BIG_W; gx++) {
                if ((line[gx / 8] & (0x80u >> (gx % 8))) == 0) {
                    continue;
                }
                if (scale == 1) {
                    fb_px(x + gx, y + gy, c);
                } else {
                    fb_fill(x + gx * scale, y + gy * scale, scale, scale, c);
                }
            }
        }
        x += (LCD_FONT_BIG_W + 1) * scale;
    }
    return x;
}

static int fb_text_big_width(const char *s, int scale)
{
    if (s == NULL) {
        return 0;
    }
    return (int)strlen(s) * (LCD_FONT_BIG_W + 1) * scale;
}

/* ---------------------------------------------------------------- WiFi 图标 */
/*
 * 经典 WiFi 扇形：三层同心圆弧（开口向上）+ 底部圆点。
 * 连接状态用「点亮几层弧」表示信号强度：
 *   level 3 = 强（RSSI ≥ -55）/ 2 = 中（≥ -67）/ 1 = 弱（≥ -78）/ 0 = 很弱但已连
 * 未连接：三层弧全灰 + 一条红色斜杠。
 */
static void fb_wifi_icon(int cx, int base_y, bool connected, int level, uint16_t on_col)
{
    static const int k_r_in[3] = { 4, 9, 14 };
    const uint16_t off_col = COL_GREY;

    for (int ring = 0; ring < 3; ring++) {
        int r = k_r_in[ring];
        /* level 3 → 三层全亮；2 → 外两层；1 → 最外一层；0 → 最内一层（很弱） */
        bool lit = connected && (level >= (3 - ring));
        if (connected && level == 0) {
            lit = (ring == 2);
        }
        uint16_t col = lit ? on_col : (connected ? COL_GREY : off_col);
        int r2_out = (r + 1) * (r + 1);
        int r2_in = (r - 1) * (r - 1);

        for (int dy = -(r + 2); dy <= 0; dy++) {
            for (int dx = -(r + 2); dx <= (r + 2); dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 < r2_in || d2 > r2_out) {
                    continue;
                }
                /* 只保留上方 ±55° 的弧（|dx| ≤ 0.85r） */
                int adx = (dx < 0) ? -dx : dx;
                if (adx * 100 > r * 85) {
                    continue;
                }
                fb_px(cx + dx, base_y + dy, col);
            }
        }
    }
    /* 底部圆点 */
    fb_fill(cx - 1, base_y - 1, 3, 3, connected ? on_col : off_col);

    if (!connected) {
        /* 红色斜杠：左上 → 右下 */
        for (int i = 0; i < 26; i++) {
            int x = cx - 13 + i;
            int y = base_y - 14 + i / 2;
            fb_fill(x, y, 2, 2, COL_RED);
        }
    }
}

/* ---------------------------------------------------------------- 背景 */
static bool bg_probe(void)
{
    s_bg_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, BG_PART_LABEL);
    if (s_bg_part == NULL) {
        ESP_LOGW(TAG, "找不到分区 \"%s\" ⇒ 背景改为纯色", BG_PART_LABEL);
        return false;
    }
    uint8_t hdr[BG_HDR_BYTES] = { 0 };
    if (esp_partition_read(s_bg_part, 0, hdr, sizeof(hdr)) != ESP_OK) {
        ESP_LOGW(TAG, "背景分区读失败 ⇒ 纯色");
        return false;
    }
    uint16_t w = (uint16_t)(hdr[8] | (hdr[9] << 8));
    uint16_t h = (uint16_t)(hdr[10] | (hdr[11] << 8));
    if (memcmp(hdr, BG_MAGIC, 8) != 0 || w != LCD_W || h != LCD_H) {
        ESP_LOGW(TAG, "背景分区未烧或格式不符（magic/w=%u/h=%u）⇒ 纯色背景（烧录：tools/mk-lcd-bg.py + esptool 0x410000）",
                 (unsigned)w, (unsigned)h);
        return false;
    }
    ESP_LOGI(TAG, "背景图已装载：分区=%s offset=0x%lx %ux%u RGB565（%u B 流式读取）",
             s_bg_part->label, (unsigned long)s_bg_part->address, (unsigned)w, (unsigned)h,
             (unsigned)(LCD_W * LCD_H * 2));
    return true;
}

/* ---------------------------------------------------------------- 卡片 */
/*
 * 卡片：把底图压到 1/4 亮度 + 1 px 奶油描边。
 * 2026-10-25 用户要求：「背景图的奶油字移到最上方，下方分栏排卡片展示，温度值展示大字，
 * wifi 信息栏略小，保证所有文字的清晰可辨」⇒ 底图只留顶部 logo，其余区域由卡片承载内容。
 */
static void fb_panel(int x, int y, int w, int h)
{
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    /* ① 内部压暗（**亮度连续减半两次**：⛔ 不能写成 `(v >> 2) & mask` ——
     *   RGB565 是**打包**格式，整体右移 2 位会把 R 移进 G、G 移进 B，
     *   红色 0xF800 → 0x3E00 变成绿色/蓝色（2026-10-25 真机照片里"卡片是蓝的"就是这个）。
     *   正确做法 = 对每个通道各自右移：调用两次 darken()。 */
    for (int yy = y0 + 1; yy < y1 - 1; yy++) {
        if (yy < s_band_y0 || yy >= s_band_y1) {
            continue;
        }
        uint16_t *row = &s_band[(yy - s_band_y0) * LCD_W];
        for (int xx = x0 + 1; xx < x1 - 1; xx++) {
            if (xx < 0 || xx >= LCD_W) {
                continue;
            }
            row[xx] = darken(darken(row[xx]));
        }
    }
    /* ② 描边 */
    fb_fill(x0, y0, w, 1, COL_CREAM);
    fb_fill(x0, y1 - 1, w, 1, COL_CREAM);
    fb_fill(x0, y0, 1, h, COL_CREAM);
    fb_fill(x1 - 1, y0, 1, h, COL_CREAM);
}

/* ---------------------------------------------------------------- 渲染主函数 */
void board_lcd_render(const lcd_status_t *st)
{
    if (!s_ready || st == NULL) {
        return;
    }
    if (s_rendering) {
        return;     /* 单任务渲染；重入直接丢弃这一帧 */
    }
    s_rendering = true;

    const char *model = (st->sub_model != NULL && st->sub_model[0] != '\0') ? st->sub_model : "T31";

    /* 温度文本：**整数/小数分开**（整数 4 倍字、小数 2 倍字）——135 px 宽屏上「36.5」整串 4 倍
     *   会到 144 px 放不下；分开后总宽 108 px，既够大又不裁字。
     * ⚠️ 用 short / unsigned char + `%hd`/`%hhu`：`%d` 可打印宽度是 int 全域，
     *   gcc 的 -Werror=format-truncation 会直接判红（真机首编就踩到）。 */
    char t_whole[8] = "--";
    char t_frac[8] = ".-";
    if (st->has_temp) {
        int v = st->temp_centi;
        int whole = v / 100;
        int frac = v % 100;
        if (frac < 0) {
            frac = -frac;
        }
        if (whole > 999) {
            whole = 999;
        } else if (whole < -999) {
            whole = -999;
        }
        (void)snprintf(t_whole, sizeof(t_whole), "%hd", (short)whole);
        (void)snprintf(t_frac, sizeof(t_frac), ".%hhu", (unsigned char)((frac / 10) % 10));
    }

    for (int y0 = 0; y0 < LCD_H; y0 += LCD_BAND_H) {
        int y1 = y0 + LCD_BAND_H;
        if (y1 > LCD_H) {
            y1 = LCD_H;
        }
        s_band_y0 = y0;
        s_band_y1 = y1;

        /* ① 背景条带 */
        if (s_bg_ok && s_bg_part != NULL) {
            size_t off = BG_HDR_BYTES + (size_t)y0 * LCD_W * 2u;
            size_t len = (size_t)(y1 - y0) * LCD_W * 2u;
            if (esp_partition_read(s_bg_part, off, s_band, len) != ESP_OK) {
                fb_fill(0, y0, LCD_W, y1 - y0, COL_RED);
            }
        } else {
            for (int yy = y0; yy < y1; yy++) {
                uint16_t *row = &s_band[(yy - y0) * LCD_W];
                for (int xx = 0; xx < LCD_W; xx++) {
                    row[xx] = (uint16_t)(((xx * 32 / LCD_W) << 11) | ((yy * 32 / LCD_H) << 5) | 0x10);
                }
            }
        }

        /* ② 卡片式排版（用户 2026-10-25：「背景图的奶油字移到最上方，下方分栏排卡片展示，
         *    温度值展示大字，wifi 信息栏略小，保证所有文字的清晰可辨」）
         *
         * 版面（135×240 竖屏）：
         *   y   0..44   底图：顶部奶油 logo（不入卡片）
         *   y  46..146  卡片 A（大）：温度 —— 整数 4 倍字 + 小数 2 倍字
         *   y 152..238  卡片 B（左，略小）：WiFi 图标/信号/等级   |   卡片 C（右，略小）：机型/电量/云+子设备灯
         */
        {
            /* ---- 卡片 A：温度 ---- */
            fb_panel(4, 46, LCD_W - 8, 100);
            (void)fb_text(10, 50, "TEMP", 1, COL_CREAM);
            /* 提示行（2026-10-25 用户要求：「留出提示文本如 WAIT SUB 的显示空间」）——
             * 独立占一行（66..82），⛔ 不再和标题挤在同一行、也⛔ 不压在温度数字上 */
            if (!st->has_temp) {
                (void)fb_text(10, 66, "WAIT SUB", 1, COL_GREY);
            } else if (!st->sub_online) {
                (void)fb_text(10, 66, "SUB OFFLINE", 1, COL_GREY);
            }
            {
                /* 温度整体**下移**到 84（原 70），给提示行腾位置；℃ 跟着数字走（顶部对齐） */
                int wq = fb_text_big_width(t_whole, 2);
                int wf = fb_text_big_width(t_frac, 1);
                const int k_deg_w = 14;             /* 度符号 + C 的预留宽度 */
                int x0 = (LCD_W - (wq + wf + k_deg_w)) / 2;
                int y_t = 84;
                (void)fb_text_big(x0, y_t, t_whole, 2, COL_WHITE);          /* 84..132 */
                (void)fb_text_big(x0 + wq, y_t + 24, t_frac, 1, COL_WHITE); /* 108..132，底对齐 */
                {
                    int xd = x0 + wq + wf + 4;
                    fb_fill(xd, y_t + 4, 4, 4, COL_CREAM);                 /* 度符号：4×4 空心 */
                    fb_fill(xd + 1, y_t + 5, 2, 2, COL_BLACK);
                    (void)fb_text(xd + 5, y_t + 8, "C", 1, COL_CREAM);
                }
            }

            /* ---- 卡片 B（左，略小）：WiFi ---- */
            fb_panel(4, 152, 62, 86);
            (void)fb_text(8, 156, "WiFi", 1, COL_CREAM);
            {
                bool conn = st->wifi_connected;
                int  rssi = st->wifi_rssi;
                int  level = 0;
                char txt[16];
                const char *lv;

                if (conn) {
                    level = (rssi >= -55) ? 3 : (rssi >= -67) ? 2 : (rssi >= -78) ? 1 : 0;
                    (void)snprintf(txt, sizeof(txt), "%ddBm", rssi);
                } else {
                    (void)snprintf(txt, sizeof(txt), "OFF");
                }
                fb_wifi_icon(35, 196, conn, level, COL_CREAM);
                (void)fb_text(4 + (62 - fb_text_width(txt, 1)) / 2, 200, txt, 1,
                              conn ? COL_CREAM : COL_RED);
                lv = !conn ? "NO NET" : (level >= 3) ? "GOOD" : (level == 2) ? "FAIR"
                     : (level == 1) ? "WEAK" : "POOR";
                (void)fb_text(4 + (62 - fb_text_width(lv, 1)) / 2, 218, lv, 1,
                              conn ? COL_CREAM : COL_GREY);
            }

            /* ---- 卡片 C（右，略小）：机型 / 电量 / 云+子设备灯 ---- */
            fb_panel(70, 152, LCD_W - 74, 86);
            (void)fb_text(74, 156, model, 1, COL_CREAM);
            {
                char bat[12];
                if (st->battery >= 0) {
                    (void)snprintf(bat, sizeof(bat), "%d%%", st->battery);
                } else {
                    (void)snprintf(bat, sizeof(bat), "--%%");
                }
                (void)fb_text(74, 176, bat, 2, COL_WHITE);
            }
            fb_fill(74, 216, 8, 8, st->cloud_up ? COL_CREAM : COL_GREY);
            (void)fb_text(84, 212, "C", 1, COL_GREY);
            fb_fill(102, 216, 8, 8, st->sub_online ? COL_CREAM : COL_GREY);
            (void)fb_text(112, 212, "S", 1, COL_GREY);
        }

        /* ④ 备用字节序：面板不认 RAMCTRL 小端位时，软件把每条带逐像素交换成大端再推 */
#if CONFIG_ONEYE_T31_LCD_SWAP_BYTES
        {
            int n = (y1 - y0) * LCD_W;
            for (int i = 0; i < n; i++) {
                s_band[i] = (uint16_t)((s_band[i] >> 8) | (s_band[i] << 8));
            }
        }
#endif

        /* ⑤ 推屏：**必须等这一段发完**再画下一段（单缓冲复用的前提） */
        (void)esp_lcd_panel_draw_bitmap(s_panel, 0, y0, LCD_W, y1, s_band);
        if (s_flush_done != NULL) {
            (void)xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(1000));
        }
    }

    s_band_y0 = 0;
    s_band_y1 = 0;
    s_rendering = false;
}

/* ---------------------------------------------------------------- 初始化 */
bool board_lcd_ready(void)
{
    return s_ready;
}

esp_err_t board_lcd_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

#if !CONFIG_ONEYE_T31_LCD_ENABLE
    ESP_LOGI(TAG, "本构建未启用 LCD（CONFIG_ONEYE_T31_LCD_ENABLE=n）");
    return ESP_ERR_NOT_SUPPORTED;
#else
    /* ① 背光脚：先拉低（避免上电花屏），最后再点亮 */
    gpio_config_t bl_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl_cfg), TAG, "背光 GPIO 配置失败");
    (void)gpio_set_level(LCD_PIN_BL, !LCD_BL_ON_LEVEL);

    /* ② SPI 总线 */
    const spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_W * LCD_BAND_H * (int)sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI 总线初始化失败");

    /* ③ 面板 IO + ST7789 驱动 */
    s_flush_done = xSemaphoreCreateCounting(4, 0);
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLK,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        /* 队列深度只留 3：深度 10 在 C6 上要多占 ~1 KB 描述符，而本屏是"单任务顺序推送"，
         * 用不到深队列（⛔ 不要照抄 demo 的 10） */
        .trans_queue_depth = 3,
        /* 每段颜色发完回调一次 ⇒ 单缓冲可以安全复用（见 s_flush_done 的注释） */
        .on_color_trans_done = lcd_color_trans_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io),
                        TAG, "panel IO 创建失败");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        /* 🔴 2026-10-25 真机「花图」根因：**RGB565 字节序**配错。
         *   IDF 的 st7789 驱动缺省把 RAMCTRL 写成 **大端**（`ramctl_val_2 = 0xf0; // Use big endian by default`，
         *   见 esp_lcd_panel_st7789.c:112-117），而本工程的背景图/前景像素是**小端** RGB565
         *   （RGB565 工具产出的就是 LE）⇒ 每个像素的高低字节互换 ⇒ 红底变蓝紫、奶油字变暗绿，看起来就是「花图」。
         *   ⛔ 与"色域/色彩深度"无关（面板就是 16 位 RGB565 = 65K 色），⛔ 也不是 SPI 性能问题（40 MHz 远够）。
         *   修法一（缺省）：把面板 RAMCTRL 设成**小端**，像素数据不用动，零 CPU 开销。
         *   修法二（备用）：若这块 ST7789P3 不认 RAMCTRL 的端序位 ⇒ 打开
         *   `CONFIG_ONEYE_T31_LCD_SWAP_BYTES`（软件逐像素交换 + 面板保持大端）。 */
#if CONFIG_ONEYE_T31_LCD_SWAP_BYTES
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
#else
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
#endif
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel), TAG, "ST7789 驱动创建失败");

    (void)esp_lcd_panel_reset(s_panel);
    (void)esp_lcd_panel_init(s_panel);

    /* ④ 方向：**竖屏原生 135×240**（官方 demo 的已知可用口径：不 swap / mirror(x,y) / gap(52,40)）。
     * 🔴 第一版按横屏画（swap_xy + gap(40,52)）在真机上是「转 90° + 每行错切/竖条纹」：
     *   CASET 只有 135 列，喂 240 像素/行 ⇒ 每行溢出折到下一行。⛔ 不要回到横屏方案。
     * 四档只是镜像位组合（若字反了就换档重烧）： */
#if CONFIG_ONEYE_T31_LCD_ROT == 0
    (void)esp_lcd_panel_swap_xy(s_panel, false);
    (void)esp_lcd_panel_mirror(s_panel, true, true);
    (void)esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, LCD_GAP_Y);
#elif CONFIG_ONEYE_T31_LCD_ROT == 1
    (void)esp_lcd_panel_swap_xy(s_panel, false);
    (void)esp_lcd_panel_mirror(s_panel, true, false);
    (void)esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, LCD_GAP_Y);
#elif CONFIG_ONEYE_T31_LCD_ROT == 2
    (void)esp_lcd_panel_swap_xy(s_panel, false);
    (void)esp_lcd_panel_mirror(s_panel, false, true);
    (void)esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, LCD_GAP_Y);
#else
    (void)esp_lcd_panel_swap_xy(s_panel, false);
    (void)esp_lcd_panel_mirror(s_panel, false, false);
    (void)esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, LCD_GAP_Y);
#endif

    (void)esp_lcd_panel_invert_color(s_panel, true);     /* 这块 IPS 需要反色，否则红蓝反 */
    (void)esp_lcd_panel_disp_on_off(s_panel, true);

    /* ⑤ 背景分区 */
    s_bg_ok = bg_probe();

    /* ⑥ 点亮背光 */
    (void)gpio_set_level(LCD_PIN_BL, LCD_BL_ON_LEVEL);

    s_band_y0 = 0;
    s_band_y1 = 0;
    s_ready = true;
    ESP_LOGI(TAG, "LCD 就绪：ST7789 %dx%d，rotation=%d（CONFIG_ONEYE_T31_LCD_ROT），条带 %d 行，背景=%s",
             LCD_W, LCD_H, (int)CONFIG_ONEYE_T31_LCD_ROT, LCD_BAND_H, s_bg_ok ? "已装载" : "纯色回退");
    return ESP_OK;
#endif /* CONFIG_ONEYE_T31_LCD_ENABLE */
}
