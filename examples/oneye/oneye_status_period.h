/*
 * oneye_status_period.h —— `status/up` 周期 H（秒）与**机型**的唯一对照表
 * =============================================================================
 * 为什么需要它（用户 2026-10-21 裁定，逐字）：
 *   「转为由设备机型决定，比如 OY-SPK-01 型号设备的 H 为 60，让设备侧的 status/up
 *     满足对应 H 的实现」
 *
 * 云侧语义（2026-10-21 `73dcfb7c` 之后的**事实**，⛔ 不是本文件的假设）：
 *   · 在线会话键 TTL = **6 × H**（`TtlPolicy.HEARTBEAT_MULTIPLIER = 6`）；
 *   · **每一条已认证上行**都会滑动续期（`MessageRouter#route` ③′）；
 *   · H 由**机型配置**决定（OY-SPK-01 = 60、OY-SPK-02 = 300，另一批在 cloud 侧落地）。
 *   ⇒ 设备侧只需保证「**上行间隔 ≤ H**」；本文件把 H 按机型钉住，并由
 *     `oneye_dev_base_config_t.status_interval_s` 交给 SDK 的 base 心跳（见下）。
 *
 * 为什么**不新建**一个 app 侧定时任务：
 *   SDK base 已经拥有**唯一**的 `status/up` 发送路径 ——
 *     · 建链成功：`bint_on_link_up()` 先发一帧 `status/up{online:true}`（`oneye_internal.c:820`）；
 *     · 周期心跳：`bint_maybe_heartbeat()`（`oneye_internal.c:563-580`）按
 *       `status_interval_s` 在**网络线程**里驱动（与模块轮询解耦，`oneye_internal.c:598`）；
 *     · 断线重连：`bint_on_link_up()` 把 `last_status_ms` 重置为建链时刻并重发
 *       （`oneye_internal.c:801`、`:820-821`）。
 *   若 app 再起一个任务发同一面，就会**双发**同一条 retained 在线状态（语义重复、上行翻倍），
 *   因此本批只设置周期，不新增第二个发送方。
 *
 * ⛔ 不要往 `shadow/up` 的 state 里加 `heartbeatIntervalSec`：该 identifier 未进 TSL
 *    （`TslRegistry.validate` 对未登记键**抛异常** ⇒ 整帧被拒），本文件也不读影子值。
 *
 * ⚠️ **H 必须与 cloud 侧该机型的配置一致**：设备发得比 H 慢 ⇒ 在线键会过期（App 显示离线）；
 *    发得比 H 快 ⇒ 只多耗流量（不改变判据）。cloud 侧改机型 H 时，**必须**同步改本表
 *    （或改 Kconfig 覆盖值），否则两侧口径就分叉了。
 * =============================================================================
 */
#ifndef ONEYE_STATUS_PERIOD_H
#define ONEYE_STATUS_PERIOD_H

#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

/* 机型未登记时的兜底（= SDK 缺省 300 s；宁可不那么勤，也不要一个"猜"出来的更短周期）。 */
#define ONEYE_STATUS_PERIOD_FALLBACK_S 300u

/*
 * 机型 → H（秒）。**唯一**一份对照表；korvo2_oneye 与 devkitc_oneye 都包含本文件。
 *   OY-SPK-01（甲型；Korvo-2 / devkitc alpha）= 60
 *   OY-SPK-02（乙型；devkitc beta）            = 300
 *   OY-SPK-03（丙型；**不上云**）              = 300（不生效，仅为取值完整）
 */
static inline uint16_t oneye_status_period_for_model(const char *product_key)
{
    if (product_key == NULL) {
        return (uint16_t)ONEYE_STATUS_PERIOD_FALLBACK_S;
    }
    if (strcmp(product_key, "OY-SPK-01") == 0) {
        return 60u;
    }
    if (strcmp(product_key, "OY-SPK-02") == 0) {
        return 300u;
    }
    if (strcmp(product_key, "OY-SPK-03") == 0) {
        return 300u; /* 丙型不建云链路 ⇒ 该值不会被使用 */
    }
    return (uint16_t)ONEYE_STATUS_PERIOD_FALLBACK_S;
}

/*
 * 生效值：
 *   · `ONEYE_FW_STATUS_INTERVAL_S` > 0 ⇒ **显式覆盖**（用于与 cloud 临时对齐；
 *     改它就必须同步 cloud 的机型 H —— 见各工程 Kconfig 的 help）；
 *   · 0（缺省）⇒ 按 `ONEYE_FW_PRODUCT_KEY` 查上表（**推荐**：机型即 H）。
 *   ⛔ 0xFFFF 是 SDK 的"关闭心跳"哨兵，本处**不**暴露它（关了就别指望在线键）。
 */
#if defined(CONFIG_ONEYE_FW_STATUS_INTERVAL_S) && (CONFIG_ONEYE_FW_STATUS_INTERVAL_S > 0)
#define ONEYE_STATUS_INTERVAL_SEC() ((uint16_t)CONFIG_ONEYE_FW_STATUS_INTERVAL_S)
#else
#define ONEYE_STATUS_INTERVAL_SEC() oneye_status_period_for_model(CONFIG_ONEYE_FW_PRODUCT_KEY)
#endif

#endif /* ONEYE_STATUS_PERIOD_H */
