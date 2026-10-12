/*
 * test-prov-priority.c —— 配网通道**优先级判据**的主机侧夹具测试（ADR-0017 D2/D4）
 *
 * ★ 关键性质：它 #include 的是**固件里跑的那一份判据**（`../main/wifi_prov_policy.h`），
 *   而不是另抄一份 —— 抄件只能证明抄件。门禁 `tools/check-prov-convergence.py` 的 A10
 *   会断言本文件确实 include 了该头、且**没有**自己定义 `wifi_prov_policy_decide` / rank。
 *
 * ★ 自带负向对照（变异测试，证明夹具不是空转）：
 *   同一组夹具会**再跑一遍**「变异判据」（优先级反向 + 忽略"只入网一次"），
 *   要求它**必须报红**。变异判据也全绿 ⇒ 夹具对本判据没有区分力 ⇒ 退出码 1。
 *   ★ 2026-10-12 追加**专项**负向对照（夹具⑤）：把「已入网一律拒绝」**放回**（不看通道）
 *   ⇒ 夹具⑤ 必须报红 —— 这是"App 发起配网 ⇒ 断旧连新"这条裁定的可判定性证明。
 *
 * 夹具（对应用户口径「优先级 = SD 卡 WiFi 凭据 > 蓝牙配网 > 其他」）：
 *   ① SD 凭据在 + BLE 也提交了 ⇒ **SD 胜出**（旧形态下无法判定：两条通道各自入网）
 *      ①a SD 先、BLE 后   ①b 到达顺序反过来（BLE 先、SD 后）   ①c 掉线不影响"谁被采信"
 *   ② SD 不在 + BLE 提交 ⇒ **BLE 胜出**；且「其他」层（台面 API / Kconfig / 预留通道）不越权
 *   ③ **只入网一次**：入网后**非权威通道**（SD/台面 API/预留）的提交一律被拒；
 *      唯一能重新配网的路径是**显式释放采信槽**（台面改配）或 prov.reset
 *   ⑤ **★ 2026-10-12 新增：权威通道（BLE/App）"断旧连新"**（用户裁定逐字：
 *      「已有app发起配网时设备要断现有wifi重新连接目标wifi，否则app会卡在配网中这个流程。」）：
 *      ⑤a 已在线（采信槽 = SD，已入网）+ 新 `prov.connect`（BLE）⇒ **采信** + **恰好一次**
 *          `esp_wifi_disconnect()`（且断链时抑制位已在位 ⇒ 不会"用旧配置重连"）+ 旧 SSID 不再被采信
 *          + 新凭据落槽（BLE）+ 重连成功；
 *      ⑤b **每次提交恰好一次**断开（第二次提交同样只断一次）；
 *      ⑤c **负向**：未入网且槽被 SD 占用 ⇒ BLE 提交**仍**按 rank 判（`REJECT_LOWER`）⇒ 0 次断链
 *          （⛔ C3 的优先级语义未放宽）；
 *      ⑤d **负向**：已入网 + **非权威通道**（台面 API）⇒ 仍是 `REJECT_ALREADY_UP` ⇒ 0 次断链
 *          （⛔ "只入网一次"只对 BLE 开了口子）；
 *      ⑤e ⑤a 之后：采信槽 = BLE（SD 那份**已作废**），台面 API 提交仍被拒。
 *   ④ **★ 2026-10-10 新增：`prov.status{state=connected}` 上报判据**（`wifi_prov_status.h`，
 *      契约 `contracts/local/link-frames.json` 的 `prov_states` 里 `connected` 是**既有**状态、
 *      且同文件 `prov.status` 的 `desc` 写明「connected 时 ip 必填」）：
 *      ④a 拿到 IP ⇒ **报**（含非空 ip）
 *      ④b/④c/④d **负向**：未入网 / ip 为空 / ip 为 NULL ⇒ **都不报**（⛔ 不拿空 ip 冒充在线）
 *      ④e **负向**：连续多次 GOT_IP ⇒ **只报一次**（不刷屏）
 *      ④f 发送失败 ⇒ 幂等位**不**置，下一次 GOT_IP 仍报（⛔ 不许把"发失败"写成"已上报"）
 *      ④g 掉线重连后再拿 IP（已报过）⇒ 不重复
 *   ⑥ **★ 2026-10-12〔本批〕`prov.status{connected}` 的会话边界复位**（判据本体 = `wifi_prov_status.h` 的 ④）：
 *      ⑥a 第一轮配网 ⇒ 报一次；**同一会话**内第二次 GOT_IP ⇒ 不再报（② 的"不刷屏"仍在）
 *      ⑥b Wi-Fi 断开（会话结束）⇒ 幂等位**复位**
 *      ⑥c 重连后再拿 IP ⇒ **再次报**一次（这正是真机第二轮"App 卡在配网中"要修的那条路径）
 *      ⑥d 断开-重连 ×3 ⇒ 各自恰好 1 次
 *      ⑥e **负向**：复位只动**状态** —— 复位后仍未拿到 IP ⇒ 仍**不报**（① ip 非空判据一字未改）
 *      ★ 专项负向对照（变异⑥ = **本批要修掉的旧行为**）：断开**不**复位 ⇒ ⑥b/⑥c 必须报红。
 *  ⚠️ 「失败走既有 failed」属 SDK link 层的既有行为（`tests/test_prov_provider.c` /
 *     `test_prov_worker.c` 断言 `prov.status{failed, err=auth_failed}`），本夹具**不**重造它
 *     —— 那是 SDK 宿主测试的职责，⛔ 不在这里抄一份。
 *
 * 构建/运行（WSL 或任何有 cc 的环境）：
 *   cc -std=c99 -Wall -Wextra -Werror -I ../main test-prov-priority.c -o /tmp/tpp && /tmp/tpp
 * 退出码：0 = 全部夹具通过且负向对照报红；1 = 有夹具失败，或负向对照未报红。
 */

#include <stdio.h>

#include "wifi_prov_policy.h"   /* ← 固件里跑的同一份判据（纯逻辑头，无 ESP 依赖） */
#include "wifi_prov_status.h"   /* ← 同上：prov.status{connected} 上报判据（同一份源） */

/* 若有人把上面那两行换成"本地抄件"，这两个 include guard 就不存在了 ⇒ 编译期直接失败。 */
#ifndef _WIFI_PROV_POLICY_H_
#error "必须 include 固件的 wifi_prov_policy.h（判据唯一事实源），不得另抄一份"
#endif
#ifndef _WIFI_PROV_STATUS_H_
#error "必须 include 固件的 wifi_prov_status.h（上报判据唯一事实源），不得另抄一份"
#endif

typedef wifi_prov_decision_t (*decide_fn)(bool, wifi_prov_channel_t, wifi_prov_channel_t, bool);
typedef int (*rank_fn)(wifi_prov_channel_t);

typedef struct {
    const char *name;
    decide_fn   decide;
    rank_fn     rank;
} policy_t;

static int g_fail;
static int g_checks;

static void expect(const char *what, wifi_prov_decision_t got, wifi_prov_decision_t want)
{
    g_checks++;
    if (got != want) {
        g_fail++;
        printf("  \xe2\x9c\x97 %s：期望 %s，实测 %s\n", what,
               wifi_prov_decision_str(want), wifi_prov_decision_str(got));
    } else {
        printf("  \xe2\x9c\x93 %s ⇒ %s\n", what, wifi_prov_decision_str(got));
    }
}

static void expect_held(const char *what, wifi_prov_channel_t got, wifi_prov_channel_t want)
{
    g_checks++;
    if (got != want) {
        g_fail++;
        printf("  \xe2\x9c\x97 %s：期望被采信的是 %d，实测 %d\n", what, (int)want, (int)got);
    } else {
        printf("  \xe2\x9c\x93 %s（channel=%d）\n", what, (int)got);
    }
}

static void expect_rank(const char *what, int got, int want)
{
    g_checks++;
    if (got != want) {
        g_fail++;
        printf("  \xe2\x9c\x97 %s：期望 rank=%d，实测 rank=%d\n", what, want, got);
    } else {
        printf("  \xe2\x9c\x93 %s（rank=%d）\n", what, got);
    }
}

static void expect_bool(const char *what, bool got, bool want)
{
    g_checks++;
    if (got != want) {
        g_fail++;
        printf("  \xe2\x9c\x97 %s：期望 %s，实测 %s\n", what,
               want ? "true" : "false", got ? "true" : "false");
    } else {
        printf("  \xe2\x9c\x93 %s ⇒ %s\n", what, got ? "true" : "false");
    }
}

/* 夹具⑤用：计数桩的读数断言（"恰好一次"必须可数，不是注释声称） */
static void expect_int(const char *what, int got, int want)
{
    g_checks++;
    if (got != want) {
        g_fail++;
        printf("  \xe2\x9c\x97 %s：期望 %d，实测 %d\n", what, want, got);
    } else {
        printf("  \xe2\x9c\x93 %s ⇒ %d\n", what, got);
    }
}

/* ------------------------------------------------------------------ 采信槽模型
 * 与 wifi_prov.c 的 submit 路径**同形**（不是复制业务逻辑，只是把三行状态推进写出来）：
 *   decide(held_valid, held, incoming, connected && held_valid) → 采信则写槽 / 入网则 connected=1。
 * ⚠️ 注意 `onboarded` 的算法与固件一致：**显式释放**槽位即视为"可重新配网"
 *    （`wifi_prov_release_held()` / `wifi_prov_forget()` 之后 held_valid=false）。 */
typedef struct {
    bool                held_valid;
    wifi_prov_channel_t held;
    bool                connected;
} slot_t;

static wifi_prov_decision_t submit(const policy_t *p, slot_t *s, wifi_prov_channel_t ch,
                                   bool connects_ok)
{
    wifi_prov_decision_t d = p->decide(s->held_valid, s->held, ch, s->connected && s->held_valid);
    if (wifi_prov_decision_is_accept(d)) {
        s->held_valid = true;
        s->held = ch;
        if (connects_ok) {
            s->connected = true;
        }
    }
    return d;
}

/* 显式释放采信槽（台面改配用；**不断开**连接）—— 对应 wifi_prov_release_held() */
static void release_held(slot_t *s)
{
    s->held_valid = false;
    s->held = WIFI_PROV_CHAN_UNKNOWN;
}

/* prov.reset —— 断连 + 清槽，对应 wifi_prov_forget() */
static void prov_reset(slot_t *s)
{
    release_held(s);
    s->connected = false;
}

/* ------------------------------------------------------------------ 夹具 */

static void fixture_1(const policy_t *p)
{
    printf("[夹具①] SD 凭据在 + BLE 也提交了 ⇒ **SD 胜出**（旧形态下无法判定：两条通道各自入网）\n");

    /* ①a：SD 先被采信（本次没能入网，如 AP 暂时不可达）⇒ BLE 提交必须被拒 */
    slot_t a = { false, WIFI_PROV_CHAN_UNKNOWN, false };
    expect("①a SD 卡凭据先提交 ⇒ 采信", submit(p, &a, WIFI_PROV_CHAN_FILE, false),
           WIFI_PROV_DECISION_ACCEPT_FIRST);
    expect("①a 随后 BLE 提交 ⇒ 拒绝（优先级低于已采信的 SD）",
           submit(p, &a, WIFI_PROV_CHAN_BLE, false), WIFI_PROV_DECISION_REJECT_LOWER);
    expect_held("①a 被采信的仍是 SD", a.held, WIFI_PROV_CHAN_FILE);

    /* ①b：到达顺序反过来 —— 判据看 rank，不看先后 */
    slot_t b = { false, WIFI_PROV_CHAN_UNKNOWN, false };
    expect("①b BLE 先提交 ⇒ 采信（当时还没有 SD 凭据）",
           submit(p, &b, WIFI_PROV_CHAN_BLE, false), WIFI_PROV_DECISION_ACCEPT_FIRST);
    expect("①b 随后 SD 提交 ⇒ 抢占（SD 仍然胜出）",
           submit(p, &b, WIFI_PROV_CHAN_FILE, false), WIFI_PROV_DECISION_ACCEPT_PREEMPT);
    expect_held("①b 被采信的变成 SD", b.held, WIFI_PROV_CHAN_FILE);

    /* ①c：掉线（connected=0）不影响"谁被采信" */
    slot_t c = { false, WIFI_PROV_CHAN_UNKNOWN, false };
    expect("①c SD 提交并入网", submit(p, &c, WIFI_PROV_CHAN_FILE, true),
           WIFI_PROV_DECISION_ACCEPT_FIRST);
    c.connected = false;   /* 掉线 */
    expect("①c 掉线后 BLE 提交 ⇒ 仍被拒（SD 仍持有采信槽）",
           submit(p, &c, WIFI_PROV_CHAN_BLE, false), WIFI_PROV_DECISION_REJECT_LOWER);
    expect("①c 掉线后 SD 再次提交 ⇒ 采信（同通道重试）",
           submit(p, &c, WIFI_PROV_CHAN_FILE, true), WIFI_PROV_DECISION_ACCEPT_SAME);
}

static void fixture_2(const policy_t *p)
{
    printf("[夹具②] SD 不在 + BLE 提交 ⇒ **BLE 胜出**；「其他」层不越权\n");

    slot_t a = { false, WIFI_PROV_CHAN_UNKNOWN, false };
    expect("②a 空槽 + BLE 提交（无 SD 凭据）⇒ 采信",
           submit(p, &a, WIFI_PROV_CHAN_BLE, false), WIFI_PROV_DECISION_ACCEPT_FIRST);
    expect_held("②a 被采信的是 BLE", a.held, WIFI_PROV_CHAN_BLE);
    expect("②b BLE 之后「其他」层（台面 API）提交 ⇒ 拒绝",
           submit(p, &a, WIFI_PROV_CHAN_API, false), WIFI_PROV_DECISION_REJECT_LOWER);
    expect("②c BLE 之后「其他」层（Kconfig 兜底）提交 ⇒ 拒绝",
           submit(p, &a, WIFI_PROV_CHAN_KCONFIG, false), WIFI_PROV_DECISION_REJECT_LOWER);
    expect("②d BLE 之后预留通道（声波）提交 ⇒ 拒绝（预留同属「其他」层）",
           submit(p, &a, WIFI_PROV_CHAN_SONIC, false), WIFI_PROV_DECISION_REJECT_LOWER);
    expect("②e BLE 之后预留通道（二维码）提交 ⇒ 拒绝",
           submit(p, &a, WIFI_PROV_CHAN_QRCODE, false), WIFI_PROV_DECISION_REJECT_LOWER);

    /* rank 表本身（口径的可判定形态：SD 0 > BLE 1 > 其他 2） */
    expect_rank("②f rank(SD 卡凭据)", p->rank(WIFI_PROV_CHAN_FILE), 0);
    expect_rank("②f rank(蓝牙配网)", p->rank(WIFI_PROV_CHAN_BLE), 1);
    expect_rank("②f rank(台面 API，其他)", p->rank(WIFI_PROV_CHAN_API), 2);
    expect_rank("②f rank(Kconfig 兜底，其他)", p->rank(WIFI_PROV_CHAN_KCONFIG), 2);
    expect_rank("②f rank(声波，预留/其他)", p->rank(WIFI_PROV_CHAN_SONIC), 2);
    expect_rank("②f rank(二维码，预留/其他)", p->rank(WIFI_PROV_CHAN_QRCODE), 2);
}

static void fixture_3(const policy_t *p)
{
    printf("[夹具③] 重复入网被拒（**只入网一次**；权威通道 BLE/App 除外，见夹具⑤）\n");

    slot_t a = { false, WIFI_PROV_CHAN_UNKNOWN, false };
    expect("③a 空槽 + SD 提交并入网", submit(p, &a, WIFI_PROV_CHAN_FILE, true),
           WIFI_PROV_DECISION_ACCEPT_FIRST);
    expect("③b 已入网 + SD 再提交（同通道重试）⇒ 拒绝",
           submit(p, &a, WIFI_PROV_CHAN_FILE, false), WIFI_PROV_DECISION_REJECT_ALREADY_UP);
    /* 🔴 2026-10-12 就地订正（用户裁定「App 发起配网 = 断旧连新」）：
     *  旧字逐字留痕（⛔ 不抹）：「③c 已入网 + BLE 提交 ⇒ 拒绝」（`REJECT_ALREADY_UP`）。
     *  新判据：BLE（权威通道）**不再**因已入网被拒 ⇒ 采信（抢占 SD 那份），由调用方断旧连新。 */
    expect("③c 已入网 + BLE 提交 ⇒ **采信**（权威通道接管；旧字逐字留痕见源码注释）",
           submit(p, &a, WIFI_PROV_CHAN_BLE, false), WIFI_PROV_DECISION_ACCEPT_PREEMPT);
    expect_held("③c 被采信的变成 BLE（SD 那份作废）", a.held, WIFI_PROV_CHAN_BLE);
    expect("③d 已入网 + 台面 API 提交 ⇒ 拒绝（非权威通道，语义未放宽）",
           submit(p, &a, WIFI_PROV_CHAN_API, false), WIFI_PROV_DECISION_REJECT_ALREADY_UP);
    expect("③e 已入网 + 预留通道（二维码）提交 ⇒ 拒绝",
           submit(p, &a, WIFI_PROV_CHAN_QRCODE, false), WIFI_PROV_DECISION_REJECT_ALREADY_UP);

    /* ③f 已入网后**非权威通道**连更高优先级也不能抢占（SD 是 rank 0，已是最高） */
    slot_t b = { false, WIFI_PROV_CHAN_UNKNOWN, false };
    expect("③f 空槽 + BLE 提交并入网", submit(p, &b, WIFI_PROV_CHAN_BLE, true),
           WIFI_PROV_DECISION_ACCEPT_FIRST);
    expect("③f 已入网 + 更高优先级的 SD 提交 ⇒ 仍拒绝（入网后不抢占）",
           submit(p, &b, WIFI_PROV_CHAN_FILE, false), WIFI_PROV_DECISION_REJECT_ALREADY_UP);

    /* ③g 显式释放仍是**非权威通道**换网的唯一路径（台面改配口径）；
     *    ⚠️ 对 BLE 而言，「无需释放也能重配」由夹具⑤判定（断旧连新）。 */
    expect("③g 未释放时非权威通道不放行（对照：证明「释放」是必要条件）",
           submit(p, &b, WIFI_PROV_CHAN_API, false), WIFI_PROV_DECISION_REJECT_ALREADY_UP);
    release_held(&b);
    expect("③g 显式释放采信槽后（仍在线上）+ SD 提交 ⇒ 采信",
           submit(p, &b, WIFI_PROV_CHAN_FILE, true), WIFI_PROV_DECISION_ACCEPT_FIRST);

    /* ③h prov.reset（断连 + 清槽）之后可以重新配网，且仍然只入网一次 */
    slot_t c = { false, WIFI_PROV_CHAN_UNKNOWN, false };
    expect("③h prov.reset 之后 + BLE 提交 ⇒ 采信",
           submit(p, &c, WIFI_PROV_CHAN_BLE, true), WIFI_PROV_DECISION_ACCEPT_FIRST);
    expect("③h 重新入网后 + 台面 API 提交 ⇒ 拒绝（只入网一次对非权威通道一致）",
           submit(p, &c, WIFI_PROV_CHAN_API, false), WIFI_PROV_DECISION_REJECT_ALREADY_UP);
    prov_reset(&c);   /* 与 forget() 同形，供下一条断言使用 */
    expect("③h prov.reset 之后再提交（槽空、已断连）⇒ 采信",
           submit(p, &c, WIFI_PROV_CHAN_BLE, true), WIFI_PROV_DECISION_ACCEPT_FIRST);
}

/* --------------------------------------------------- 夹具⑤：权威通道（BLE/App）**断旧连新**
 *
 * 🔴 2026-10-12（用户裁定，最高依据）逐字：
 *    「**已有app发起配网时设备要断现有wifi重新连接目标wifi**，否则app会卡在配网中这个流程。」
 *
 * 被断言的形状（与 `wifi_prov.c` 的 `wifi_prov_submit_credentials_src()` **同形**：
 * 判据 → [权威通道：在线才断 **恰好一次** → 置抑制位 → 丢旧采信态] → 落新槽 → connect）：
 *   · **恰好一次** `esp_wifi_disconnect()`（宿主侧用计数桩，不是"注释声称"）；
 *   · 断链**之前**抑制位必须在位（否则 `WIFI_EVENT_STA_DISCONNECTED` 分支会用**旧配置**
 *     自动重连 ⇒ "换网"退化成"重连旧网"；宿主侧同样以计数桩判定）；
 *   · 旧 SSID **不再被采信**（旧采信态作废、新通道落槽）；
 *   · ⛔ 非权威通道（SD / 台面 API / 预留）**不经过**这一段：0 次断链、语义逐字不变。
 */
typedef struct {
    slot_t slot;            /* 采信槽 + 在线位（与 fixture ①–③ 同一份语义） */
    int    disconnects;     /* 桩：`esp_wifi_disconnect()` 调用次数 */
    int    old_ssid_connects; /* 桩：断链时抑制位**不在位** ⇒ "用旧配置重连"发生次数 */
    bool   suppress;        /* 对应 `wifi_prov.c` 的 `s_reprovision` */
} switch_slot_t;

/* 与 `wifi_prov.c:submit` 的权威前置段同形（**只**是这三行状态推进，不是复制业务判据） */
static wifi_prov_decision_t submit_switch(const policy_t *p, switch_slot_t *s,
                                          wifi_prov_channel_t ch, bool connects_ok)
{
    wifi_prov_decision_t d = p->decide(s->slot.held_valid, s->slot.held, ch,
                                       s->slot.connected && s->slot.held_valid);
    if (!wifi_prov_decision_is_accept(d)) {
        return d;                       /* 拒绝 ⇒ 不采信、**不断链**（旧行为逐字） */
    }
    if (wifi_prov_channel_is_authoritative(ch)) {          /* ⇐ BLE/App = 权威通道 */
        if (s->slot.connected) {
            s->suppress = true;                            /* ⇐ wifi_prov.c 先置 s_reprovision */
            s->disconnects++;                              /* ⇐ esp_wifi_disconnect() 恰好一次 */
            if (!s->suppress) {
                s->old_ssid_connects++;                    /* 抑制失效 ⇒ 旧配置重连 */
            }
        }
        s->slot.connected = false;
        s->slot.held_valid = false;                        /* ⇐ 旧 SSID 不再被采信 */
        s->slot.held = WIFI_PROV_CHAN_UNKNOWN;
    }
    s->slot.held_valid = true;
    s->slot.held = ch;
    if (connects_ok) {
        s->slot.connected = true;
    }
    s->suppress = false;   /* 对应 `wifi_prov_connect()` 在 set_config 之后清零 */
    return d;
}

static void fixture_5(const policy_t *p)
{
    printf("[夹具⑤] 权威通道（BLE/App）**断旧连新**：恰好一次断开 + 旧 SSID 不再被采信\n");

    /* ⑤a 已在线（采信槽 = SD，已入网）+ 新 prov.connect（BLE）⇒ 采信 + 断一次 + 换槽 */
    switch_slot_t a = { { false, WIFI_PROV_CHAN_UNKNOWN, false }, 0, 0, false };
    expect("⑤a 空槽 + SD 提交并入网", submit_switch(p, &a, WIFI_PROV_CHAN_FILE, true),
           WIFI_PROV_DECISION_ACCEPT_FIRST);
    expect("⑤a 已在线 + BLE 提交 ⇒ **采信**（不再 REJECT_ALREADY_UP）",
           submit_switch(p, &a, WIFI_PROV_CHAN_BLE, true), WIFI_PROV_DECISION_ACCEPT_PREEMPT);
    expect_int("⑤a esp_wifi_disconnect() 调用次数（已在线，恰好一次）", a.disconnects, 1);
    expect_int("⑤a 断链前抑制位必须在位 ⇒ 用旧配置重连次数", a.old_ssid_connects, 0);
    expect_held("⑤a 新凭据落槽（BLE）；SD 那份作废", a.slot.held, WIFI_PROV_CHAN_BLE);
    expect_bool("⑤a 重连成功后在线上", a.slot.connected, true);

    /* ⑤b 每次提交**各自**恰好断一次（第二次同样只断一次，不是 0 次也不是 2 次） */
    expect("⑤b 再次 BLE 提交（同通道改配）⇒ 采信",
           submit_switch(p, &a, WIFI_PROV_CHAN_BLE, true), WIFI_PROV_DECISION_ACCEPT_SAME);
    expect_int("⑤b 累计断开次数（每次提交恰好一次 ⇒ 2）", a.disconnects, 2);

    /* ⑤e ⑤a 之后：SD 那份已作废 ⇒ 非权威通道的提交仍被"只入网一次"挡住 */
    expect("⑤e 换网之后 + 台面 API 提交 ⇒ 拒绝（非权威通道语义未变）",
           submit_switch(p, &a, WIFI_PROV_CHAN_API, true), WIFI_PROV_DECISION_REJECT_ALREADY_UP);
    expect_int("⑤e 被拒 ⇒ 不断链（累计仍是 2）", a.disconnects, 2);

    /* ⑤c **负向**：未入网且槽被 SD 占用 ⇒ BLE 提交**仍**按 rank 判 ⇒ 拒绝 + 0 次断链
     *    （⛔ C3 凭据文件的优先级语义没有被本次裁定放宽） */
    switch_slot_t c = { { false, WIFI_PROV_CHAN_UNKNOWN, false }, 0, 0, false };
    expect("⑤c 空槽 + SD 提交（本次未连上）", submit_switch(p, &c, WIFI_PROV_CHAN_FILE, false),
           WIFI_PROV_DECISION_ACCEPT_FIRST);
    expect("⑤c 未入网 + BLE 提交 ⇒ 仍是 REJECT_LOWER（⛔ C3 优先级不放宽）",
           submit_switch(p, &c, WIFI_PROV_CHAN_BLE, false), WIFI_PROV_DECISION_REJECT_LOWER);
    expect_int("⑤c 被拒 ⇒ 0 次断链", c.disconnects, 0);
    expect_held("⑤c 被采信的仍是 SD", c.slot.held, WIFI_PROV_CHAN_FILE);

    /* ⑤d **负向**：已入网 + **非权威通道**（台面 API）⇒ 仍是 REJECT_ALREADY_UP + 0 次断链 */
    switch_slot_t d = { { false, WIFI_PROV_CHAN_UNKNOWN, false }, 0, 0, false };
    expect("⑤d 空槽 + SD 提交并入网", submit_switch(p, &d, WIFI_PROV_CHAN_FILE, true),
           WIFI_PROV_DECISION_ACCEPT_FIRST);
    expect("⑤d 已入网 + 台面 API 提交 ⇒ 仍是 REJECT_ALREADY_UP（只对 BLE 开口子）",
           submit_switch(p, &d, WIFI_PROV_CHAN_API, true), WIFI_PROV_DECISION_REJECT_ALREADY_UP);
    expect_int("⑤d 被拒 ⇒ 0 次断链", d.disconnects, 0);
}

/* --------------------------------------------------- 夹具④：prov.status{connected} 上报判据
 *
 * 被断言的形状（`wifi_prov_status.h`；契约依据见该头）：
 *   · **非空 ip** —— App 的 `isOnline` 判据是 `(state=="connected") && ip 非空`；
 *     ⛔ 空串 / NULL 一律不许报 connected（否则把"没联网"报成"在线"）；
 *   · **只报一次** —— `on_wifi_ready()` 每次 GOT_IP 都会调 ⇒ 幂等位保证不刷屏；
 *   · **失败不置位** —— "已报过"只在发送成功后成立 ⇒ 失败时下一次 GOT_IP 仍会补报。
 */

typedef struct {
    const char *name;
    bool (*should_report)(bool got_ip, const char *ip, bool already_reported);
    bool (*reported_after)(bool already_reported, bool send_ok);
    /* ★ 2026-10-12〔本批〕会话边界（Wi-Fi 断开）⇒ 复位"已报过"（`wifi_prov_status.h` 的 ④） */
    bool (*reported_after_disconnect)(bool already_reported);
} status_policy_t;

static void fixture_4(const status_policy_t *p)
{
    printf("[夹具④] prov.status{connected}：非空 ip + 只报一次 + 失败不置位\n");

    /* ④a 正对照：拿到 IP ⇒ 报（ip 非空） */
    expect_bool("④a 已入网 + ip=192.168.110.80 ⇒ **报** connected",
                p->should_report(true, "192.168.110.80", false), true);

    /* ④b–④d **负向对照**：三种"没有真 IP"的形态都**不许**报 */
    expect_bool("④b **负向** 未入网 + ip=\"\" ⇒ 不报",
                p->should_report(false, "", false), false);
    expect_bool("④c **负向** 已入网但 ip=\"\" ⇒ 不报（契约：connected 时 ip 必填）",
                p->should_report(true, "", false), false);
    expect_bool("④d **负向** 已入网但 ip=NULL ⇒ 不报",
                p->should_report(true, NULL, false), false);
    expect_bool("④d' **负向** 未入网但 ip 非空（旧回调/重连窗口）⇒ 不报",
                p->should_report(false, "192.168.110.80", false), false);

    /* ④e **负向：不刷屏** —— 连续 5 次 GOT_IP（每次都发成功）只允许报 1 次 */
    {
        bool reported = false;
        int sent = 0;
        int i;
        for (i = 0; i < 5; i++) {
            if (p->should_report(true, "10.0.0.7", reported)) {
                sent++;
            }
            reported = p->reported_after(reported, true);
        }
        g_checks++;
        if (sent != 1) {
            g_fail++;
            printf("  \xe2\x9c\x97 ④e **负向** 连续 5 次 GOT_IP ⇒ 期望只报 1 次，实测 %d 次（刷屏）\n",
                   sent);
        } else {
            printf("  \xe2\x9c\x93 ④e **负向** 连续 5 次 GOT_IP ⇒ 只报 1 次（不刷屏）\n");
        }
    }

    /* ④f 失败不置位：发送失败 ⇒ 下一次 GOT_IP 仍要报；成功之后才不再报 */
    {
        bool reported = false;
        reported = p->reported_after(reported, false);
        expect_bool("④f 首次发送**失败** ⇒ 幂等位仍为 false（⛔ 不把发失败写成已上报）",
                    reported, false);
        expect_bool("④f 发送失败后的下一次 GOT_IP ⇒ 仍**报**（补报）",
                    p->should_report(true, "10.0.0.7", reported), true);
        reported = p->reported_after(reported, true);
        expect_bool("④f 发送**成功**之后 ⇒ 不再报", p->should_report(true, "10.0.0.7", reported),
                    false);
    }

    /* ④g 掉线重连（已有一次成功上报）⇒ 不重复 */
    expect_bool("④g 掉线重连后再拿 IP（已报过）⇒ 不重复（只报一次跨掉线成立）",
                p->should_report(true, "10.0.0.7", true), false);
}

/* --------------------------------------------------- 夹具⑥：会话边界复位（2026-10-12 本批）
 *
 * 真机证据（⛔ 不重复采集，2026-10-12 两轮）：
 *   · 15:25 第一轮：`GOT_IP 192.168.110.90` ⇒ `★ 已上报 prov.status{state=connected}`（只报一次）；
 *   · 15:36 第二轮（栈修好后重跑）：`[prov-switch] 先断开现有连接 rc=0 ⇒ set_config + connect`
 *     → `GOT_IP`（1.3 s）→ provider 返回 `ESP_OK` ⇒ **此后没有再报 connected**
 *     ⇒ App 一直停在"配网中"直到超时。
 *   成因 = ② 的幂等位**跨会话不复位**：第二轮是**新的一次配网**，位却还停在第一轮的 true。
 *
 * 被断言的形状（`wifi_prov_status.h` 的 ④ `wifi_prov_connected_reported_after_disconnect()`）：
 *   · **复位**：Wi-Fi 断开（这一次配网会话结束）⇒ 幂等位作废 ⇒ 下一段会话**可以再报一次**；
 *   · ⛔ 判据本体（① ip 非空 / ② 本会话只报一次）**一字未改** —— 复位的是**状态**，不是判据；
 *   · 专项负向对照 = 变异⑥（断开**不**复位 = 旧行为）⇒ 本夹具必须报红（否则等于空转）。
 */
static void fixture_6(const status_policy_t *p)
{
    printf("[夹具⑥] 会话边界复位：断开 ⇒ 重新配网可再报 connected（各一次，⛔ 不刷屏）\n");

    /* ⑥a 第一轮配网：报一次；**同一会话**内不再报（② 仍成立） */
    bool reported = false;
    expect_bool("⑥a 第一轮 GOT_IP（未报过）⇒ **报**",
                p->should_report(true, "192.168.110.90", reported), true);
    reported = p->reported_after(reported, true);
    expect_bool("⑥a 同一会话内第二次 GOT_IP（已报过）⇒ 不报（不刷屏）",
                p->should_report(true, "192.168.110.90", reported), false);

    /* ⑥b 断开 = 这一次配网会话结束 ⇒ 复位 */
    reported = p->reported_after_disconnect(reported);
    expect_bool("⑥b Wi-Fi 断开（会话结束）⇒ 幂等位**复位**（false）", reported, false);

    /* ⑥c 第二轮配网（重新拿到 IP）⇒ **可以再报一次**（真机缺陷现场） */
    expect_bool("⑥c 重连后再拿 IP ⇒ **再次报**（App 不再卡在「配网中」）",
                p->should_report(true, "192.168.110.91", reported), true);
    reported = p->reported_after(reported, true);
    expect_bool("⑥c 第二轮会话内不再报（仍是本会话只报一次）",
                p->should_report(true, "192.168.110.91", reported), false);

    /* ⑥d 断开-重连 ×3 ⇒ 每一段会话**各自恰好 1 次**（不是 0 次、也不是每次 GOT_IP 都报） */
    {
        int sent = 0;
        int i;
        reported = p->reported_after_disconnect(reported);
        for (i = 0; i < 3; i++) {
            if (p->should_report(true, "10.1.1.1", reported)) {      /* 重连后拿到 IP */
                sent++;
            }
            reported = p->reported_after(reported, true);
            if (p->should_report(true, "10.1.1.1", reported)) {      /* 同会话内重复 GOT_IP */
                sent++;
            }
            reported = p->reported_after_disconnect(reported);       /* 再次断开 */
        }
        g_checks++;
        if (sent != 3) {
            g_fail++;
            printf("  \xe2\x9c\x97 ⑥d 断开-重连 ×3 ⇒ 期望每段会话各报 1 次（共 3 次），实测 %d 次"
                   "（0 次 = 未复位；> 3 = 每段会话内刷屏）\n", sent);
        } else {
            printf("  \xe2\x9c\x93 ⑥d 断开-重连 ×3 ⇒ 每段会话各报 1 次（共 3 次）\n");
        }
    }

    /* ⑥e **负向**：复位只动状态 —— 复位后仍未拿到 IP ⇒ 仍不报（① 未放宽） */
    reported = p->reported_after_disconnect(true);
    expect_bool("⑥e **负向** 复位后仍未拿到 IP（got_ip=false）⇒ 仍**不报**（⛔ 复位不放宽 ip 判据）",
                p->should_report(false, "", reported), false);
    expect_bool("⑥e **负向** 复位后 `ip=NULL` ⇒ 仍**不报**",
                p->should_report(true, NULL, reported), false);
}

static int run_status_fixtures(const status_policy_t *p)
{
    printf("=== prov.status{connected} 判据：%s ===\n", p->name);
    g_fail = 0;
    g_checks = 0;
    fixture_4(p);
    fixture_6(p);
    printf("  小计：%d 条断言，%d 条不符\n", g_checks, g_fail);
    return g_fail;
}

static int run_fixtures(const policy_t *p)
{
    printf("=== 判据：%s ===\n", p->name);
    g_fail = 0;
    g_checks = 0;
    fixture_1(p);
    fixture_2(p);
    fixture_3(p);
    fixture_5(p);
    printf("  小计：%d 条断言，%d 条不符\n", g_checks, g_fail);
    return g_fail;
}

/* ------------------------------------------------------------------ 变异判据（负向对照） */

/* 变异①：rank 反向（其他 0 < BLE 1 < SD 2）—— 即"优先级不是 SD > BLE > 其他" */
static int mutant_rank(wifi_prov_channel_t ch)
{
    switch (ch) {
    case WIFI_PROV_CHAN_FILE: return 2;
    case WIFI_PROV_CHAN_BLE:  return 1;
    default:                  return 0;
    }
}

/* 变异②：丢掉"只入网一次"守卫（入网后仍接受新凭据） */
static wifi_prov_decision_t mutant_decide(bool held_valid, wifi_prov_channel_t held,
                                          wifi_prov_channel_t incoming, bool onboarded)
{
    (void)onboarded;
    if (!held_valid) {
        return WIFI_PROV_DECISION_ACCEPT_FIRST;
    }
    if (incoming == held) {
        return WIFI_PROV_DECISION_ACCEPT_SAME;
    }
    if (mutant_rank(incoming) < mutant_rank(held)) {
        return WIFI_PROV_DECISION_ACCEPT_PREEMPT;
    }
    return WIFI_PROV_DECISION_REJECT_LOWER;
}

/* 变异③（针对夹具④）：丢掉"ip 非空"判据 —— 只要没报过就报（拿不到 IP 也报 connected） */
static bool mutant_should_report(bool got_ip, const char *ip, bool already_reported)
{
    (void)got_ip;
    (void)ip;
    return !already_reported;
}

/* 变异④（针对夹具④）：发送**失败**也算"已报过" ⇒ App 永远等不到 connected */
static bool mutant_reported_after(bool already_reported, bool send_ok)
{
    (void)already_reported;
    (void)send_ok;
    return true;
}

/* 变异⑥（针对夹具⑥，= **本批要修掉的旧行为**）：Wi-Fi 断开**不复位** ⇒ 第二轮配网再也报不出
 *   `connected`（App 卡在"配网中"的原因）。
 *   ⛔ 这条负向对照存在的意义：万一有人把 `wifi_prov_connected_reported_after_disconnect()`
 *     改回 `return already_reported;`（或干脆删掉复位边），夹具⑥ 必须立刻红，而不是继续全绿。 */
static bool mutant_reported_after_disconnect(bool already_reported)
{
    return already_reported;      /* 旧行为：跨会话永不复位 */
}

/* 变异⑤（针对夹具⑤，= **本批要修掉的旧行为**）：把"已入网一律拒绝"放回
 *   —— 不看通道、不放行权威通道 ⇒ 夹具⑤ 的 ⑤a/⑤b 必须报红（"App 卡在配网中"的原形态）。
 *   ⛔ 这条负向对照存在的意义：万一有人把 `onboarded && !authoritative` 的按通道分派改回
 *      `if (onboarded)`，夹具必须立刻红，而不是继续全绿。 */
static wifi_prov_decision_t mutant_decide_reject_up_all(bool held_valid, wifi_prov_channel_t held,
                                                        wifi_prov_channel_t incoming, bool onboarded)
{
    if (onboarded) {
        return WIFI_PROV_DECISION_REJECT_ALREADY_UP;   /* 旧行为：任何通道都拒 */
    }
    if (!held_valid) {
        return WIFI_PROV_DECISION_ACCEPT_FIRST;
    }
    if (incoming == held) {
        return WIFI_PROV_DECISION_ACCEPT_SAME;
    }
    if (wifi_prov_channel_rank(incoming) < wifi_prov_channel_rank(held)) {
        return WIFI_PROV_DECISION_ACCEPT_PREEMPT;
    }
    return WIFI_PROV_DECISION_REJECT_LOWER;
}

int main(void)
{
    const policy_t real = { "固件判据 wifi_prov_policy_decide()（wifi_prov_policy.h）",
                            wifi_prov_policy_decide, wifi_prov_channel_rank };
    const policy_t mutant = { "变异判据（rank 反向 + 忽略只入网一次）",
                              mutant_decide, mutant_rank };
    /* 变异⑤（夹具⑤专用的负向对照）= **本批要修掉的旧行为**：已入网一律拒绝（不看通道） */
    const policy_t mutant_old_up = { "变异判据（把「已入网一律拒绝」放回：不看通道）",
                                     mutant_decide_reject_up_all, wifi_prov_channel_rank };
    const status_policy_t real_status = {
        "固件判据 wifi_prov_connected_should_report()（wifi_prov_status.h）",
        wifi_prov_connected_should_report, wifi_prov_connected_reported_after,
        wifi_prov_connected_reported_after_disconnect };
    const status_policy_t mutant_status = {
        "变异判据（丢掉 ip 非空判据 + 发送失败也置位）",
        mutant_should_report, mutant_reported_after,
        wifi_prov_connected_reported_after_disconnect };
    /* 变异⑥（夹具⑥专用）= **本批要修掉的旧行为**：断开不复位（判据本来那一份 + 只换复位跃迁） */
    const status_policy_t mutant_no_reset_status = {
        "变异判据（断开不复位 = 本批要修掉的旧行为）",
        wifi_prov_connected_should_report, wifi_prov_connected_reported_after,
        mutant_reported_after_disconnect };

    int real_fail = run_fixtures(&real);
    real_fail += run_status_fixtures(&real_status);

    printf("=== 负向对照（同一组夹具 × 变异判据，**必须报红**）===\n");
    int mutant_fail = run_fixtures(&mutant);
    mutant_fail += run_status_fixtures(&mutant_status);
    if (mutant_fail == 0) {
        printf("\xe2\x9c\x97 负向对照失败：变异判据也全绿 ⇒ 本夹具对判据没有区分力（空转）\n");
        return 1;
    }
    printf("\xe2\x9c\x93 负向对照通过：变异判据被夹具抓出 %d 条不符（夹具非空转）\n", mutant_fail);

    /* ★ 2026-10-12 专项负向对照：把 `REJECT_ALREADY_UP` **放回**（不看通道）⇒ 夹具⑤ 必须红。
     *   它证明"App 每次发起配网都直接生效"这条裁定**是可判定的**，而不是注释声称。 */
    printf("=== 负向对照（变异⑤：把「已入网一律拒绝」放回 ⇒ 夹具⑤ 必须报红）===\n");
    int mutant_up_fail = run_fixtures(&mutant_old_up);
    if (mutant_up_fail == 0) {
        printf("\xe2\x9c\x97 负向对照失败：把 REJECT_ALREADY_UP 放回（不看通道）后夹具⑤ 仍全绿"
               " ⇒ 「断旧连新」判据没有区分力（空转）\n");
        return 1;
    }
    printf("\xe2\x9c\x93 负向对照通过：把 REJECT_ALREADY_UP 放回后被夹具抓出 %d 条不符"
           "（断旧连新可判定）\n", mutant_up_fail);

    /* ★ 2026-10-12 专项负向对照（夹具⑥）：Wi-Fi 断开**不复位**（= 本批要修掉的旧行为）⇒ 夹具⑥ 必须红。
     *   它证明"重新配网后可以**再报一次** connected（同一会话内仍只报一次）"是可判定的。 */
    printf("=== 负向对照（变异⑥：断开不复位 ⇒ 夹具⑥ 必须报红）===\n");
    int mutant_no_reset_fail = run_status_fixtures(&mutant_no_reset_status);
    if (mutant_no_reset_fail == 0) {
        printf("\xe2\x9c\x97 负向对照失败：断开不复位（旧行为）后夹具⑥ 仍全绿"
               " ⇒ 「会话边界复位」判据没有区分力（空转）\n");
        return 1;
    }
    printf("\xe2\x9c\x93 负向对照通过：断开不复位被夹具⑥ 抓出 %d 条不符"
           "（会话边界复位可判定）\n", mutant_no_reset_fail);

    if (real_fail != 0) {
        printf("\xe2\x9c\x97 RESULT=FAIL 固件判据有 %d 条断言不符（ADR-0017 优先级/只入网一次"
               " ＋ 权威通道断旧连新 ＋ prov.status{connected} 上报与会话边界复位）\n", real_fail);
        return 1;
    }
    printf("RESULT=PASS 优先级（SD>BLE>其他）＋「只入网一次（权威通道 BLE/App 除外）」"
           "＋ 权威通道**断旧连新**（恰好一次断链 / 旧 SSID 不再被采信）"
           "＋ prov.status{connected} 上报判据（非空 ip / 同一会话只报一次 / 失败不置位）"
           "＋ **会话边界复位**（Wi-Fi 断开 ⇒ 重新配网可再报一次）成立（ADR-0017 D2/D4）\n");
    return 0;
}
