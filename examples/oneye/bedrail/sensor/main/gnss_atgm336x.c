/*
 * gnss_atgm336x.c —— ATGM336H-5N（AT6558）UART/NMEA 接收与解析（仅 SUBDEV 角色）
 * =============================================================================
 * 硬件/引脚/协议口径见 gnss_atgm336x.h 的文件头（⛔ 不要在本文件重复写一遍，
 * 避免两处漂移；唯一事实源是 .h 与 E:\hardware\ATGM336H-5N模块资料）。
 *
 * ★ 本文件的判据（真机联调时先看这三条）：
 *   ① `[gnss] UART 已起` —— 驱动/引脚配置成功；
 *   ② `[gnss] 收到 NMEA` —— **线接对了**（收到任意合法 `$...*hh` 行）；
 *   ③ `[gnss] 定位有效` —— 真正拿到经纬度（室内/无天线会一直不出现，属正常）。
 *   ⚠️ ②③ 是两件事：室内常见"②有、③没有"；用户 2026-10-25 口径是"天线已接但只能在室内"，
 *      所以联调时以 ② + `fix=0/sats=N` 为"链路通"的证据，经纬度要等能见天空再验。
 * =============================================================================
 */

#include "sdkconfig.h"

#if CONFIG_ONEYE_GNSS_ENABLE

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"

#include "gnss_atgm336x.h"

static const char *TAG = "gnss";

/* ---------------------------------------------------------------- 常量 */

#define GNSS_UART        ((uart_port_t)CONFIG_ONEYE_GNSS_UART_NUM)
#define GNSS_TX_GPIO     (CONFIG_ONEYE_GNSS_TX_GPIO)
#define GNSS_RX_GPIO     (CONFIG_ONEYE_GNSS_RX_GPIO)
#define GNSS_PPS_GPIO    (CONFIG_ONEYE_GNSS_PPS_GPIO)
#define GNSS_BAUD        (CONFIG_ONEYE_GNSS_BAUD)

#define GNSS_UART_RX_BUF 2048
#define GNSS_LINE_MAX    128
#define GNSS_FIELD_MAX   20
#define GNSS_TASK_STACK  4096
#define GNSS_TASK_PRIO   3
#define GNSS_READ_TIMEOUT_MS 200

/* "UART 还在收数据"的判据窗口：5 s（模组 1 Hz 输出，5 s 仍无字节 = 线/供电/波特率有问题） */
#define GNSS_ALIVE_WINDOW_MS 5000
/* 定位有效期：10 s 内没有新的有效位置 ⇒ 对外报告"定位丢失"（模组 1 Hz） */
#define GNSS_FIX_STALE_MS    10000
/* 数据有效期：5 s 内没有合法 NMEA ⇒ gnssState=NO_DATA（模组 1 Hz） */
#define GNSS_DATA_STALE_MS   5000

/* ---------------------------------------------------------------- 状态 */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static gnss_fix_t   s_fix;                 /* 受 s_lock 保护 */
static bool         s_started;
static TaskHandle_t s_task;
static int64_t      s_last_byte_ms;        /* 最近收到任意字节的时刻 */
static uint8_t      s_pps_level = 1;       /* PPS 轮询用的上一电平 */

/* 解析中间态（只在解析任务里读写 ⇒ 不需要锁） */
static bool     s_rmc_valid;               /* RMC 状态位 = A */
static uint8_t  s_gga_quality;             /* GGA 第 6 字段（0=未定位） */
static uint8_t  s_gsa_fix;                 /* GSA 第 2 字段（1=未定位 2=2D 3=3D） */
static uint8_t  s_gga_sats;                /* GGA 参与定位卫星数 */
static uint8_t  s_gsv_sats;                /* GSV 可见卫星总数（按星座累加，见 parse_gsv） */

/* GSV 每条语句只带“本星座”的可见总数；$GPGSV / $BDGSV / $GLGSV 会各来一句。
 * ⛔ 不能用“最后一句覆盖”：北斗 0 会把 GPS 的可见数冲掉（真机 2026-10-28 已复现）。
 * 这里按 talker 各记一份，求和；超过 5 s 没更新的条目视为过期。 */
#define GNSS_GSV_TALKERS 6
typedef struct {
    char    talker[2];
    uint8_t total;
    int64_t ms;
} gsv_talker_t;
static gsv_talker_t s_gsv[GNSS_GSV_TALKERS];
static uint8_t      s_gsv_used;

/* ---------------------------------------------------------------- 小工具 */

static int64_t now_ms(void)
{
    return (int64_t)esp_log_timestamp();
}

/* 十六进制字符 → 值（非法返回 -1） */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* NMEA 校验：`$` 与 `*` 之间所有字节异或 = `*` 后两位十六进制。空 span 视为合法（无 `*` 的简化行） */
static bool nmea_checksum_ok(const char *line, size_t len)
{
    const char *star = NULL;
    uint8_t sum = 0;
    int hi;
    int lo;

    if (len < 4 || line[0] != '$') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (line[i] == '*') {
            star = &line[i];
            break;
        }
    }
    if (star == NULL || (size_t)(star - line) + 3u > len) {
        return false;   /* 本工程只收带校验和的完整行：⛔ 不猜 */
    }
    for (const char *p = line + 1; p < star; p++) {
        sum ^= (uint8_t)*p;
    }
    hi = hexval(star[1]);
    lo = hexval(star[2]);
    if (hi < 0 || lo < 0) {
        return false;
    }
    return sum == (uint8_t)((hi << 4) | lo);
}

/* `ddmm.mmmm` / `dddmm.mmmm` + 半球 → ×1e7 的整数度（非法/空字段返回 false） */
static bool dm_to_e7(const char *dm, char hemi, bool is_lat, int32_t *out)
{
    double v;
    int deg;
    double minute;
    double d;

    if (dm == NULL || dm[0] == '\0') {
        return false;
    }
    v = atof(dm);
    if (!(v > 0.0)) {
        return false;
    }
    deg = (int)(v / 100.0);
    minute = v - (double)deg * 100.0;
    d = (double)deg + minute / 60.0;
    if (hemi == 'S' || hemi == 'W') {
        d = -d;
    } else if (hemi != 'N' && hemi != 'E') {
        return false;
    }
    if (is_lat) {
        if (d < -90.0 || d > 90.0) {
            return false;
        }
    } else if (d < -180.0 || d > 180.0) {
        return false;
    }
    *out = (int32_t)llround(d * 1e7);
    return true;
}

/* 未定位时的经纬度空字段：清掉"上一次的位置"，但⛔ 不清 fix_ms（历史事实） */
static void clear_position_locked(void)
{
    s_fix.lat_e7 = 0;
    s_fix.lon_e7 = 0;
    s_fix.alt_m = 0;
    s_fix.speed_cms = 0;
    s_fix.course_cdeg = 0;
}

/* 由三个来源合成 fix/fix_type 并落 fix_ms */
static void recompute_fix_locked(void)
{
    uint8_t type = 0;

    if (s_gsa_fix == 2u) {
        type = 1;
    } else if (s_gsa_fix == 3u) {
        type = 2;
    } else if (s_rmc_valid || s_gga_quality > 0u) {
        type = 2;   /* GSA 还没来/不输出时，A 或质量>0 先按 3D 记 */
    }
    if (type == 0u) {
        s_fix.fix = false;
        s_fix.fix_type = 0;
        return;
    }
    s_fix.fix = true;
    s_fix.fix_type = type;
    s_fix.fix_ms = now_ms();
}

/* ------------------------------------------------------------ 语句解析 */

/* 按逗号切分（就地改字符串）；返回字段数，字段指针写进 f[] */
static size_t split_fields(char *s, char **f, size_t cap)
{
    size_t n = 0;
    char *p = s;

    if (cap == 0) {
        return 0;
    }
    f[n++] = p;
    for (; *p != '\0'; p++) {
        if (*p == ',') {
            *p = '\0';
            if (n >= cap) {
                return n;
            }
            f[n++] = p + 1;
        }
    }
    return n;
}

/* 取 `$GNRMC` / `GNRMC` 这类语句名的**最后 3 个字符**（RMC/GGA/GSA/GSV/VTG/TXT…），
 * 不区分 $GN/$GP/$BD/$GL 前缀 ⇒ 双模/北斗模式都能认。
 * ⚠️ 2026-10-28 真机修复：旧实现把**整行**传进来，取到的是行尾字段（如 `,,,`），
 *    导致 RMC/GGA/GSA/GSV 一个都没被分发 ⇒ fix/sats 永远停在默认 0。现在由 handle_line
 *    先 split_fields，再把 f[0]（语句名）传进来。 */
static bool sentence_kind(const char *name, char kind[4])
{
    size_t n;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    n = strlen(name);
    if (n < 3) {
        return false;
    }
    kind[0] = name[n - 3];
    kind[1] = name[n - 2];
    kind[2] = name[n - 1];
    kind[3] = '\0';
    return true;
}

static void parse_rmc(char **f, size_t n)
{
    char status;
    int32_t lat = 0;
    int32_t lon = 0;
    bool had_pos;

    if (n < 10) {
        return;
    }
    status = (f[2][0] != '\0') ? f[2][0] : 'V';
    had_pos = dm_to_e7(f[3], f[4][0], true, &lat) && dm_to_e7(f[5], f[6][0], false, &lon);

    portENTER_CRITICAL(&s_lock);
    s_rmc_valid = (status == 'A') && had_pos;
    if (had_pos) {
        s_fix.lat_e7 = lat;
        s_fix.lon_e7 = lon;
    } else {
        clear_position_locked();
    }
    if (f[7][0] != '\0') {
        double kn = atof(f[7]);
        if (kn >= 0.0 && kn < 655.0) {
            s_fix.speed_cms = (uint16_t)llround(kn * 51.4444);   /* 1 kn = 0.514444 m/s */
        }
    }
    if (f[8][0] != '\0') {
        double cdeg = atof(f[8]);
        if (cdeg >= 0.0 && cdeg <= 360.0) {
            s_fix.course_cdeg = (uint16_t)llround(cdeg * 100.0);
        }
    }
    if (f[1][0] != '\0') {
        s_fix.utc_hhmmss = (uint32_t)strtoul(f[1], NULL, 10);
    }
    if (f[9][0] != '\0') {
        s_fix.date_ddmmyy = (uint32_t)strtoul(f[9], NULL, 10);
    }
    s_fix.sats = (s_gga_sats > 0u) ? s_gga_sats : s_gsv_sats;
    recompute_fix_locked();
    portEXIT_CRITICAL(&s_lock);
}

static void parse_gga(char **f, size_t n)
{
    if (n < 10) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_gga_quality = (f[6][0] != '\0') ? (uint8_t)atoi(f[6]) : 0u;
    if (f[7][0] != '\0') {
        int sats = atoi(f[7]);
        s_gga_sats = (sats > 0 && sats <= 64) ? (uint8_t)sats : 0u;
    }
    if (f[9][0] != '\0') {
        double alt = atof(f[9]);
        if (alt > -1000.0 && alt < 10000.0) {
            s_fix.alt_m = (int16_t)llround(alt);
        }
    }
    s_fix.sats = (s_gga_sats > 0u) ? s_gga_sats : s_gsv_sats;
    recompute_fix_locked();
    portEXIT_CRITICAL(&s_lock);
}

static void parse_gsa(char **f, size_t n)
{
    if (n < 3 || f[2][0] == '\0') {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_gsa_fix = (uint8_t)atoi(f[2]);   /* 1=未定位 2=2D 3=3D */
    recompute_fix_locked();
    portEXIT_CRITICAL(&s_lock);
}

static void parse_gsv(char **f, size_t n)
{
    int total;
    int idx = -1;
    int64_t now;
    unsigned sum = 0;

    if (n < 4 || f[3][0] == '\0') {
        return;
    }
    if (strlen(f[0]) < 5) {
        return;
    }
    total = atoi(f[3]);
    if (total < 0 || total > 64) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    now = now_ms();

    /* 按 talker（GP/BD/GL/GA/GN…）找条目；找不到就追加。f[0] 是 "GPGSV"（无 `$`），
     * talker = f[0][0..1]。 */
    for (uint8_t i = 0; i < s_gsv_used; i++) {
        if (s_gsv[i].talker[0] == f[0][0] && s_gsv[i].talker[1] == f[0][1]) {
            idx = i;
            break;
        }
    }
    if (idx < 0 && s_gsv_used < GNSS_GSV_TALKERS) {
        idx = s_gsv_used++;
        s_gsv[idx].talker[0] = f[0][0];
        s_gsv[idx].talker[1] = f[0][1];
    }
    if (idx >= 0) {
        s_gsv[idx].total = (uint8_t)total;
        s_gsv[idx].ms = now;
    }

    for (uint8_t i = 0; i < s_gsv_used; i++) {
        if (s_gsv[i].ms != 0 && (now - s_gsv[i].ms) <= 5000) {
            sum += s_gsv[i].total;
        }
    }
    s_gsv_sats = (sum > 64u) ? 64u : (uint8_t)sum;

    /* 可见星数只影响"未定位时也有的读数"，⛔ 不改 fix 判定 */
    if (!s_fix.fix) {
        s_fix.sats = (s_gga_sats > 0u) ? s_gga_sats : s_gsv_sats;
    }
    portEXIT_CRITICAL(&s_lock);

#ifdef CONFIG_ONEYE_GNSS_LOG_RAW
    /* 只在"某星座真的看到星"时打一条，验证按星座累加后的 sats（2026-10-28 真机排障用）。 */
    if (total > 0) {
        ESP_LOGI(TAG, "[gnss] GSV %c%c total=%d ⇒ sats=%u（按星座累加）",
                 f[0][0], f[0][1], total, (unsigned)s_fix.sats);
    }
#endif
}

/* `$GPTXT,01,01,01,ANTENNA OK|OPEN|SHORT` —— 只认天线状态，其余 TXT 忽略 */
static void parse_txt(char **f, size_t n)
{
    uint8_t state = 0;

    for (size_t i = 0; i < n; i++) {
        if (strstr(f[i], "ANTENNA OPEN") != NULL) {
            state = 2;
            break;
        }
        if (strstr(f[i], "ANTENNA SHORT") != NULL) {
            state = 3;
            break;
        }
        if (strstr(f[i], "ANTENNA OK") != NULL) {
            state = 1;
            break;
        }
    }
    if (state == 0) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    bool changed = (s_fix.antenna != state);
    s_fix.antenna = state;
    portEXIT_CRITICAL(&s_lock);
    if (changed) {
        ESP_LOGI(TAG, "[gnss] 天线状态：%s",
                 state == 1 ? "OK" : (state == 2 ? "OPEN（未接/线断）" : "SHORT（短路）"));
    }
}

/* 一条完整 NMEA 行（已含 `$`/`*hh`，不含 CRLF） */
static void handle_line(char *line, size_t len)
{
    char *star;
    char kind[4];
    char *f[GNSS_FIELD_MAX];
    size_t nf;

    if (len < 7 || line[0] != '$') {
        return;
    }
    if (!nmea_checksum_ok(line, len)) {
        ESP_LOGD(TAG, "校验和不对，丢弃：%s", line);
        return;
    }
    star = strchr(line, '*');
    if (star != NULL) {
        *star = '\0';                 /* 只留 `$TYPE,f1,f2...` */
    }

    nf = split_fields(line + 1, f, GNSS_FIELD_MAX);   /* f[0] = 语句名 */
    if (nf < 2) {
        return;
    }
    if (!sentence_kind(f[0], kind)) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    s_fix.nmea_lines++;
    s_fix.valid = true;
    s_fix.updated_ms = now_ms();
    portEXIT_CRITICAL(&s_lock);

    if (strcmp(kind, "RMC") == 0) {
        parse_rmc(f, nf);
    } else if (strcmp(kind, "GGA") == 0) {
        parse_gga(f, nf);
    } else if (strcmp(kind, "GSA") == 0) {
        parse_gsa(f, nf);
    } else if (strcmp(kind, "GSV") == 0) {
        parse_gsv(f, nf);
    } else if (strcmp(kind, "TXT") == 0) {
        parse_txt(f, nf);
    }
    /* 其它语句（VTG/GLL/ZDA/TXT…）本模块不解：⛔ 不要因为"不认识"丢整条流 */
}

/* ---------------------------------------------------------------- 状态快照 */

bool gnss_get(gnss_fix_t *out)
{
    if (out == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_fix;
    portEXIT_CRITICAL(&s_lock);
#ifdef CONFIG_ONEYE_GNSS_TEST_FIX
    /* ⚠️ 仅验证用（Kconfig 默认 n；生产构建永远不得打开，见 ONEYE_GNSS_TEST_FIX 的 help）：
     *   室内拿不到真定位时，用一组写死坐标把「经纬度 → 帧 → 网关 TLV → 云端影子」跑通，
     *   避免这条分支直到户外才第一次执行。串口逐条打印「GNSS 测试注入」。 */
    {
        static bool test_inject_logged;

        out->valid = true;
        out->fix = true;
        out->fix_type = 2;                 /* 3D */
        out->sats = 9;
        out->lat_e7 = 399123456;           /* 39.9123456° */
        out->lon_e7 = 1164123456;          /* 116.4123456° */
        out->alt_m = 43;
        out->speed_cms = 50;               /* 0.50 m/s */
        out->course_cdeg = 9000;           /* 90.00° */
        out->fix_ms = now_ms();
        if (!test_inject_logged) {
            test_inject_logged = true;
            ESP_LOGW(TAG, "⚠️ GNSS 测试注入：本次上报固定 lat=39.9123456 lon=116.4123456 "
                          "alt=43 m speed=0.50 m/s sats=9 fix=3D（**仅验证链路**；"
                          "验证后必须回刷 CONFIG_ONEYE_GNSS_TEST_FIX=n 的默认构建）");
        }
    }
#endif
    /* 定位有效期：过期就在这里降级（不写回 s_fix：它记的是"模组说过什么"） */
    if (out->fix && out->fix_ms != 0 && (now_ms() - out->fix_ms) > GNSS_FIX_STALE_MS) {
        out->fix = false;
        out->fix_type = 0;
    }
    /* gnssState：先判"有没有数据"，再判天线/定位；与云端 TSL 0..4 逐值一致 */
    if (!out->valid || out->updated_ms == 0 ||
        (now_ms() - out->updated_ms) > GNSS_DATA_STALE_MS) {
        out->state = GNSS_STATE_NO_DATA;
        out->fix = false;
        out->fix_type = 0;
        out->sats = 0;
    } else if (out->antenna == 2 || out->antenna == 3) {
        out->state = GNSS_STATE_ANTENNA_FAULT;
    } else if (out->fix) {
        out->state = (out->fix_type == 1) ? GNSS_STATE_FIX_2D : GNSS_STATE_FIX_3D;
    } else {
        out->state = GNSS_STATE_NO_FIX;
    }
    return out->valid;
}

bool gnss_nmea_alive(void)
{
    int64_t t;

    portENTER_CRITICAL(&s_lock);
    t = s_last_byte_ms;
    portEXIT_CRITICAL(&s_lock);
    return (t != 0) && ((now_ms() - t) <= GNSS_ALIVE_WINDOW_MS);
}

/* ---------------------------------------------------------------- 任务 */

static void gnss_log_summary(const gnss_fix_t *g)
{
    if (g->fix) {
        ESP_LOGI(TAG, "[gnss] 定位有效 fix=%u sats=%u lat=%.7f lon=%.7f alt=%d m speed=%.2f m/s"
                      " utc=%06lu（NMEA %lu 行）",
                 (unsigned)g->fix_type, (unsigned)g->sats,
                 (double)g->lat_e7 / 1e7, (double)g->lon_e7 / 1e7,
                 (int)g->alt_m, (double)g->speed_cms / 100.0,
                 (unsigned long)g->utc_hhmmss, (unsigned long)g->nmea_lines);
    } else if (g->valid) {
        ESP_LOGI(TAG, "[gnss] 收到 NMEA、**尚未定位**（fix=0 sats=%u 可见/参与；室内属正常）"
                      "—— 需要天空视野，见 gnss_atgm336x.h 的 ① ② ③ 判据",
                 (unsigned)g->sats);
    }
}

static void gnss_poll_pps(void)
{
#if CONFIG_ONEYE_GNSS_PPS_GPIO >= 0
    int level = gpio_get_level((gpio_num_t)GNSS_PPS_GPIO);

    if (s_pps_level == 0 && level != 0) {
        portENTER_CRITICAL(&s_lock);
        s_fix.pps_count++;
        portEXIT_CRITICAL(&s_lock);
    }
    s_pps_level = (uint8_t)level;
#endif
}

static void gnss_task(void *arg)
{
    char line[GNSS_LINE_MAX];
    size_t used = 0;
    uint8_t buf[128];
    int64_t last_summary_ms = now_ms();
    bool alive_logged = false;
    bool first_fix_logged = false;

    (void)arg;
    for (;;) {
        int r = uart_read_bytes(GNSS_UART, buf, sizeof(buf), pdMS_TO_TICKS(GNSS_READ_TIMEOUT_MS));

        if (r > 0) {
            portENTER_CRITICAL(&s_lock);
            s_last_byte_ms = now_ms();
            portEXIT_CRITICAL(&s_lock);
            for (int i = 0; i < r; i++) {
                char c = (char)buf[i];
                if (c == '\r' || c == '\n') {
                    if (used > 0) {
                        line[used] = '\0';
#ifdef CONFIG_ONEYE_GNSS_LOG_RAW
                        ESP_LOGI(TAG, "NMEA: %s", line);
#endif
                        handle_line(line, used);
                        used = 0;
                    }
                } else if (c == '$') {
                    /* 新语句开始：丢掉半行（上一句没收完 = 行内异常） */
                    line[0] = '$';
                    used = 1;
                } else if (used + 1u < sizeof(line)) {
                    line[used++] = c;
                } else {
                    used = 0;   /* 超长行 ⇒ 丢弃重来 */
                }
            }
            if (!alive_logged && gnss_nmea_alive()) {
                alive_logged = true;
                ESP_LOGI(TAG, "[gnss] 收到 NMEA ⇒ **UART 接线/波特率正确**（9600 8N1）");
            }
        }

        gnss_poll_pps();

        /* 每 10 s 打一条状态摘要（含"室内一直未定位"的样子），便于串口自证 */
        {
            int64_t t = now_ms();
            if (last_summary_ms == 0 || (t - last_summary_ms) >= 10000) {
                gnss_fix_t g;
                bool has = gnss_get(&g);

                last_summary_ms = t;
                if (has) {
                    gnss_log_summary(&g);
                    if (g.fix && !first_fix_logged) {
                        first_fix_logged = true;
                        ESP_LOGI(TAG, "[gnss] ★ 首次定位成功（经纬度已可上报）");
                    }
                } else if (!alive_logged) {
                    ESP_LOGW(TAG, "[gnss] 10 s 未收到任何 NMEA（检查 5V 供电/共地/TXD↔GPIO21）");
                }
            }
        }

        /* 5 s 没字节 ⇒ 明确报警（比"安静地不定位"更可诊断） */
        if (alive_logged && !gnss_nmea_alive()) {
            static int64_t last_dead_log;
            int64_t t = now_ms();
            if (last_dead_log == 0 || (t - last_dead_log) >= 30000) {
                last_dead_log = t;
                ESP_LOGW(TAG, "[gnss] 5 s 内没有 NMEA 字节（模组掉电/线松/波特率不对？）");
            }
        }
    }
}

/* ---------------------------------------------------------------- 启动 */

esp_err_t gnss_start(void)
{
    esp_err_t err;
    uart_config_t cfg = {
        .baud_rate = GNSS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (s_started) {
        return ESP_OK;
    }

    err = uart_driver_install(GNSS_UART, GNSS_UART_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install 失败：%s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(GNSS_UART, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(GNSS_UART, GNSS_TX_GPIO, GNSS_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%u 参数/引脚配置失败：%s（TX=%d RX=%d baud=%d）",
                 (unsigned)CONFIG_ONEYE_GNSS_UART_NUM, esp_err_to_name(err),
                 (int)GNSS_TX_GPIO, (int)GNSS_RX_GPIO, (int)GNSS_BAUD);
        uart_driver_delete(GNSS_UART);
        return err;
    }

#if CONFIG_ONEYE_GNSS_PPS_GPIO >= 0
    {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << GNSS_PPS_GPIO),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        (void)gpio_config(&io);
        s_pps_level = (uint8_t)gpio_get_level((gpio_num_t)GNSS_PPS_GPIO);
    }
#endif

#ifdef CONFIG_ONEYE_GNSS_LOG_RAW
    /* 2026-10-28 自检（仅 raw 诊断构建）：不依赖天空，验证 GSV 按星座累加。
     * GPGSV 见 1 颗、BDGSV 见 0 颗 ⇒ 期望 sats=1（旧实现最后一句覆盖会得 0）。 */
    {
        char gp[] = "$GPGSV,1,1,01,28,,,21*71";
        char bd[] = "$BDGSV,1,1,00*68";

        handle_line(gp, strlen(gp));
        handle_line(bd, strlen(bd));
        ESP_LOGI(TAG, "[gnss] 自检 GSV：GP=1 / BD=0 ⇒ sats=%u（期望 1）", (unsigned)s_fix.sats);
    }
#endif

    if (xTaskCreate(gnss_task, "gnss", GNSS_TASK_STACK, NULL, GNSS_TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "GNSS 任务创建失败（内存不足）⇒ 本板不上报定位");
        uart_driver_delete(GNSS_UART);
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "==== ATGM336H-5N 接收已起：UART%u TX=GPIO%d RX=GPIO%d PPS=%d baud=%d 8N1 "
                  "（模组上电即输出 NMEA 1 Hz）====",
             (unsigned)CONFIG_ONEYE_GNSS_UART_NUM, (int)GNSS_TX_GPIO, (int)GNSS_RX_GPIO,
             (int)GNSS_PPS_GPIO, (int)GNSS_BAUD);
    return ESP_OK;
}

#endif /* CONFIG_ONEYE_GNSS_ENABLE */
