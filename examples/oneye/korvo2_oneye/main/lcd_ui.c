/*
 * lcd_ui.c —— ILI9341（320×240）本地验证面：图案 + 文本状态屏
 *
 * 参照 ADF 例程（examples/display/lcd_jpeg、lcd_camera）的做法：
 *   board_handle = audio_board_lcd_init(periph_set, NULL);         // 板级：TCA9554 CS/RST/BL + SPI
 *   esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, rgb565_buf);  // 分带刷新
 * 本模块额外做两件事（便于"验证面板可驱动、可对账"）：
 *   ① 全屏 RGB565 帧缓冲放 **PSRAM**（320×240×2 = 150 KB；内部 RAM 要留给 Wi-Fi/AFE）；
 *   ② 内置 5×7 点阵字体（仅大写/数字/少量符号），可画可读的状态文本 —— 人眼/拍照可核对。
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "esp_lcd_panel_ops.h"

#include "lcd_ui.h"

static const char *TAG = "lcd_ui";

/* 分带刷新行数：一行 320 px × 2 B = 640 B ⇒ 40 行 = 25 KB/带，SPI 传输粒度合适 */
#define LCD_BAND_LINES 40

typedef uint16_t lcd_px_t;

static esp_lcd_panel_handle_t s_panel;
static lcd_px_t              *s_fb;
static bool                   s_ready;
static bool                   s_fb_psram;
static char                   s_pattern[16] = "";
static uint32_t               s_draws;
static uint64_t               s_last_ms;
static char                   s_l1[40], s_l2[40], s_l3[40];

/* ---------------------------------------------------------------- 颜色（RGB565） */
#define RGB565(r, g, b) ((lcd_px_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COL_BLACK   RGB565(0, 0, 0)
#define COL_WHITE   RGB565(255, 255, 255)
#define COL_RED     RGB565(220, 40, 40)
#define COL_GREEN   RGB565(40, 200, 80)
#define COL_BLUE    RGB565(60, 110, 240)
#define COL_YELLOW  RGB565(240, 210, 40)
#define COL_CYAN    RGB565(40, 200, 220)
#define COL_MAGENTA RGB565(220, 60, 200)
#define COL_GRAY    RGB565(90, 90, 90)
#define COL_DARK    RGB565(24, 28, 34)

/* ---------------------------------------------------------------- 5×7 点阵字体
 * 每个字形 7 行 × 5 列，行内低 5 位有效（bit4 = 最左）。仅覆盖本模块会显示的大写/数字/符号；
 * 未覆盖字符画成实心块（一眼可见"字体缺字"，不静默画错）。 */
typedef struct {
    char ch;
    uint8_t rows[7];
} lcd_glyph_t;

static const lcd_glyph_t k_font[] = {
    { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'D', { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
    { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
    { ':', { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
    { '/', { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 } },
    { '+', { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } },
    { '%', { 0x19, 0x1A, 0x02, 0x04, 0x08, 0x0B, 0x13 } },
    { '?', { 0x0E, 0x11, 0x01, 0x06, 0x04, 0x00, 0x04 } },
};

static const lcd_glyph_t *lcd_glyph(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    for (size_t i = 0; i < sizeof(k_font) / sizeof(k_font[0]); i++) {
        if (k_font[i].ch == c) {
            return &k_font[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- 帧缓冲原语 */
static inline void fb_px(int x, int y, lcd_px_t c)
{
    if (x >= 0 && x < LCD_UI_W && y >= 0 && y < LCD_UI_H) {
        s_fb[y * LCD_UI_W + x] = c;
    }
}

static void fb_fill(lcd_px_t c)
{
    for (size_t i = 0; i < (size_t)LCD_UI_W * LCD_UI_H; i++) {
        s_fb[i] = c;
    }
}

static void fb_rect(int x, int y, int w, int h, lcd_px_t c)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            fb_px(i, j, c);
        }
    }
}

static void fb_char(int x, int y, char ch, lcd_px_t fg, lcd_px_t bg, int scale)
{
    const lcd_glyph_t *g = lcd_glyph(ch);
    for (int r = 0; r < 7; r++) {
        uint8_t bits = (g != NULL) ? g->rows[r] : 0x1F;   /* 缺字：实心块，显式可见 */
        for (int c = 0; c < 5; c++) {
            lcd_px_t col = (bits & (1u << (4 - c))) ? fg : bg;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    fb_px(x + c * scale + sx, y + r * scale + sy, col);
                }
            }
        }
    }
}

static void fb_text(int x, int y, const char *txt, lcd_px_t fg, lcd_px_t bg, int scale)
{
    int cx = x;
    for (const char *p = txt; *p != '\0'; p++) {
        fb_char(cx, y, *p, fg, bg, scale);
        cx += 6 * scale;
        if (cx > LCD_UI_W - 6 * scale) {
            break;                                  /* 单行不换行，避免越界 */
        }
    }
}

/* 分带 flush：一次 40 行（25 KB），避免一次性 150 KB 的长传输 */
static esp_err_t lcd_flush(void)
{
    if (s_panel == NULL || s_fb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int y = 0; y < LCD_UI_H; y += LCD_BAND_LINES) {
        int lines = (y + LCD_BAND_LINES <= LCD_UI_H) ? LCD_BAND_LINES : (LCD_UI_H - y);
        esp_err_t rc = esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_UI_W, y + lines,
                                                 (const void *)(s_fb + (size_t)y * LCD_UI_W));
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "draw_bitmap 失败 y=%d rc=%s", y, esp_err_to_name(rc));
            return rc;
        }
    }
    s_draws++;
    s_last_ms = (uint64_t)(esp_timer_get_time() / 1000);
    return ESP_OK;
}

/* ---------------------------------------------------------------- 图案 */
static void draw_bars(void)
{
    const lcd_px_t cols[8] = { COL_WHITE, COL_YELLOW, COL_CYAN, COL_GREEN,
                               COL_MAGENTA, COL_RED, COL_BLUE, COL_BLACK };
    int w = LCD_UI_W / 8;
    for (int i = 0; i < 8; i++) {
        fb_rect(i * w, 0, (i == 7) ? (LCD_UI_W - 7 * w) : w, LCD_UI_H, cols[i]);
    }
}

static void draw_grid(void)
{
    fb_fill(COL_DARK);
    for (int x = 0; x < LCD_UI_W; x += 16) {
        fb_rect(x, 0, 1, LCD_UI_H, COL_GRAY);
    }
    for (int y = 0; y < LCD_UI_H; y += 16) {
        fb_rect(0, y, LCD_UI_W, 1, COL_GRAY);
    }
    fb_text(8, LCD_UI_H - 20, "GRID 16PX", COL_WHITE, COL_DARK, 2);
}

static void draw_checker(void)
{
    for (int y = 0; y < LCD_UI_H; y += 16) {
        for (int x = 0; x < LCD_UI_W; x += 16) {
            lcd_px_t c = (((x / 16) + (y / 16)) % 2) ? COL_WHITE : COL_BLACK;
            fb_rect(x, y, 16, 16, c);
        }
    }
    fb_text(8, 8, "CHECKER 16PX", COL_RED, COL_WHITE, 2);
}

static void draw_test(void)
{
    /* 对角线 + 边框 + 四角标记：用于核对分辨率/镜像/偏移（人眼或拍照即可判定） */
    fb_fill(COL_DARK);
    fb_rect(0, 0, LCD_UI_W, 2, COL_WHITE);
    fb_rect(0, LCD_UI_H - 2, LCD_UI_W, 2, COL_WHITE);
    fb_rect(0, 0, 2, LCD_UI_H, COL_WHITE);
    fb_rect(LCD_UI_W - 2, 0, 2, LCD_UI_H, COL_WHITE);
    for (int i = 0; i < LCD_UI_H; i++) {
        int x = i * LCD_UI_W / LCD_UI_H;
        fb_rect(x, i, 2, 1, COL_YELLOW);
        fb_rect(LCD_UI_W - 1 - x, i, 2, 1, COL_CYAN);
    }
    fb_rect(2, 2, 12, 12, COL_RED);                              /* 左上 */
    fb_rect(LCD_UI_W - 14, 2, 12, 12, COL_GREEN);                 /* 右上 */
    fb_rect(2, LCD_UI_H - 14, 12, 12, COL_BLUE);                  /* 左下 */
    fb_rect(LCD_UI_W - 14, LCD_UI_H - 14, 12, 12, COL_MAGENTA);   /* 右下 */
    fb_text(28, LCD_UI_H / 2 - 8, "320X240 TEST", COL_WHITE, COL_DARK, 3);
}

static void draw_status(void)
{
    fb_fill(COL_DARK);
    fb_rect(0, 0, LCD_UI_W, 26, COL_BLUE);
    fb_text(8, 6, "KORVO2 ONEYE", COL_WHITE, COL_BLUE, 3);
    fb_text(8, 40, s_l1[0] ? s_l1 : "LCD OK 320X240", COL_GREEN, COL_DARK, 3);
    fb_text(8, 76, s_l2[0] ? s_l2 : "CLOUD -", COL_YELLOW, COL_DARK, 3);
    fb_text(8, 112, s_l3[0] ? s_l3 : "KEY -", COL_CYAN, COL_DARK, 3);
    fb_rect(0, LCD_UI_H - 22, LCD_UI_W, 22, COL_GRAY);
    fb_text(8, LCD_UI_H - 19, "LOCAL VERIFY ONLY", COL_WHITE, COL_GRAY, 2);
}

static esp_err_t render_pattern(const char *pattern)
{
    if (strcmp(pattern, "bars") == 0) {
        draw_bars();
    } else if (strcmp(pattern, "grid") == 0) {
        draw_grid();
    } else if (strcmp(pattern, "checker") == 0) {
        draw_checker();
    } else if (strcmp(pattern, "test") == 0) {
        draw_test();
    } else if (strcmp(pattern, "status") == 0) {
        draw_status();
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_pattern, sizeof(s_pattern), "%s", pattern);
    return lcd_flush();
}

/* ---------------------------------------------------------------- 对外接口 */
esp_err_t lcd_ui_attach(void *panel_handle)
{
    if (panel_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready) {
        return ESP_OK;
    }
    s_panel = (esp_lcd_panel_handle_t)panel_handle;

    /* 帧缓冲优先 PSRAM（150 KB；内部 RAM 留给 Wi-Fi/AFE/网络栈） */
    size_t bytes = (size_t)LCD_UI_W * LCD_UI_H * sizeof(lcd_px_t);
    s_fb = (lcd_px_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    s_fb_psram = (s_fb != NULL);
    if (s_fb == NULL) {
        s_fb = (lcd_px_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "LCD 帧缓冲分配失败（%u B）：面板只做 init 不做绘制", (unsigned)bytes);
        s_panel = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG, "LCD 就绪：%dx%d RGB565，帧缓冲 %u B（%s），分带刷新 %d 行",
             LCD_UI_W, LCD_UI_H, (unsigned)bytes, s_fb_psram ? "PSRAM" : "内部 RAM", LCD_BAND_LINES);
    return ESP_OK;
}

bool lcd_ui_ready(void)
{
    return s_ready;
}

const char *lcd_ui_pattern(void)
{
    return s_pattern;
}

uint32_t lcd_ui_draws(void)
{
    return s_draws;
}

uint64_t lcd_ui_last_ms(void)
{
    return s_last_ms;
}

size_t lcd_ui_fb_bytes(void)
{
    return (size_t)LCD_UI_W * LCD_UI_H * sizeof(lcd_px_t);
}

bool lcd_ui_fb_in_psram(void)
{
    return s_fb_psram;
}

esp_err_t lcd_ui_set_pattern(const char *pattern)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pattern == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return render_pattern(pattern);
}

esp_err_t lcd_ui_show_status(const char *line1, const char *line2, const char *line3)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(s_l1, sizeof(s_l1), "%s", line1 != NULL ? line1 : "");
    snprintf(s_l2, sizeof(s_l2), "%s", line2 != NULL ? line2 : "");
    snprintf(s_l3, sizeof(s_l3), "%s", line3 != NULL ? line3 : "");
    return render_pattern("status");
}

void lcd_ui_get_state(lcd_ui_state_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->ready = s_ready;
    out->w = LCD_UI_W;
    out->h = LCD_UI_H;
    snprintf(out->pattern, sizeof(out->pattern), "%s", s_pattern);
    out->draws = s_draws;
    out->last_ms = s_last_ms;
    out->fb_bytes = lcd_ui_fb_bytes();
    out->fb_in_psram = s_fb_psram;
}
