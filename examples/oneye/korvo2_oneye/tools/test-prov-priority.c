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
 *
 * 夹具（对应用户口径「优先级 = SD 卡 WiFi 凭据 > 蓝牙配网 > 其他」）：
 *   ① SD 凭据在 + BLE 也提交了 ⇒ **SD 胜出**（旧形态下无法判定：两条通道各自入网）
 *      ①a SD 先、BLE 后   ①b 到达顺序反过来（BLE 先、SD 后）   ①c 掉线不影响"谁被采信"
 *   ② SD 不在 + BLE 提交 ⇒ **BLE 胜出**；且「其他」层（台面 API / Kconfig / 预留通道）不越权
 *   ③ **只入网一次**：入网后任何通道（含更高优先级）的提交一律被拒；
 *      唯一能重新配网的路径是**显式释放采信槽**（台面改配）或 prov.reset
 *
 * 构建/运行（WSL 或任何有 cc 的环境）：
 *   cc -std=c99 -Wall -Wextra -Werror -I ../main test-prov-priority.c -o /tmp/tpp && /tmp/tpp
 * 退出码：0 = 全部夹具通过且负向对照报红；1 = 有夹具失败，或负向对照未报红。
 */

#include <stdio.h>

#include "wifi_prov_policy.h"   /* ← 固件里跑的同一份判据（纯逻辑头，无 ESP 依赖） */

/* 若有人把上面那行换成"本地抄件"，本 include guard 就不存在了 ⇒ 编译期直接失败。 */
#ifndef _WIFI_PROV_POLICY_H_
#error "必须 include 固件的 wifi_prov_policy.h（判据唯一事实源），不得另抄一份"
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
    printf("[夹具③] 重复入网被拒（**只入网一次**）\n");

    slot_t a = { false, WIFI_PROV_CHAN_UNKNOWN, false };
    expect("③a 空槽 + SD 提交并入网", submit(p, &a, WIFI_PROV_CHAN_FILE, true),
           WIFI_PROV_DECISION_ACCEPT_FIRST);
    expect("③b 已入网 + SD 再提交（同通道重试）⇒ 拒绝",
           submit(p, &a, WIFI_PROV_CHAN_FILE, false), WIFI_PROV_DECISION_REJECT_ALREADY_UP);
    expect("③c 已入网 + BLE 提交 ⇒ 拒绝",
           submit(p, &a, WIFI_PROV_CHAN_BLE, false), WIFI_PROV_DECISION_REJECT_ALREADY_UP);
    expect("③d 已入网 + 台面 API 提交 ⇒ 拒绝",
           submit(p, &a, WIFI_PROV_CHAN_API, false), WIFI_PROV_DECISION_REJECT_ALREADY_UP);
    expect("③e 已入网 + 预留通道（二维码）提交 ⇒ 拒绝",
           submit(p, &a, WIFI_PROV_CHAN_QRCODE, false), WIFI_PROV_DECISION_REJECT_ALREADY_UP);

    /* ③f 已入网后**连更高优先级也不能抢占** */
    slot_t b = { false, WIFI_PROV_CHAN_UNKNOWN, false };
    expect("③f 空槽 + BLE 提交并入网", submit(p, &b, WIFI_PROV_CHAN_BLE, true),
           WIFI_PROV_DECISION_ACCEPT_FIRST);
    expect("③f 已入网 + 更高优先级的 SD 提交 ⇒ 仍拒绝（入网后不抢占）",
           submit(p, &b, WIFI_PROV_CHAN_FILE, false), WIFI_PROV_DECISION_REJECT_ALREADY_UP);

    /* ③g 显式释放是**唯一**让已入网设备重新配网的路径（台面改配口径） */
    expect("③g 未释放时不放行（对照：证明「释放」是必要条件）",
           submit(p, &b, WIFI_PROV_CHAN_BLE, false), WIFI_PROV_DECISION_REJECT_ALREADY_UP);
    release_held(&b);
    expect("③g 显式释放采信槽后（仍在线上）+ SD 提交 ⇒ 采信",
           submit(p, &b, WIFI_PROV_CHAN_FILE, true), WIFI_PROV_DECISION_ACCEPT_FIRST);

    /* ③h prov.reset（断连 + 清槽）之后可以重新配网，且仍然只入网一次 */
    slot_t c = { false, WIFI_PROV_CHAN_UNKNOWN, false };
    expect("③h prov.reset 之后 + BLE 提交 ⇒ 采信",
           submit(p, &c, WIFI_PROV_CHAN_BLE, true), WIFI_PROV_DECISION_ACCEPT_FIRST);
    expect("③h 重新入网后 + 台面 API 提交 ⇒ 拒绝（只入网一次对所有通道一致）",
           submit(p, &c, WIFI_PROV_CHAN_API, false), WIFI_PROV_DECISION_REJECT_ALREADY_UP);
    prov_reset(&c);   /* 与 forget() 同形，供下一条断言使用 */
    expect("③h prov.reset 之后再提交（槽空、已断连）⇒ 采信",
           submit(p, &c, WIFI_PROV_CHAN_BLE, true), WIFI_PROV_DECISION_ACCEPT_FIRST);
}

static int run_fixtures(const policy_t *p)
{
    printf("=== 判据：%s ===\n", p->name);
    g_fail = 0;
    g_checks = 0;
    fixture_1(p);
    fixture_2(p);
    fixture_3(p);
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

int main(void)
{
    const policy_t real = { "固件判据 wifi_prov_policy_decide()（wifi_prov_policy.h）",
                            wifi_prov_policy_decide, wifi_prov_channel_rank };
    const policy_t mutant = { "变异判据（rank 反向 + 忽略只入网一次）",
                              mutant_decide, mutant_rank };

    const int real_fail = run_fixtures(&real);

    printf("=== 负向对照（同一组夹具 × 变异判据，**必须报红**）===\n");
    const int mutant_fail = run_fixtures(&mutant);
    if (mutant_fail == 0) {
        printf("\xe2\x9c\x97 负向对照失败：变异判据也全绿 ⇒ 本夹具对判据没有区分力（空转）\n");
        return 1;
    }
    printf("\xe2\x9c\x93 负向对照通过：变异判据被夹具抓出 %d 条不符（夹具非空转）\n", mutant_fail);

    if (real_fail != 0) {
        printf("\xe2\x9c\x97 RESULT=FAIL 固件判据有 %d 条断言不符（ADR-0017 优先级/只入网一次）\n",
               real_fail);
        return 1;
    }
    printf("RESULT=PASS 优先级（SD>BLE>其他）与「只入网一次」成立（ADR-0017 D2/D4）\n");
    return 0;
}
