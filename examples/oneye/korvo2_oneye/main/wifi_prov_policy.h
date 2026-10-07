/*
 * wifi_prov_policy.h —— 配网通道的**优先级判据**（ADR-0017 D2/D4）
 *
 * ============================================================================
 * ★ 本文件是**纯逻辑**：不 include 任何 ESP-IDF 头、不碰任何硬件，因此可以被
 *   **主机侧单测**（tools/test-prov-priority.c）与固件**编译同一份源**。
 *   这么做的理由：优先级判据是本决策里唯一"会判错"的地方，若单测另抄一份判据，
 *   它证明的是抄件而不是固件里跑的那份（本仓库的既有纪律：非空转测试）。
 *
 * ★ 优先级口径（用户 2026-10-07 产品级口径，逐字见 ADR-0017「决策记录」）：
 *
 *       SD 卡 Wi-Fi 凭据  >  蓝牙配网  >  其他（声波 / 二维码等预留）
 *        rank 0              rank 1      rank 2
 *
 *   ⚠️ 优先级**不是**「互斥开关」，而是**同一台设备上多通道都可得时谁先被采信**的规则：
 *      通道该开就开（如 BLE 机型照样读卡、有卡机型照样可开 BLE），
 *      但**同一时刻只有一份凭据被采信**（`held`），并据此发起入网。
 *   ⚠️ 判据**不看到达顺序**，只看 rank：BLE 先到、SD 后到 ⇒ SD 仍然胜出（抢占未入网的低优先级）。
 *      纯"先到先得"会让「手机先配了、卡是后来插上的」变成 BLE 胜出，与上述优先级直接冲突。
 *   ⚠️ 已入网（拿到 IP）之后，任何通道的提交一律被拒 —— **只入网一次**（上云链路只启动一次）。
 *      显式改配（台面 `wifi_set`）与 `prov.reset` 走「先释放采信槽再提交」的显式路径，
 *      不是本判据的例外，而是调用方明确表达的"重新配网"意图。
 *
 * ★ 预留通道（R1 声波 / R2 二维码）只有枚举，**没有**任何扫描/解码/协议实现，
 *   落在 rank 2（「其他」层）。⛔ 不要为它们写假实现或假分支。
 *
 * ---- 新增通道步骤（三步，违反任一步都会破坏 ADR-0017 D2 的唯一收敛点）----
 *   ① 在本枚举登记新成员，给它定 rank（`wifi_prov_channel_rank()`），
 *      并在 `wifi_prov_channel_source()`（wifi_prov.c）补上来源字符串；
 *   ② 通道模块**只调** `wifi_prov_submit_credentials(ssid, pass, 你的通道)` 把凭据交进来；
 *   ③ ⛔ **不得**调用 `oneye_start()`、⛔ **不得**新建任务/代码去跑「入网后逻辑」、
 *      ⛔ **不得**绕过本判据自己发起连接（连接层 `wifi_prov_connect()` 已收为
 *      `wifi_prov.c` 内部 static，本模块之外谁也拿不到）。
 */

#ifndef _WIFI_PROV_POLICY_H_
#define _WIFI_PROV_POLICY_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 通道标识 */

/**
 * 配网通道标识（**对象模型上的扩展点** —— 新增通道时在此登记，见文件头「新增通道步骤」）。
 *
 * ⚠️ 取值用于日志 / 来源字符串 / `/api/status` 展示 **以及优先级判据**（rank）；
 *    `SONIC` 与 `QRCODE` 是**预留**值，⛔ 没有任何生产代码路径会产生它们
 *    （不为声波/二维码写假实现 / 假扫描 / 假解码）。
 */
typedef enum {
    WIFI_PROV_CHAN_UNKNOWN = 0,   /* 未标注来源 */
    WIFI_PROV_CHAN_FILE    = 1,   /* C3 SD 卡 / SPIFFS 凭据文件（rank 0，最高） */
    WIFI_PROV_CHAN_BLE     = 2,   /* C1/C2 BLE（App 或另一台嵌入式设备）（rank 1） */
    WIFI_PROV_CHAN_API     = 3,   /* C4 台面本地 HTTP API（rank 2，「其他」） */
    WIFI_PROV_CHAN_KCONFIG = 4,   /* C5 Kconfig 兜底常量（**不是**交互式通道）（rank 2） */

    /* ---- 预留：仅登记扩展点，⛔ 未实现（rank 2，「其他」） ---- */
    WIFI_PROV_CHAN_SONIC   = 100, /* R1 声波 —— 预留，未实现 */
    WIFI_PROV_CHAN_QRCODE  = 101, /* R2 二维码 —— 预留，未实现 */
} wifi_prov_channel_t;

/* ------------------------------------------------------------------ 判据 */

/** 通道优先级：**数值小 = 优先级高**（SD 卡 0 > BLE 1 > 其他 2）。 */
static inline int wifi_prov_channel_rank(wifi_prov_channel_t ch)
{
    switch (ch) {
    case WIFI_PROV_CHAN_FILE: return 0;   /* SD 卡 WiFi 凭据 */
    case WIFI_PROV_CHAN_BLE:  return 1;   /* 蓝牙配网 */
    default:                  return 2;   /* 其他（台面 API / Kconfig 兜底 / 预留通道 / 未标注） */
    }
}

/** 一次「凭据提交」的裁决结果。 */
typedef enum {
    /** 采信槽为空 ⇒ 采信（首次有效凭据）。 */
    WIFI_PROV_DECISION_ACCEPT_FIRST      = 0,
    /** 同一通道再次提交 ⇒ 采信（改配/重试，如 App 重发同一通道的新密码）。 */
    WIFI_PROV_DECISION_ACCEPT_SAME       = 1,
    /** 更高优先级且尚未入网 ⇒ 采信（抢占：低优先级那份作废）。 */
    WIFI_PROV_DECISION_ACCEPT_PREEMPT    = 2,
    /** 优先级更低 ⇒ **拒绝**（不采信、不入网；必须留痕）。 */
    WIFI_PROV_DECISION_REJECT_LOWER      = 3,
    /** 已入网 ⇒ **拒绝**（只入网一次；必须留痕）。 */
    WIFI_PROV_DECISION_REJECT_ALREADY_UP = 4,
} wifi_prov_decision_t;

/**
 * ★ 唯一判据 ★ —— 决定"这一次提交的凭据要不要被采信"。
 *
 * @param held_valid 采信槽是否已有凭据（`wifi_prov.c` 的 `s_held_valid`）
 * @param held       采信槽里那份凭据来自哪条通道（`held_valid=false` 时忽略）
 * @param incoming   本次提交来自哪条通道
 * @param onboarded  **已用当前被采信的那份凭据完成入网**（调用方算作 `s_connected && s_held_valid`）
 *                   —— 这就是"只入网一次"的判据：槽一旦被**显式释放**（`wifi_prov_release_held()`，
 *                   只由"台面改配"与"`prov.reset`"这两个操作者显式动作触发），
 *                   `held_valid=false` ⇒ `onboarded=false` ⇒ 允许重新采信（换网），
 *                   ⛔ 但**上云链路的唯一性不受影响**：`oneye_start()` 仍只有收敛点下游一个调用点。
 *
 * 判据顺序有语义，⛔ 不要重排：
 *   ① 已入网且采信槽仍被占用 ⇒ 一律拒绝（**只入网一次**，与优先级无关：入网后连更高优先级也不抢占）；
 *   ② 槽为空（含被显式释放）⇒ 采信（首次有效）；
 *   ③ 同通道 ⇒ 采信（改配/重试）；
 *   ④ 严格更高优先级 ⇒ 抢占（低优先级那份作废）；
 *   ⑤ 其余 ⇒ 拒绝（留痕）。
 */
static inline wifi_prov_decision_t
wifi_prov_policy_decide(bool held_valid, wifi_prov_channel_t held,
                        wifi_prov_channel_t incoming, bool onboarded)
{
    if (onboarded) {
        return WIFI_PROV_DECISION_REJECT_ALREADY_UP;      /* ① 只入网一次 */
    }
    if (!held_valid) {
        return WIFI_PROV_DECISION_ACCEPT_FIRST;           /* ② 首次有效（或被显式释放后重新采信） */
    }
    if (incoming == held) {
        return WIFI_PROV_DECISION_ACCEPT_SAME;            /* ③ 同通道改配/重试 */
    }
    if (wifi_prov_channel_rank(incoming) < wifi_prov_channel_rank(held)) {
        return WIFI_PROV_DECISION_ACCEPT_PREEMPT;         /* ④ 高优先级抢占 */
    }
    return WIFI_PROV_DECISION_REJECT_LOWER;               /* ⑤ 低优先级拒绝 */
}

/** 该决策是否"采信"（会被交给连接层）。 */
static inline bool wifi_prov_decision_is_accept(wifi_prov_decision_t d)
{
    return d == WIFI_PROV_DECISION_ACCEPT_FIRST
        || d == WIFI_PROV_DECISION_ACCEPT_SAME
        || d == WIFI_PROV_DECISION_ACCEPT_PREEMPT;
}

/** 决策 → 稳定短名（日志/单测用；⛔ 不要改成中文以外有歧义的写法）。 */
static inline const char *wifi_prov_decision_str(wifi_prov_decision_t d)
{
    switch (d) {
    case WIFI_PROV_DECISION_ACCEPT_FIRST:      return "ACCEPT_FIRST";
    case WIFI_PROV_DECISION_ACCEPT_SAME:       return "ACCEPT_SAME";
    case WIFI_PROV_DECISION_ACCEPT_PREEMPT:    return "ACCEPT_PREEMPT";
    case WIFI_PROV_DECISION_REJECT_LOWER:      return "REJECT_LOWER";
    case WIFI_PROV_DECISION_REJECT_ALREADY_UP: return "REJECT_ALREADY_UP";
    default:                                   return "UNKNOWN";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* _WIFI_PROV_POLICY_H_ */
