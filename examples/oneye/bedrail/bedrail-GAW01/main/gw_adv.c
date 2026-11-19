/*
 * bedrail-GAW01 —— 床护栏传感器 v5 TLV 广播接收 + 子设备影子**批量快照**上报
 *
 * 2026-11-19 方案 A（云端契约同步改造）：
 *   · 传感器侧 seq = **采样版本号**，每 2 s +1；
 *   · 网关只比较 1 B 的 seq（不再对整段 TLV 做 memcmp）；
 *   · 网关以 **2 s 周期把有变化的设备合并成一条** `sub.telemetry.batch` 上报；
 *   · 6 s 收不到新 seq ⇒ 判定 stale，上报一次 `online:false`；
 *   · 60 s 未上报过 ⇒ 心跳补一帧（影子不老化）。
 *
 * 出口 JSON（一条消息包含多台）：
 *   {"ts":1732070000,"n":2,"devs":[
 *      {"node":"12:23:34:45:56:88","seq":42,"rssi":-58,
 *       "tlv":"0101...","online":true,"hb":false}, ...]}
 */
#include "gw_adv.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_coexist.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oneye_link.h"
#include "oneye_link_adv_v5.h"

extern bool gw_child_emit_json(const char *type, const char *json);

#ifndef CONFIG_ONEYE_GAW01_TELE_MAX_DEV
#define CONFIG_ONEYE_GAW01_TELE_MAX_DEV            8
#endif

#define GW_TELE_TLV_MAX     64
#define GW_TELE_NODE_MAX    18
#define GW_BATCH_MS         2000    /* 批量出口周期 */
#define GW_STALE_MS         6000    /* 3 个采样周期没新 seq ⇒ stale */
#define GW_HB_MS            60000   /* 心跳：60 s 至少一帧 */
#define GW_REARM_MS         60000   /* 兜底重扫：60 s 一次（原 10 s，减少扫描空窗） */

typedef struct {
    char     node[GW_TELE_NODE_MAX];
    bool     used;
    bool     seen;
    bool     pending;           /* 本轮有变化，待批量出口 */
    bool     stale_reported;
    uint8_t  last_seq;
    uint8_t  last_tlv[GW_TELE_TLV_MAX];
    uint8_t  last_tlv_len;
    int8_t   last_rssi;
    int64_t  last_rx_ms;
    int64_t  last_report_ms;
    uint32_t frames;
} gw_tele_dev_t;

static gw_tele_dev_t s_devs[CONFIG_ONEYE_GAW01_TELE_MAX_DEV];

/* 2026-11-19[观测] 10 s 一行的窗口统计：不刷屏，但能看清到达率与 seq 连续性。 */
static uint32_t s_win_recv;      /* 窗口内收到的有效广播帧 */
static uint32_t s_win_dup;       /* 其中 seq 未变（重复帧） */
static uint32_t s_win_new_seq;   /* 其中 seq 变化（新版本） */
static uint32_t s_win_missed;    /* 按 seq 缺口估算的丢失包数 */
static uint32_t s_win_batch_msg; /* 窗口内发出的批量消息数 */
static uint32_t s_win_batch_dev; /* 窗口内批量消息包含的设备数 */
static int64_t  s_gap_last_ms;   /* 最近两次到达间隔 */
static int64_t  s_gap_max_ms;    /* 窗口内最大到达间隔 */
static int64_t  s_last_rx_ms;
static const char *TAG = "gw_adv";

static int64_t now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000);
}

static void hex_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char *kHex = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; i < len && (o + 2u) < cap; i++) {
        out[o++] = kHex[(in[i] >> 4) & 0x0Fu];
        out[o++] = kHex[in[i] & 0x0Fu];
    }
    out[o] = '\0';
}

static gw_tele_dev_t *dev_get(const char *node)
{
    gw_tele_dev_t *slot = NULL;
    for (size_t i = 0; i < (size_t)CONFIG_ONEYE_GAW01_TELE_MAX_DEV; i++) {
        gw_tele_dev_t *d = &s_devs[i];
        if (d->used && strcmp(d->node, node) == 0) {
            return d;
        }
        if (!d->used && slot == NULL) {
            slot = d;
        }
    }
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
        (void)snprintf(slot->node, sizeof(slot->node), "%s", node);
        slot->used = true;
    }
    return slot;
}

/* ---------------------------------------------------------------- 广播回调 */

static void on_adv_v5(const char *addr, const oneye_link_adv_v5_t *v5,
                      const uint8_t *mfg, uint16_t mfg_len, int8_t rssi, void *ctx)
{
    (void)ctx;
    if (addr == NULL || v5 == NULL || mfg == NULL || mfg_len <= 2u) {
        return;
    }

    gw_tele_dev_t *d = dev_get(addr);
    if (d == NULL) {
        ESP_LOGW(TAG, "[tele] 设备表已满（%d）⇒ 丢弃 mac=%s",
                 (int)CONFIG_ONEYE_GAW01_TELE_MAX_DEV, addr);
        return;
    }

    const uint8_t *tlv = &mfg[2];
    const size_t tlv_len = (size_t)(mfg_len - 2u);
    if (tlv_len == 0u || tlv_len > GW_TELE_TLV_MAX) {
        return;
    }

    {
        const int64_t now = now_ms();
        s_win_recv++;
        if (s_last_rx_ms > 0) {
            s_gap_last_ms = now - s_last_rx_ms;
            if (s_gap_last_ms > s_gap_max_ms) {
                s_gap_max_ms = s_gap_last_ms;
            }
        }
        s_last_rx_ms = now;
    }

    /* 判据：只比 1 B seq（seq 每采样 +1）。相同 ⇒ 重复帧，丢。 */
    if (d->seen) {
        const uint8_t delta = (uint8_t)(v5->seq - d->last_seq);   /* 模运算，兼容回绕 */
        if (delta == 0) {
            s_win_dup++;
            d->frames++;
            return;
        }
        s_win_new_seq++;
        if (delta > 1) {
            s_win_missed += (uint32_t)(delta - 1);   /* 中间漏掉的版本数 */
        }
    }

    if (!d->seen) {
        /* 2026-11-19[修复] 首帧就要建立心跳基线：last_report_ms 若留 0，
         * `t - 0 >= 60 s` 恒真 ⇒ 每 2 s 都被当成"心跳"重发（实测 5 条/10 s）。 */
        d->last_report_ms = now_ms();
    }
    d->seen = true;
    d->pending = true;
    d->stale_reported = false;
    d->last_seq = v5->seq;
    d->last_rssi = rssi;
    d->last_rx_ms = now_ms();
    d->last_tlv_len = (uint8_t)tlv_len;
    memcpy(d->last_tlv, tlv, tlv_len);
    d->frames++;
}

/* ----------------------------------------------------------- 2 s 批量出口 */

static void flush_batch(void)
{
    static char body[900];
    char dev[220];
    char hex[GW_TELE_TLV_MAX * 2 + 1];
    const int64_t t = now_ms();
    int n = 0;
    size_t off = 0;

    off += (size_t)snprintf(body + off, sizeof(body) - off,
                            "{\"ts\":%lld,\"n\":0,\"devs\":[", (long long)t);

    for (size_t i = 0; i < (size_t)CONFIG_ONEYE_GAW01_TELE_MAX_DEV; i++) {
        gw_tele_dev_t *d = &s_devs[i];
        if (!d->used || !d->seen) {
            continue;
        }

        const bool stale = (t - d->last_rx_ms) > GW_STALE_MS;
        /* stale 帧本身不是"新鲜上报"，⛔ 不能同时触发心跳，否则 stale 期间会每 2 s 重复发。 */
        const bool hb = !stale && (t - d->last_report_ms) >= GW_HB_MS;

        if (!d->pending && !(stale && !d->stale_reported) && !hb) {
            continue;
        }

        hex_encode(d->last_tlv, d->last_tlv_len, hex, sizeof(hex));
        (void)snprintf(dev, sizeof(dev),
                       "%s{\"node\":\"%s\",\"seq\":%u,\"rssi\":%d,\"online\":%s,"
                       "\"hb\":%s,\"tlv\":\"%s\"}",
                       (n == 0) ? "" : ",", d->node, (unsigned)d->last_seq, (int)d->last_rssi,
                       stale ? "false" : "true", hb ? "true" : "false", hex);

        if (off + strlen(dev) + 4u >= sizeof(body)) {
            break;
        }
        off += (size_t)snprintf(body + off, sizeof(body) - off, "%s", dev);
        n++;

        d->pending = false;
        if (stale) {
            d->stale_reported = true;
        }
        /* 无论 stale / hb / 变化，只要这一轮真的发出去了，就推进基线。 */
        d->last_report_ms = t;
    }

    if (n == 0) {
        return;
    }
    (void)snprintf(body + off, sizeof(body) - off, "]}");

    if (!gw_child_emit_json("sub.telemetry.batch", body)) {
        ESP_LOGW(TAG, "[batch] 入队失败（事件队列满？）n=%d", n);
        return;
    }
    s_win_batch_msg++;
    s_win_batch_dev += (uint32_t)n;
    ESP_LOGI(TAG, "[batch] 上报 %d 台（2 s 周期）", n);
}

static void batch_task(void *arg)
{
    (void)arg;
    uint32_t ticks = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(GW_BATCH_MS));
        flush_batch();

        if (++ticks >= 5u) {   /* 5 × 2 s = 10 s */
            ESP_LOGI(TAG,
                     "[stat] 10s recv=%u dup=%u newseq=%u missed=%u batch=%u devs=%u "
                     "gap_last=%lldms gap_max=%lldms",
                     (unsigned)s_win_recv, (unsigned)s_win_dup,
                     (unsigned)s_win_new_seq, (unsigned)s_win_missed,
                     (unsigned)s_win_batch_msg, (unsigned)s_win_batch_dev,
                     (long long)s_gap_last_ms, (long long)s_gap_max_ms);
            s_win_recv = 0;
            s_win_dup = 0;
            s_win_new_seq = 0;
            s_win_missed = 0;
            s_win_batch_msg = 0;
            s_win_batch_dev = 0;
            s_gap_max_ms = 0;
            ticks = 0;
        }
    }
}

/* 10 s 兜底重扫：保证动态占空比/共存偏好收敛 */
static void scan_rearm_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(GW_REARM_MS));
        oneye_link_scan_refresh();
    }
}

void gw_adv_start(void)
{
    if (oneye_link_register_adv_v5_cb(on_adv_v5, NULL) != ONEYE_LINK_OK) {
        ESP_LOGE(TAG, "注册 v5 广播回调失败 ⇒ 传感器广播不会被接收");
        return;
    }
    oneye_link_set_discovery(true);
    (void)esp_coex_preference_set(ESP_COEX_PREFER_BT);

    if (xTaskCreate(batch_task, "gw_adv_batch", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "批量出口任务创建失败 ⇒ 影子不会上云");
    }
    if (xTaskCreate(scan_rearm_task, "gw_adv_rearm", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "扫描兜底任务创建失败");
    }

    ESP_LOGI(TAG, "★ 广播接收已注册；出口=%d ms 批量（seq 判据），stale=%d ms，心跳=%d ms",
             GW_BATCH_MS, GW_STALE_MS, GW_HB_MS);
}
