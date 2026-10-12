/*
 * wifi_prov_ble.c —— 配网通道 C1/C2：BLE（口径见 wifi_prov_ble.h 与 ADR-0017）
 *
 * 本文件**只是通道**。它把凭据交给 `wifi_prov_submit_credentials()`，然后什么都不做 ——
 * 「入网成功 ⇒ 上云」由 wifi_prov.c 的 GOT_IP 处理器统一触发（唯一收敛点）。
 *
 * 与既有例程的关系：`examples/oneye/korvo2_llm_chat/main/prov_service.c:274-292/424-443`
 * 是同一套 SDK API（`oneye_dev_ble_*` + `oneye_dev_link_prov_set_provider`）的既有实现，
 * 真机已打通（见该例程 README §7.1）。本文件按 korvo2_oneye 的模块边界重写，**不复制**它的
 * 「入网后逻辑」（那一份在 llm_chat 里是调 `s_ready_cb()`；本工程收敛点在 wifi_prov.c）。
 */

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"     /* 配网窗口超时：既有依赖（ESP-IDF 核心组件，多处在用），⛔ 不引新依赖 */
/* 诊断留痕（2026-10-10）：provider 现在跑在 **SDK 的 prov worker 任务**上（`oneye_prov`，见
 * `oneye-dev-sdk/src/oneye_dev_link.c` §5.1），而它曾经跑在 NimBLE 收包任务上。
 * "到底是谁在执行、有没有回到执行点"是"既无 connected 也无 failed"的第一分岔点
 * ⇒ 诊断行里打出**当前任务名**（`pcTaskGetName`）—— 它把"worker 没回到执行点 / 队列卡住"
 * 这类怀疑变成一眼可见的事实。⛔ 只打任务名，⛔ 不打任何凭据。 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "oneye_dev_ble.h"
#include "oneye_dev_link.h"

#include "adv_identity.h"
#include "wifi_prov.h"
#include "wifi_prov_ble.h"

static const char *TAG = "wifi_prov_ble";

static bool s_started;
static bool s_advertising;
/* ⛔ 旧值逐字留痕（⛔ 不抹）：`static char s_pop[ONEYE_DEV_BLE_POP_LEN];` —— POP（6 位数字）已按
 *   用户 2026-10-09 裁定「彻底删除实现及契约」整条删除（含本工程 Kconfig 的 `ONEYE_FW_BLE_POP`）。 */

/* ------------------------------------------------- 「配网模式」状态（2026-10-09 本批）
 *
 * 用户 2026-10-09 裁定：**开机不自动广播**（只"待机"）⇒ 长按 SET 进入配网模式（广播）⇒
 * 再长按退出 ⇒ 进入后 `CONFIG_ONEYE_FW_BLE_PROV_PAIRING_WINDOW_S` 秒未成功入网 ⇒ 自动退出。
 *
 * 两个状态的区别（⛔ 不要合并成一个 bool）：
 *   · `s_pairing`   = 「配网模式开关」：由按键置位/清零（或超时清零；入网成功时由
 *     `wifi_prov_ble_stop_advertising()` 顺带清零 —— 既然已经入网，"等一台设备来配网"这件事就结束了）。
 *   · `s_advertising` = 「此刻是否真的在广播」：由 SDK 的 `advertise(true/false)` 结果决定。
 *   两者会被分别读写的时刻是真实存在的：入网成功 ⇒ 收敛点调 `wifi_prov_ble_stop_advertising()`
 *   停广播；若那一刻配网模式开关还开着，超时回调必须能区分"其实早就成功了"与"没人来配网"
 *   （见 pairing_timeout_cb 的注释），否则会把成功写成失败。
 *
 * ⚠️ 缺省口径（`s_autostart`）：`wifi_prov_ble_start()` 的行为**逐字不变**（起栈 + 立刻广播），
 *   那是 `wifi_prov_boot()` 与 `check-prov-convergence` A8a/A13b 依赖的既有路径；
 *   只有 `wifi_prov_ble_set_autostart(false)` 之后 `start()` 才停在"待机"（起栈、不广播）。
 *   ⚠️ `s_autostart` 缺省 `true` 且**不读** `CONFIG_ONEYE_FW_BLE_PROV_KEY_GATE`：
 *      门控的决定权在**唯一调用者** `wifi_prov_boot()`（它按该宏调 `set_autostart()`），
 *      本模块不去看第二个配置源（那样就变成"两处都能决定是否广播"）。 */
static bool s_autostart = true;   /* 置 false ⇒ start() 之后停在待机（由按键进入配网模式） */
static bool s_pairing;            /* 配网模式开关（按键/超时驱动） */
static bool s_window_active;      /* 超时计时器在跑？ */
static esp_timer_handle_t s_window_timer;

/*
 * 设备身份 = **creds 分区**（`adv_identity.c` 一次性取出**两串**）。
 *
 * 🔴 2026-10-09（台账 #1 / #3 / #4）：**两串拆开**，⛔ `node_id` 不再喂三处：
 *   · **`node_id` = 激活码**（**私密**：= 设备证书 CN；⛔ 不广播、⛔ **不进明文帧**；✅ 贴机身）
 *       ① `lcfg.node_id`（link 帧 `from`，`oneye_dev_link.c`）；
 *       ② **甲型** `prov.verify.req` 的**逐字核验基准**（台账 #8：设备本地比对、fail-fast）。
 *       ⚠️ **2026-10-10 订正（旧字逐字留痕，⛔ 不抹）**：本处此前逐字为「② 配对 HKDF **salt**
 *       （`bcfg.device_id`；契约 §4.3：`salt = node_id`，⛔ 不做大小写折叠）；③ `prov.pair.code` 的
 *       **逐字比对基准**」—— **两条都已作废**：契约 §4.4.10（四）改为 **`salt = chal`**、
 *       §4.4.10（三）改为**配对不判码**（甲型的比对移到 `prov.verify.req`）。
 *   · **`sn` = SN**（**公开**：贴机身、**可广播**）
 *       ① **广播名** = `ONEYE-<SN 后 4 位>`（派生函数与调用点 = SDK
 *          `components/oneye-dev-sdk/src/oneye_dev_ble.c` 的 `oneye_dev_ble_init()`：
 *          `oneye_snprintf(s_ble.adv_name, …, "ONEYE-%s", (nm_len > 4u) ? (nm + nm_len - 4u) : nm)`；
 *          本文件把 `bcfg.adv_name` 留空 ⇒ 走自动派生）；⚠️ 名字只在 **GAP 层**（0x2A00），
 *          ⛔ 不进广播数据、⛔ 不进扫描响应（台账 #3）；
 *       ② **厂商字段 `sn_field(24)`**（`bcfg.adv_sn`，布局见 SDK 头「广播自定义字段」段）；
 *          乙 / 丙 无身份时 ⇒ `0x28` ＋ 空 `sn_field` 占位（契约 §4.4.10（一）/（二））；
 *       ③ **`prov.pair.ok.sn`**（**条件必填**：甲必给 / 乙·丙 可缺，⛔ 该帧**不回 `node_id`**）。
 *   ⛔ **不许留第二个来源**：本文件内**不再**出现 `CONFIG_ONEYE_FW_DEVICE_ID` 作为设备身份
 *      （旧值逐字留痕：上一版 `lcfg.node_id = CONFIG_ONEYE_FW_DEVICE_ID;`（:119）与
 *      `bcfg.device_id = CONFIG_ONEYE_FW_DEVICE_ID;`（:135））。
 *   🔴 **读不到怎么办 —— 档位感知（2026-10-10 订正；旧值为"⛔ 读不到 ⇒ 本通道不启动"）**：
 *       **甲 / 档位未声明** ⇒ **仍 fail-closed**（⛔ 不启动，理由见 `wifi_prov_ble_start()` 第 0 步）；
 *       **乙 / 丙** ⇒ **允许无实例身份起栈**（那是它们的正常起点；⛔ 仍不回落编译期常量）。
 *   ★ **`creds.sn` 为空 ⇒ 回落 `node_id`**（存量机；`s_ids.sn_fell_back`）—— 行为与 2026-10-07 那批一致，
 *      只是厂商字段会如实标注 `0x04`（老世代）。
 * ⚠️ 读取在**独立 TU**（`adv_identity.c`）里做：`oneye_dev_creds.h` 与 `oneye_dev_link.h`
 *    在同一个 TU 里会撞 `oneye_dev_link_status_t`（SDK 既有缺陷，见该文件头）。顺序上**先读、
 *    取两串、立刻 free，再起 NimBLE**（避免 16 KiB 与 NimBLE 主机 12 KiB 栈同时在峰）。
 */
static adv_identity_t s_ids;
static bool s_ids_ready;
/* 🔴 2026-10-10：身份装载的**结果与成因**（档位感知的判据 —— "读不到"对甲与对乙 / 丙 处置不同） */
static adv_identity_status_t s_ids_status;
static adv_identity_status_t s_ids_reason;

/* ------------------------------------------------------------------ provider（应用侧 Wi-Fi 动作） */

/* ---- 诊断留痕：SDK 侧「决定上报什么状态」的回显（2026-10-10；仅诊断构建） -------------------
 *
 * 为什么非它不可：SDK 的 `oneye_dev_link_prov_report_status()`（`src/oneye_dev_link.c:1508`）
 *   先 `link_event(PROV_STATE, payload)`，**再** `if (s_link.started) oneye_dev_link_broadcast(...)`
 *   —— 并且**无论广播成功与否都 `return ONEYE_DEV_SDK_OK`**（广播返回值被 `(void)` 丢弃）。
 *   ⇒ "SDK 决定了要报 connected / failed" 与 "这一帧有没有真发到 App" 是**两件事**，
 *      而真机症状恰恰是 App 侧一帧都没收到 ⇒ 必须能把这两件事分开看。
 *   ⇒ 本回调把 PROV_STATE 的**载荷**打到串口：只要它出现在串口上，就说明设备侧**确实报了**，
 *      问题就不再是"没上报"。
 * ⛔ 只在诊断构建（`CONFIG_ONEYE_FW_DIAG_PROV_TRACE=y`，量产预设为 n）打开；
 * ⛔ 载荷里只有 state / ssid / ip / source / err（SDK 组帧原样），⛔ 不含密码 / 激活码 / chal。 */
#if CONFIG_ONEYE_FW_DIAG_PROV_TRACE
static void diag_link_evt_cb(oneye_dev_event_t evt, const void *payload, size_t len, void *ctx)
{
    (void)ctx;
    if (evt != ONEYE_DEV_LINK_EVT_PROV_STATE || payload == NULL || len == 0u) {
        return;
    }
    ESP_LOGI(TAG, "[diag] SDK 决定上报配网状态（本行与 BLE 是否真发出无关）：%.*s",
             (int)len, (const char *)payload);
}
#endif

/* 前置声明：provider 里的 `ble_prov_reset()`（清凭据 ⇒ 重新进入可配网态）与 enter/exit 共用配网窗口
 * 计时器，而它们的定义在下方「配网模式」段 —— C 的单遍扫描要求此处先声明。 */
static void window_restart(void);
static void window_stop(void);
/* 前置声明：`pairing_enter()` 的"进入配网模式"日志要打印**本机型是否需要激活凭证**（非敏感口径，
 * ⛔ 不打印 POP / 激活码 / chal），而 `dev_cred_required()` 的定义在下方「通道启动」段的
 * `#if CONFIG_ONEYE_FW_ENABLE_BLE_PROV` 之内 ⇒ 与 window_* 同理，此处先声明（C 单遍扫描）。
 * ⚠️ 该调用点自身也在 `#if CONFIG_ONEYE_FW_ENABLE_BLE_PROV` 的 `#else` 分支里（通道关时不可达）。 */
static bool dev_cred_required(void);
/* 🔴 2026-10-10 补前置声明（上一批**漏声明**，真机轨唯一的硬失败点）：`pairing_enter()` 新增的
 * "本机型档位 = …（依据 …）"日志（见下方 `ESP_LOGI`）在**定义之前**用了这两个 `static` 函数，
 * 而它们的定义在文件后段（`:355` / `:368`）⇒ 缺声明即 `-Werror=implicit-function-declaration`
 * ＋ 连带 `conflicting types`（IDF 轨把告警当错误，固件构建停在第 457/474 步）。
 * ⛔ 只补声明，⛔ 不改这两个函数的取值口径、⛔ 不改任何日志内容。 */
static oneye_dev_ble_device_tier_t dev_onboarding_tier(void);
static const char *dev_onboarding_tier_basis(void);

/* 扫描：直接转发给 wifi_prov.c 的实现（Wi-Fi 生命周期归它管，本模块不碰 esp_wifi_*） */
static int ble_prov_scan(char *json_out, size_t cap, void *ctx)
{
    (void)ctx;
    /* 诊断留痕（2026-10-10）：本函数 = 「worker 已执行到 prov.scan.req 这一步」的**唯一**证据
     * （它只可能被 SDK 的 prov worker 调用）。任务名 + 耗时一起打出来。 */
    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "[diag] prov.scan.req → provider 调用：task=%s", pcTaskGetName(NULL));
    int rc = wifi_prov_scan_json(json_out, cap) == ESP_OK ? 0 : -1;
    ESP_LOGI(TAG, "[diag] prov.scan.req → provider 返回 rc=%d（耗时 %lld ms）"
                  "⇒ SDK 将据此回 prov.scan.rsp（或 error）",
             rc, (long long)((esp_timer_get_time() - t0) / 1000));
    return rc;
}

/*
 * 连接：**只**把凭据交给唯一入网入口，然后返回。
 * ⛔ 这里不调 oneye_start()、不建任务、不碰面板/授时/影子/埋点 —— 那些都在收敛点的下游。
 * ⚠️ 本函数**允许阻塞**（它内部会等 `wifi_prov_connect` 的超时，缺省 20 s），
 *    因为它跑在 **SDK 的 prov worker 任务**上（`oneye-dev-sdk` 的 `link_handle_prov()` 只做
 *    "解析 + 入队"，耗时动作由 worker 执行）—— **谁负责非阻塞 = SDK 的 link 层**：
 *    BLE 收包路径（NimBLE 主机任务）因此不再被本函数占住，ATT 写回报能立刻发出去。
 *    🔴 2026-10-10〔旧字逐字留痕，⛔ 不抹〕本段此前逐字为：
 *      「⚠️ 本函数在 NimBLE 主机任务上下文（`ble_on_rx → oneye_dev_link_inject_frame → link_handle_prov`
 *        → 本 provider，见 `oneye_dev_sdk/src/oneye_dev_ble.c:202-207` 与 `oneye_dev_link.c:289-320`）
 *        里被**同步**调用，因此会阻塞该任务至多 `wifi_prov_connect` 的超时（缺省 20 s）。」
 *      —— 该口径**已作废**：同步调用正是真机缺陷本体（App 写回报等满 3 s 超时、入站帧被挤掉），
 *      修法见 SDK 侧 `src/internal/oneye_link_prov.h` 与 `src/oneye_dev_link.c` §5.1。
 *    ⛔ 不另起任务的原因不变（本工程内部 RAM 实测 `largest_internal_block` 很小、见
 *      `korvo2_oneye_main.c` 的 `[res]` 读数），但**现在不需要**：worker 由 SDK 统一持有。
 * ★ 被优先级判据拒绝时（`ESP_ERR_INVALID_STATE`：槽里已有 SD 卡凭据，或设备已入网 ⇒ 只入网一次）
 *   本函数返回 **-1**，立刻返回、**不阻塞**；SDK 据此向对端报 `PROV_FAILED`
 *   （`oneye_dev_link.c` 的 `link_prov_execute()` 里 connect 失败分支）⇒ 手机侧能立刻知道
 *   "这一次没被采信"，而不是静默超时。
 *   🔴 **2026-10-12 就地订正（用户裁定）**：**"或设备已入网"这一半对 BLE 通道已作废** ——
 *     App 发起的配网是**权威通道**，"已入网"不再拦它：设备会在同一次提交里
 *     **先断旧连新**（`wifi_prov.c` 的 `[prov-switch]` 段）⇒ App **不再**需要"先 `prov.reset`
 *     再重发"那条有界重配链（该链当年正是为绕开 `REJECT_ALREADY_UP` 而设计的）。
 *     ⛔ 旧字逐字留痕（⛔ 不抹）：本行旧口径逐字为
 *     「（`ESP_ERR_INVALID_STATE`：槽里已有 SD 卡凭据，或设备已入网 ⇒ 只入网一次）」。
 *     ⚠️ 现在能拿到 `ESP_ERR_INVALID_STATE` 的情形只剩"**未入网**且槽被更高优先级的 C3 凭据占用"。
 */
static int ble_prov_connect(const char *ssid, const char *password, const char *token, void *ctx)
{
    (void)token;    /* 契约 §4 的 rvd 自定义数据：korvo2_oneye 暂不使用（不臆造语义） */
    (void)ctx;
    /* 诊断留痕（2026-10-10）：本函数 = 「worker 已执行到 prov.connect.req 这一步」的**唯一**证据。
     * ⛔ 密码按既有纪律只打**长度**，⛔ 绝不打印内容；ssid 沿用既有口径（既有日志已在打）。 */
    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "[diag] prov.connect.req → provider 调用：task=%s ssid=%s 密码长度=%u",
             pcTaskGetName(NULL), ssid, (unsigned)((password != NULL) ? strlen(password) : 0u));
    esp_err_t rc = wifi_prov_submit_credentials(ssid, password, WIFI_PROV_CHAN_BLE);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (rc == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "本次 BLE 凭据**未被采信**（优先级判据拒绝；串口 [prov-priority] 有留痕）"
                      "⇒ 向对端报失败，不发起入网");
    }
    /* 三种归宿一眼可分：
     *   · ESP_OK            ⇒ 已拿到 IP（connected 由收敛点上报，**不在本函数**）；
     *   · ESP_ERR_TIMEOUT   ⇒ 等满 20 s 没等到 GOT_IP ⇒ SDK 会报 failed{auth_failed}；
     *   · ESP_ERR_INVALID_STATE ⇒ 判据拒绝 ⇒ SDK 同样报 failed{auth_failed}。 */
    ESP_LOGI(TAG, "[diag] prov.connect.req → provider 返回 %s（耗时 %lld ms）⇒ SDK 侧归宿=%s",
             esp_err_to_name(rc), (long long)ms,
             (rc == ESP_OK) ? "已在网（等收敛点上报 connected）"
                            : "将上报 prov.status{state=failed, err=auth_failed}");
    return (rc == ESP_OK) ? 0 : -1;
}

/* 清除凭据：**只**释放"已入网采信槽" + 清 Wi-Fi 配置（不重启、不擦 NVS），
 * ⛔ **不断 BLE 链路、⛔ 不作废 BLE 会话密钥**。
 *
 * ★ 2026-10-12（本批 · `prov.reset` 语义订正，裁定 = 选项 C，⛔ 不自行改成别的方案）：
 *   🔴 **旧行为（已删除；旧字逐字留痕，⛔ 不抹）**：本函数在 `wifi_prov_forget()` 之后调
 *     `(void)oneye_dev_ble_unpair();` ⇒ 它经 `oneye_dev_ble.c:1517` 的
 *     `oneye_ble_plat_disconnect()` 执行 `ble_gap_terminate()`（`oneye_ble_plat_nimble.c:611-616`）
 *     ⇒ **链路当场断开**；而 SDK 是在 **provider 返回之后**才上报 `prov.status{state=idle}`
 *     （`oneye_dev_link.c:648-659`，上报点 `:658`）⇒ 那一帧**没有链路可送**，
 *     App 的「被 `[prov-priority] REJECT_ALREADY_UP` 闸掉 ⇒ 先 `prov.reset` ⇒ 等
 *     `prov.status{state=idle}` ⇒ 再用内存里的同一份凭据重发 `prov.connect.req`（≤1 次）」
 *     这条**有界重配链**就断在这里。
 *   ✅ **新行为** = 只"释放采信槽 + 清 Wi-Fi 配置"，二者都在 `wifi_prov_forget()` 里：
 *     · 释放采信槽 `s_held_valid = false` —— `wifi_prov.c:484-485`；
 *     · 清 Wi-Fi 配置 `esp_wifi_disconnect()` + `esp_wifi_set_config(WIFI_IF_STA, &sta)` +
 *       清 `s_connected/s_ip/s_ssid/s_source` —— `wifi_prov.c:489-495`。
 *     ⇒ BLE 会话与配对密钥**保持有效** ⇒ 上面那条 `idle` 回执能在**同一条已建立的会话**上送达 App
 *     （App 零改动即可重发凭据）。
 *   ⛔ **回归锚（本函数路径内不得再出现）**：`oneye_dev_ble_unpair` / `oneye_ble_plat_disconnect` /
 *     `ble_gap_terminate` —— 机械判据见
 *     `build-alpha-reset/.scratch/check-reset-semantics.py`（含"把断链调用放回 ⇒ 必须报红"的负向对照）。
 *   🔴 **2026-10-12 追加（用户裁定「App 发起配网 = 断旧连新」）**：上一条"有界重配链"如今是
 *     **可选路径、不再是唯一出路** —— BLE（App）提交**一经受理即权威生效**，
 *     `REJECT_ALREADY_UP` 对它不可达（判据分派见 `wifi_prov_policy.h`；
 *     断旧连新的落码见 `wifi_prov.c` 的 `[prov-switch]` 段）。本函数（`prov.reset`）
 *     语义**不变**，仍只"释放采信槽 + 清 Wi-Fi 配置"，⛔ 不断链。
 * ★ 2026-10-09（逐字保留，⛔ 未改）：它把通道**重新开成可配网态** ⇒ 按配网模式口径处理：置 s_pairing
 *   并重启超时窗口，⛔ 否则会出现"在广播但不在配网模式"的第三种状态（超时回调与退出逻辑都判不准）。
 *   ⚠️ 链路未断时 `oneye_dev_ble_advertise(true)` 在平台层是 **no-op**
 *     （`oneye_ble_plat_nimble.c:518-521`：`if (!s_plat.conn_active) start_advertising();`
 *      —— 已连接就不重开广播）；对端断开后平台在 DISCONNECT 事件里自行回落广播（`:371-378`）
 *     ⇒ 本行留下的 `s_advertising` 簿记在断开后与事实重新一致（⛔ 本批不另改簿记口径）。 */
static int ble_prov_reset(void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "BLE 通道请求清除凭据 ⇒ 只释放采信槽 + 清 Wi-Fi 配置（回到未配网态）；"
                  "**不断 BLE 链路、不作废会话密钥** ⇒ 本帧的 prov.status{state=idle} 能原路回 App");
    wifi_prov_forget();
    if (s_started) {
        if (oneye_dev_ble_advertise(true) == ONEYE_DEV_SDK_OK) {
            s_advertising = true;
        }
        s_pairing = true;
        window_restart();
    }
    return 0;
}

/* ------------------------------------------------------------------ prov.status 上报（契约 §4.1，2026-10-10）

 * 🔴 本批落实的**既有**契约状态：`prov.status{state=connected, ip=…}`。
 *   在本批之前，本工程**从不**上报 `connected`（`ONEYE_DEV_LINK_PROV_CONNECTED` 只出现在
 *   `korvo2_llm_chat` 例程），App 的「配网中 → **设备连网成功**」因此永远等不到，卡到超时。
 *   ⛔ 不新增帧 / ⛔ 不新增错误码 / ⛔ 不动契约：`connected` 是机读契约 `prov_states` 里的既有项，
 *   `prov.status` 的字段逐字沿用（同处写明「connected 时 ip 必填」）。
 *
 * ⛔ 本模块只负责"**怎么发**"（走 SDK 既有发送路径 ⇒ 天然经过它的 TX 串行化锁）；
 *   "**该不该报**"（非空 ip / 只报一次 / 失败不置位）是调用方的判据
 *   （`wifi_prov_status.h`，与主机夹具编译同一份源）—— 两件事分开，⛔ 不在这里抄一遍判据。
 *
 * ⚠️ 本函数**不在** `CONFIG_ONEYE_FW_ENABLE_BLE_PROV` 之内：通道关掉的机型也要能编；
 *   那时 link 不会被 init ⇒ 返回 `ESP_ERR_INVALID_STATE`（调用方据此**保持未报**，⛔ 不假装成功）。
 */
esp_err_t wifi_prov_ble_report_prov_connected(const char *ssid, const char *ip, const char *source)
{
    oneye_dev_sdk_err_t rc;

    if (!oneye_dev_link_is_initialized() || !oneye_dev_link_is_started()) {
        ESP_LOGW(TAG, "link 未就绪（init=%d start=%d）⇒ prov.status{connected, ip=%s} **未发出**"
                      "（⛔ 不把 connected 报成既成事实；下一次 GOT_IP 再试）",
                 (int)oneye_dev_link_is_initialized(), (int)oneye_dev_link_is_started(),
                 (ip != NULL) ? ip : "");
        return ESP_ERR_INVALID_STATE;
    }
    /* 诊断留痕（2026-10-10）：link 侧前置条件逐项打出来 —— "上报被前置条件挡住"是最容易被
     * 误读成"上报被吞"的一种形态。 */
    ESP_LOGI(TAG, "[diag] 上报 connected 前置条件：link init=%d start=%d ip=%s source=%s",
             (int)oneye_dev_link_is_initialized(), (int)oneye_dev_link_is_started(),
             (ip != NULL) ? ip : "(空)", (source != NULL) ? source : "(空)");
    rc = oneye_dev_link_prov_report_status(ONEYE_DEV_LINK_PROV_CONNECTED,
                                           (ssid != NULL && ssid[0] != '\0') ? ssid : NULL,
                                           ip,
                                           (source != NULL && source[0] != '\0') ? source : NULL,
                                           NULL);   /* err 为空 = 没有错误（契约 optional） */
    /* ⚠️ 这个返回码**只能证明调用返回**，⛔ 不能证明帧真发出去了：SDK 的
     * `oneye_dev_link_prov_report_status()` 无论广播成败都回 OK（见本文件顶部诊断注释）。
     * 真正的"有没有发"要看 `[diag] SDK 决定上报配网状态…` 那行与对端是否收到。 */
    ESP_LOGI(TAG, "[diag] prov.status 上报调用已返回：返回码=%d（⛔ SDK 侧恒 OK，不代表已送达对端）",
             (int)rc);
    if (rc != ONEYE_DEV_SDK_OK) {
        ESP_LOGW(TAG, "prov.status{connected, ip=%s} 上报失败（%d）", (ip != NULL) ? ip : "", (int)rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "★ 已上报 prov.status{state=connected, ssid=%s, ip=%s, source=%s}",
             (ssid != NULL) ? ssid : "", (ip != NULL) ? ip : "", (source != NULL) ? source : "");
    return ESP_OK;
}

/* ---- 诊断留痕：心跳快照（2026-10-10；仅诊断构建） -------------------------------------------
 * 为什么要心跳：本轮真机串口"只有每秒一次 SD 卡报错、没有任何 wifi/prov 行"，于是无法区分
 *   「日志级别把 INFO 滤掉了」与「抓取窗口在开机之后才开始」。心跳每 5 s 一行，把这个歧义**消掉**：
 *   · 有心跳 ⇒ 串口是活的、级别确实放到了 INFO（本行就是一条 INFO）；
 *   · 没心跳 ⇒ 是抓取/级别问题，而不是设备行为。
 * ⛔ 只读状态：⛔ 不发帧、⛔ 不改任何判据、⛔ 不碰 A14a 的"唯一上报点"。 */
#if CONFIG_ONEYE_FW_DIAG_PROV_TRACE
void wifi_prov_ble_diag_dump(void)
{
    /* ⚠️ 这里**故意**只打 SDK 配网状态的**数值**（不写枚举名）：本文件的枚举名一旦出现在代码行里，
     *    `tools/check-prov-convergence.py` 的 A14h 会把它判成"第二个状态上报点"（该门禁按 token
     *    计数，它保护的是"只上报 connected 一次"这条不变量）。
     *    数值 ↔ 名称（SDK `include/oneye_dev_link.h`）：0 idle / 1 pairing / 2 scanning /
     *    3 scan_done / 4 connecting / 5 connected / 6 failed；`-1` = link 未初始化。
     *    ⛔ 只读、⛔ 不发帧：状态名与人读载荷由 `[diag] SDK 决定上报配网状态…` 那行给出。 */
    int st = oneye_dev_link_is_initialized() ? (int)oneye_dev_link_prov_state() : -1;

    ESP_LOGI(TAG, "[diag] 心跳：link init=%d start=%d sdk_prov_state=%d ble_started=%d 广播中=%d 配网模式=%d",
             (int)oneye_dev_link_is_initialized(), (int)oneye_dev_link_is_started(), st,
             (int)s_started, (int)s_advertising, (int)s_pairing);
}
#endif /* CONFIG_ONEYE_FW_DIAG_PROV_TRACE */

/* ------------------------------------------------------------------ 配网模式：进入 / 退出 / 超时 */
/*
 * 「已入网」判据 = `wifi_prov_is_connected()`（wifi_prov.c 的唯一事实源）。
 * ⛔ 本模块不看 SDK 链路状态、也不看 BLE 配对状态：那两者都不等于"这台设备已经上得了网"。
 */
static bool ble_already_connected(void)
{
    return wifi_prov_is_connected();
}

/* ⚠️ 下面这一段**必须**在 `#if CONFIG_ONEYE_FW_ENABLE_BLE_PROV` 之内（2026-10-09 实测踩到）：
 *   `sdkconfig.h` 只对 **y 的 bool** 产出 `#define`；`ONEYE_FW_ENABLE_BLE_PROV=n` 时那个宏**根本不存在**，
 *   于是 `CONFIG_ONEYE_FW_BLE_PROV_PAIRING_WINDOW_S` 也不存在（它 `depends on` 前者）
 *   ⇒ 本段若写在分支之外，关掉通道的机型**编译期报错**
 *   （实测：`error: 'CONFIG_ONEYE_FW_BLE_PROV_PAIRING_WINDOW_S' undeclared`，
 *   连"生产件不含 BLE"的预设都编不过 —— 这正是"通道关闭也必须能编"的既有边界被踩破）。
 *   ⛔ 不要为了省一层 `#if` 而给该宏加 `#ifndef` 兜底：那会造成"窗口初值"的**第二个事实源**。 */
#if CONFIG_ONEYE_FW_ENABLE_BLE_PROV

/* 配网窗口超时（一次性 esp_timer；跑在 esp_timer 任务上下文，**不得**阻塞、**不得**做重活） */
static void pairing_timeout_cb(void *arg)
{
    (void)arg;
    s_window_active = false;
    if (!s_pairing) {
        return;   /* 已经被按键主动退出 ⇒ 不再重复打印/重复停广播 */
    }
    if (ble_already_connected()) {
        /* 真实存在的一刻：窗口内**已经配网成功**，只是收敛点还没把广播停掉。
         * ⛔ 不许在这里打印"超时退出配网模式"——那会把"其实已经成功"写成"没人来配网"。 */
        ESP_LOGI(TAG, "BLE 配网窗口到期：设备**已入网** ⇒ 保持退出配网模式（停广播；由收敛点统一停）");
    } else {
        ESP_LOGW(TAG, "BLE 配网模式**超时退出**：%d s 内没有设备成功入网 ⇒ 停止广播、回到待机；"
                      "需要配网请再长按 SET 键（每次进入重新计时）",
                 (int)CONFIG_ONEYE_FW_BLE_PROV_PAIRING_WINDOW_S);
    }
    wifi_prov_ble_stop_advertising();
}

static void ensure_window_timer(void)
{
    if (s_window_timer != NULL) {
        return;
    }
    esp_timer_create_args_t ta = {
        .callback = pairing_timeout_cb,
        .arg = NULL,
        .name = "ble_prov_win",   /* ⚠️ esp_timer 名上限 16 字符（含 NUL） */
    };
    esp_err_t rc = esp_timer_create(&ta, &s_window_timer);
    if (rc != ESP_OK) {
        /* ⛔ 不静默：没有计时器就没有超时自动退出（配网模式会一直开着），必须让人看见 */
        ESP_LOGE(TAG, "配网窗口计时器创建失败（%s）⇒ 本次进入**没有超时自动退出**，"
                      "只能再长按 SET 退出", esp_err_to_name(rc));
        s_window_timer = NULL;
    }
}

/* 重开（或首次开）计时：每次进入配网模式都从完整窗口重新计时 */
static void window_restart(void)
{
    ensure_window_timer();
    if (s_window_timer == NULL) {
        return;
    }
    (void)esp_timer_stop(s_window_timer);      /* 未启动时返回 ESP_ERR_INVALID_STATE，忽略 */
    esp_err_t rc = esp_timer_start_once(s_window_timer,
                                       (uint64_t)CONFIG_ONEYE_FW_BLE_PROV_PAIRING_WINDOW_S * 1000000ULL);
    if (rc == ESP_OK) {
        s_window_active = true;
    } else {
        s_window_active = false;
        ESP_LOGE(TAG, "配网窗口计时启动失败（%s）⇒ 本次进入没有超时自动退出", esp_err_to_name(rc));
    }
}

static void window_stop(void)
{
    if (s_window_timer != NULL && s_window_active) {
        (void)esp_timer_stop(s_window_timer);
    }
    s_window_active = false;
}

#endif /* CONFIG_ONEYE_FW_ENABLE_BLE_PROV（配网窗口计时器段） */

esp_err_t wifi_prov_ble_pairing_enter(void)
{
#if !CONFIG_ONEYE_FW_ENABLE_BLE_PROV
    ESP_LOGW(TAG, "BLE 配网通道未启用（CONFIG_ONEYE_FW_ENABLE_BLE_PROV=n）⇒ 长按 SET 进入配网模式不可用");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (ble_already_connected()) {
        /* ⛔ 已入网 ≠ 可再配网：契约（ADR-0017 D2）里入网成功后不再保持可配网态。
         *    需要换网请走明确的"清除凭据/重新配网"路径，而不是靠长按 SET 悄悄重开。 */
        ESP_LOGW(TAG, "长按 SET 被忽略：设备**已入网** ⇒ 不再进入 BLE 配网模式"
                      "（换网请用清除凭据/重新配网路径）");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_advertising) {
        ESP_LOGI(TAG, "配网模式已在广播中（幂等：只重置超时计时）");
        window_restart();
        s_pairing = true;
        return ESP_OK;
    }

    esp_err_t rc = wifi_prov_ble_start();     /* 起栈（若未起）/重开广播；幂等 */
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "进入 BLE 配网模式失败（%s）⇒ 未开始广播", esp_err_to_name(rc));
        return rc;
    }
    s_pairing = true;
    window_restart();
    /* ★ 可观测性（用户 2026-10-09 口径：进入/退出/超时各一行明确日志，⛔ 不含凭据）：
     *   本行是"进入"那一行；⛔ POP 已按用户 2026-10-09 裁定彻底删除 ⇒ 串口**不再打印 POP**，
     *   改为打印**本机型是否需要激活凭证**等非敏感信息（⛔ 不打印激活码 / chal，
     *   ⛔ 也不打印任何新凭据）。
     *   "剩余时间" = 整个窗口（刚进入），后续由超时那一行给出结论。 */
    ESP_LOGI(TAG, "已进入 BLE 配网模式：广播中；本机型档位=%s（依据 %s；%s激活凭证 cred_req=%d）；"
                  "配对凭据 = 激活码（机身贴码）＋一次性 chal（chal TTL %d s）；"
                  "配网窗口剩余 %d s；再长按 SET 退出",
             oneye_dev_ble_device_tier_name((int)dev_onboarding_tier()), dev_onboarding_tier_basis(),
             dev_cred_required() ? "需要" : "不需要", dev_cred_required() ? 1 : 0,
             (int)CONFIG_ONEYE_DEV_SDK_BLE_CHAL_TTL_S,
             (int)CONFIG_ONEYE_FW_BLE_PROV_PAIRING_WINDOW_S);
    return ESP_OK;
#endif
}

esp_err_t wifi_prov_ble_pairing_exit(void)
{
#if !CONFIG_ONEYE_FW_ENABLE_BLE_PROV
    (void)0;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_pairing && !s_advertising) {
        ESP_LOGI(TAG, "长按 SET 被忽略：当前**不在** BLE 配网模式（待机中）⇒ 长按可进入配网模式");
        return ESP_OK;
    }
    const bool was_advertising = s_advertising;
    s_pairing = false;
    window_stop();
    wifi_prov_ble_stop_advertising();
    ESP_LOGI(TAG, "已退出 BLE 配网模式：%s；需要配网请再长按 SET 键",
             was_advertising ? "已停止广播、回到待机"
                             : "本来就不在广播（设备已入网或通道未起）");
    return ESP_OK;
#endif
}

bool wifi_prov_ble_pairing_is_active(void)
{
    return s_started && s_advertising && s_pairing;
}

void wifi_prov_ble_set_autostart(bool autostart)
{
    s_autostart = autostart;
}

/* ------------------------------------------------------------------ 通道启动 */

#if CONFIG_ONEYE_FW_ENABLE_BLE_PROV
/*
 * 机型级「需要激活凭证」⇒ `prov.pair.ok` 的可选字段 `cred_req`（台账 #7 / #9）。
 * 载体 = 本工程 Kconfig `ONEYE_FW_ONBOARDING_ROLE`（三值，缺省**甲**）；
 * ⚠️ 旧载体 `ONEYE_FW_DEV_CRED_REQUIRED`（布尔）**降级为兼容项**（契约 §4.4.4：它判不出乙 / 丙），
 *    本文件只把它当"与档位的一致性核对项"。
 *
 * ⚠️ **必须**写成 `#if … #else …`，⛔ **不得**写成 `CONFIG_X ? a : b`（也不得用 `!CONFIG_X`）：
 *    `sdkconfig.h` 只对 **y 的 bool** 产出 `#define`；置 n 时那个标识符**根本不存在**
 *    ⇒ 写进表达式就是"未声明标识符"（编译失败），而 `!CONFIG_X` 形式会被**静默翻成"需要"**。
 *    该机制已登记在本工程 Kconfig 的帮助文本里（2026-10-09 实测确认）。
 */
static bool dev_cred_required(void)
{
#if CONFIG_ONEYE_FW_DEV_CRED_REQUIRED
    return true;  /* y = 需要（缺省；与 cloud 机型表读不到时的 fail-closed 同向） */
#else
    return false; /* n = 明确"不需要" */
#endif
}

/*
 * 🔴 **设备侧档位（甲 / 乙 / 丙）** —— 契约 §4.4.2 / §4.4.6（四）；载体 = 本工程 Kconfig 的
 *    `ONEYE_FW_ONBOARDING_ROLE` choice（**编译期**，随机型 sdkconfig.defaults 固化）。
 *
 * ⛔ **为什么不由云侧下发**：配网期设备**无网**（拿不到 `resolve` / 机型表），厂商字段里又
 *    没有 `productKey`（台账 #3 已删 `pk_family`）⇒ 设备**无从查表**；且本档位要回答的是
 *    "**设备自己**在没有网的时候怎么处置 `prov.verify.req`"，这是一个**本地面**问题。
 * ⇒ **一致性由谁保证**：**构建期声明（本处）＋ 契约要求二者对同一 `product_key` 必须一致**；
 *    权威值仍在云侧（`dev_product_credential_policy.credential_required` ＋ `onboarding_actor`，
 *    经 `resolve` 的 `onboardingActor` 给 App）—— 设备这一位是**自称**，⛔ 不是授权。
 *    ⚠️ 若将来要改成"云侧下发"，那是**新帧 / 新字段**（属契约变更）⇒ 本批⛔ 不预先发明，
 *       已在任务报告的"未做/待裁定"里登记。
 */
static oneye_dev_ble_device_tier_t dev_onboarding_tier(void)
{
#if CONFIG_ONEYE_FW_ONBOARDING_ROLE_BETA
    return ONEYE_DEV_TIER_B_CERT_DEVICE;   /* 乙：无 creds ＋ 品类级内嵌证书 ＋ 能连云 */
#elif CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA
    return ONEYE_DEV_TIER_C_APP_ONBOARDED; /* 丙：无 creds ＋ 连不上云（入网能力全交 App） */
#else
    return ONEYE_DEV_TIER_A_CREDS_DEVICE;  /* 甲（缺省，最严）：有 creds ＋ 能连云 */
#endif
}

/* 档位 → 可打印短名（⛔ 不含机密；与 SDK 的 `oneye_dev_ble_device_tier_name()` 同源，
 * 这里再包一层只为把"依据"（Kconfig 宏名）一并打印出来）。 */
static const char *dev_onboarding_tier_basis(void)
{
#if CONFIG_ONEYE_FW_ONBOARDING_ROLE_BETA
    return "CONFIG_ONEYE_FW_ONBOARDING_ROLE_BETA";
#elif CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA
    return "CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA";
#else
    return "CONFIG_ONEYE_FW_ONBOARDING_ROLE_ALPHA";
#endif
}

/*
 * 🔴 **两个载体不许打架**（编译期拦，⛔ 不静默选一边）：契约 §4.4.2 的三型映射是**死**的 ——
 *    甲 ⇔ `cred_req=true`；乙 / 丙 ⇔ `cred_req=false`。
 *    ⛔ 不用 `#if A && !B` 之类的"n 时也成立"的写法（见上面 `dev_cred_required()` 的机制说明）。
 */
#if CONFIG_ONEYE_FW_DEV_CRED_REQUIRED
#if CONFIG_ONEYE_FW_ONBOARDING_ROLE_BETA || CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA
#error "机型位自相矛盾：档位选了乙 / 丙（契约要求 cred_req=false），但 ONEYE_FW_DEV_CRED_REQUIRED=y（需要激活凭证）。请二者取一致（见 Kconfig 两个选项的 help）。"
#endif
#else
#if CONFIG_ONEYE_FW_ONBOARDING_ROLE_ALPHA
#error "机型位自相矛盾：档位选了甲（契约要求 cred_req=true），但 ONEYE_FW_DEV_CRED_REQUIRED=n（不需要激活凭证）。请二者取一致（见 Kconfig 两个选项的 help）。"
#endif
#endif
#endif /* CONFIG_ONEYE_FW_ENABLE_BLE_PROV */

esp_err_t wifi_prov_ble_start(void)
{
#if !CONFIG_ONEYE_FW_ENABLE_BLE_PROV
    ESP_LOGW(TAG, "BLE 配网通道未启用（CONFIG_ONEYE_FW_ENABLE_BLE_PROV=n）⇒ 本机型应改用其它通道");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_started) {
        if (!s_advertising && oneye_dev_ble_advertise(true) == ONEYE_DEV_SDK_OK) {
            s_advertising = true;
        }
        return ESP_OK;
    }

    /*
     * 0) 身份：**先读 creds 分区**，一次取出 **node_id（激活码）与 sn（公开）** 两串
     *    （独立 TU，见 adv_identity.c）。顺序上必须先读、取完立刻 free，再起 NimBLE
     *    （避免 16 KiB 与 NimBLE 主机 12 KiB 栈同时在峰）。
     *
     * 🔴 **2026-10-10 订正：本步改为「档位感知」**（契约 §4.4.10（七·一）1.；依据 = 用户逐字
     *    裁定「**乙型丙型在上平台前都无身份**」）：
     *   · **甲**（缺省，最严）＋ 读不到 creds / `node_id` 为空 ⇒ **仍 `ESP_FAIL`、零广播**
     *     （⛔ 成对负向：⛔ 不得把甲一起放宽 —— 甲的 `prov.verify.req` 要拿 `node_id` 逐字比对，
     *      没有它这一档**一道闸都没有**）；
     *   · **乙 / 丙** ＋ 无身份 ⇒ **允许起栈**（`ESP_OK`；⛔ 不 `ESP_FAIL`）：
     *     空口照契约产出 `0x28` ＋ 空 `sn_field` 占位（乙必 / 丙两可，见 §4.4.10（一）/（二）），
     *     `prov.pair` 的判据只剩 `chal`（§4.4.10（三）：配对不判码）⇒ **无 `node_id` 也能配对**；
     *   · **档位未声明 / 档位值非法** ⇒ **按甲** fail-closed（⛔ 不猜档位）。
     * ⛔ **两条硬边界（无论哪一档）**：
     *   ① ⛔ **不回落 `CONFIG_ONEYE_FW_DEVICE_ID`**（第二来源 = 替一台设备声称另一个身份）；
     *   ② 无身份时 `node_id` / `sn` **保持空**（⛔ 不编占位串；空口由 SDK 按档位产出 `0x28` 占位）。
     */
    if (!s_ids_ready) {
        s_ids_status = adv_identity_load_ex(&s_ids, &s_ids_reason);
        s_ids_ready = (s_ids_status == ADV_IDENTITY_OK);
    }
    {
        bool tier_known = (dev_onboarding_tier() == ONEYE_DEV_TIER_A_CREDS_DEVICE ||
                           dev_onboarding_tier() == ONEYE_DEV_TIER_B_CERT_DEVICE ||
                           dev_onboarding_tier() == ONEYE_DEV_TIER_C_APP_ONBOARDED);
        bool tier_is_alpha = (dev_onboarding_tier() == ONEYE_DEV_TIER_A_CREDS_DEVICE) || !tier_known;

        if (!s_ids_ready && tier_is_alpha) {
            /* fail-closed（**仅甲 / 未声明**）：⛔ 不回落 CONFIG_ONEYE_FW_DEVICE_ID（第二来源，
             * 且会让 App 用错派生根 ⇒ 配对必失败）。宁可本通道不可用，也不冒充身份。 */
            ESP_LOGE(TAG, "身份不可用（creds 的 node_id 读不到，原因=%s）＋ 档位=%s（按甲 fail-closed）"
                          "⇒ **BLE 配网通道不启动**：甲的 prov.verify.req 必须拿自身 node_id 逐字比对，"
                          "⛔ 不回落编译期 CONFIG_ONEYE_FW_DEVICE_ID（第二来源）",
                     s_ids_reason == ADV_IDENTITY_ERR_EMPTY ? "creds.node_id 为空" : "creds 读取失败",
                     oneye_dev_ble_device_tier_name((int)dev_onboarding_tier()));
            return ESP_FAIL;
        }
        if (!s_ids_ready) {
            /* 乙 / 丙：**无实例身份**是它们的**正常起点**（§4.4.8（一）T1 / T2）⇒ 起栈、零冒充。 */
            ESP_LOGW(TAG, "身份不可用（原因=%s）＋ 档位=%s ⇒ **允许无实例身份起 BLE 配网栈**："
                          "空口产出 `0x28` ＋ 空 sn_field 占位（乙必 / 丙两可）；配对判据只剩 chal；"
                          "⛔ 不回落编译期 device_id、⛔ 不编占位身份串",
                     s_ids_reason == ADV_IDENTITY_ERR_EMPTY ? "creds.node_id 为空" : "creds 读取失败",
                     oneye_dev_ble_device_tier_name((int)dev_onboarding_tier()));
        } else {
            ESP_LOGI(TAG, "身份已装载：激活码 node_id「%s」（私密：link 节点 id + 甲型核验基准）、"
                          "SN「%s」（公开：广播名/厂商字段/prov.pair.ok；%s）",
                     s_ids.node_id, s_ids.sn,
                     s_ids.sn_fell_back ? "**creds.sn 为空 ⇒ 回落 node_id（存量机）**" : "来源 creds.sn");
        }
    }

    /* 1) link：prov.* 帧的内置处理器在 link 层（oneye_dev_link.c:267-331），必须先于 ble 起来。
     *    本工程只用它的 ble 信道，不启用 lan/wan（那两条是别的通道/别的决策）。
     *    ⚠️ link 层节点 id = **激活码 node_id**（它不是空口值，只是帧内 `from`；
     *       2026-10-09 起 `prov.pair.ok` **不再回它**，见台账 #7）。 */
    if (!oneye_dev_link_is_initialized()) {
        oneye_dev_link_config_t lcfg;
        memset(&lcfg, 0, sizeof(lcfg));
        ONEYE_DEV_STRUCT_INIT(lcfg);
        oneye_dev_link_config_defaults(&lcfg);
        /*
         * ⚠️ 无实例身份（乙 / 丙 的起点）⇒ 传 **NULL**（让 SDK 用其**自己文档化的**缺省节点 id），
         *    ⛔ 不传空串：link 的 `oneye_node_id_valid("")` 为 false ⇒ `oneye_dev_link_init()`
         *    会返回 `ERR_INVALID`，把"无身份起栈"变成"通道不可用"（那是本批要修掉的现象）。
         * ⛔ NULL **不是**回落 `CONFIG_ONEYE_FW_DEVICE_ID`（那才是第二来源 / 冒充）；
         *    这里既没有读任何编译期常量，也没有替设备声称任何身份。
         */
        lcfg.node_id = (s_ids.node_id[0] != '\0') ? s_ids.node_id : NULL;
#if CONFIG_ONEYE_FW_DIAG_PROV_TRACE
        /* 诊断留痕（2026-10-10）：把 SDK 侧 PROV_STATE 事件接到串口。
         * ⛔ 只挂回调、⛔ 不改任何发帧路径与状态序列。 */
        lcfg.event_cb = diag_link_evt_cb;
#endif
        /* 只跑 BLE：不给 wan_uri ⇒ wan 信道不启用；allow_plaintext_chan 保持缺省
         * （本工程没有 lan 信道，明文口径无从触发）。 */
        oneye_dev_sdk_err_t lrc = oneye_dev_link_init(&lcfg);
        if (lrc != ONEYE_DEV_SDK_OK) {
            ESP_LOGE(TAG, "link 初始化失败（%d）⇒ BLE 配网通道不可用", (int)lrc);
            return ESP_FAIL;
        }
        (void)oneye_dev_link_start();
    }

    /* 2) ble：注册 provider（Wi-Fi 动作仍由本工程实现，SDK 不代管 Wi-Fi） */
    oneye_dev_ble_config_t bcfg;
    memset(&bcfg, 0, sizeof(bcfg));
    ONEYE_DEV_STRUCT_INIT(bcfg);
    oneye_dev_ble_config_defaults(&bcfg);
    /* ★ 2026-10-09 核心改动（台账 #1 / #3）：**两串各喂各的** ——
     *   `device_id` = **激活码 node_id**（link 帧 `from` 用；⛔ 2026-10-10 起**不再是** HKDF salt、
     *                 ⛔ 也**不再是** `prov.pair` 的比对基准 —— 只剩**甲型 `prov.verify.req` 的核验基准**）；
     *   `adv_sn`    = **SN**（广播名派生 + 厂商字段 + `prov.pair.ok.sn`）。
     * 🔴 2026-10-10（档位感知）：**无实例身份**时两者传 NULL ——
     *   · `adv_sn = NULL` ＋ 档位乙 / 丙 ⇒ 平台层产出 `0x28` ＋ 空 `sn_field` 占位（契约 §4.4.10（一）/（二））；
     *   · `adv_sn = NULL` ＋ 档位甲 / 未声明 ⇒ 整段 0xFF 不产出（⛔ 不冒充，一字未改）。
     * ⛔ 两者都⛔ **不得**回落 `CONFIG_ONEYE_FW_DEVICE_ID`。 */
    bcfg.device_id = (s_ids.node_id[0] != '\0') ? s_ids.node_id : "";
    bcfg.adv_sn = (s_ids.sn[0] != '\0') ? s_ids.sn : NULL;
    /* 机型级「是否要求激活凭证」⇒ `prov.pair.ok.cred_req`（可选字段，台账 #7 / #9）。
     * 载体 = 本工程 Kconfig（编译期）：配网期无网 + 厂商字段里已无 `pk_family`（台账 #3）
     * ⇒ 设备侧拿不到 cloud 机型表，只能由机型位承载；缺省 y（与 cloud 的 fail-closed 同向）。 */
    bcfg.adv_cred_required = dev_cred_required();
    bcfg.adv_cred_required_set = true;
    /* 🔴 设备侧**档位**（三态的唯一分派依据；契约 §4.4.2 / §4.4.6（四））：
     *   甲逐字比对 / 乙形态校验 / 丙 SKIP。载体 = 同一份 Kconfig choice（编译期），
     *   与上面的 `cred_req` **同批**给出 ⇒ 二者一致（不一致时本文件已在编译期 `#error`）。 */
    bcfg.adv_device_tier = dev_onboarding_tier();
    bcfg.adv_device_tier_set = true;
    bcfg.auto_advertise = false;      /* 起完再显式开始广播，便于把结果讲清楚 */
    bcfg.prov.scan = ble_prov_scan;
    bcfg.prov.connect = ble_prov_connect;
    bcfg.prov.reset = ble_prov_reset;
#if CONFIG_ONEYE_FW_DIAG_PROV_TRACE
    /* 🔴 诊断留痕（2026-10-10 配网诊断批）：**应用确实把三件套交出去了**的第一手证据。
     *   缺了这一行，"provider 没注册" 与 "SDK 收下配置却从不使用" 两件事在串口上不可分辨。
     * ⛔ 只打是否存在（0/1），⛔ 不打任何函数指针值之外的机密（这里本来也没有机密）。 */
    ESP_LOGI(TAG, "[diag] prov 三件套注入 BLE 配置：scan=%d connect=%d reset=%d"
                  "（SDK 在 init()/start() 两处转交 link 层；转交结果由 SDK 侧 'prov provider 转交 link' 行给出）",
             (bcfg.prov.scan != NULL) ? 1 : 0, (bcfg.prov.connect != NULL) ? 1 : 0,
             (bcfg.prov.reset != NULL) ? 1 : 0);
#endif

    oneye_dev_sdk_err_t rc = oneye_dev_ble_init(&bcfg);
    if (rc != ONEYE_DEV_SDK_OK) {
        ESP_LOGE(TAG, "BLE 初始化失败（%d）⇒ 本通道不可用（其余通道不受影响）", (int)rc);
        return ESP_FAIL;
    }
    /* ⛔ 旧代码逐字留痕（⛔ 不抹）—— 固定 POP 注入口（`oneye_dev_ble_set_pop(CONFIG_ONEYE_FW_BLE_POP)`）：
     *   按用户 2026-10-09 裁定「彻底删除实现及契约」整段删除（含 Kconfig 的 `ONEYE_FW_BLE_POP`）。 */
    rc = oneye_dev_ble_start();
    if (rc != ONEYE_DEV_SDK_OK) {
        ESP_LOGE(TAG, "BLE 启动失败（%d）⇒ 本通道不可用", (int)rc);
        return ESP_FAIL;
    }
    /* 3) 绑定为 link 的 ble 信道发送器（链路回帧用；失败只影响 link 帧，不影响配网本身） */
    {
        oneye_dev_sdk_err_t brc = oneye_dev_ble_bind_to_link();

        if (brc != ONEYE_DEV_SDK_OK) {
            ESP_LOGW(TAG, "BLE 绑定 link 信道失败 ⇒ link/组域帧不走 BLE（配网仍可用）");
        }
#if CONFIG_ONEYE_FW_DIAG_PROV_TRACE
        /* 🔴 诊断留痕（2026-10-10 配网诊断批）：`prov.scan.rsp` / `prov.status` 是 **link 层** 组的帧，
         *   它们只能经这条 BLE 发送器出站 ⇒ "provider 跑了但 App 收不到回帧" 时，本行是第一个要看的量。
         * ⛔ 只读返回值，⛔ 不改绑定语义。 */
        ESP_LOGI(TAG, "[diag] link←BLE 发送器绑定 rc=%d（0=成功；link 侧回帧只能经此信道出站）",
                 (int)brc);
#endif
    }
    if (s_autostart) {
        if (oneye_dev_ble_advertise(true) == ONEYE_DEV_SDK_OK) {
            s_advertising = true;
        } else {
            ESP_LOGW(TAG, "BLE 开始广播失败 ⇒ 手机可能扫不到本机");
        }
        /* 开机即广播 = 配网模式已在开（长按 SET 才能"退出"；见 korvo2_oneye_main.c 的按键挂载点） */
        s_pairing = true;
    } else {
        /* ★ 2026-10-09 本批：开机**不广播**（待机）⇒ 由长按 SET 进入配网模式。
         *   ⛔ 这不等于"通道不可用"：NimBLE 已起栈、GATT 已注册，只差 advertise(true)。
         *   ⚠️ 因此这里 s_pairing 保持 false ——「配网模式」的语义是"正在等一台设备来配网"，
         *      待机时它还没开（长按 SET 才开）。 */
        ESP_LOGI(TAG, "BLE 配网通道**待机**：NimBLE 已起栈但**未广播**（本机型配网模式由按键进入）"
                      "⇒ 长按 SET 键进入配网模式");
    }

    s_started = true;
    /* ⚠️ 🔴 **2026-10-10 订正（旧字逐字留痕，⛔ 不抹）**：本段此前逐字写「走到这里 `s_ids_ready` 必为真
     *    （第 0 步 fail-closed 已保证）：故此处不再有"身份缺失"分支」—— ⚠️ 该"必为真"**只对甲成立**：
     *    乙 / 丙 现在**允许** `s_ids_ready == false`（无实例身份起栈）⇒ 就绪行必须按**实际有无身份**讲清。
     *
     * ★ 2026-10-09（台账 #1 / #3 / #7）：就绪行把**两串**讲清楚 ——
     *   · 激活码 `node_id`（私密）只喂：link 节点 id ＋ **甲型** `prov.verify.req` 的逐字核验基准；
     *     ⚠️ 2026-10-10：它**不再是** HKDF `salt`（`salt = chal`）**也不再是**配对阶段的比对基准；
     *   · SN（公开）喂：广播名（GAP 层，SDK 派生 `ONEYE-<SN 后 4 位>`）、厂商字段、`prov.pair.ok.sn`。
     *   ⛔ 不再写"三者同源于 node_id"（那是 2026-10-07 那批的口径，本批已就地作废）。
     *   ⚠️ 广播名的**后缀不在本文件里另算一遍**（那是第二处派生 = 第二事实源）：
     *      空口字节由 SDK 平台层开机打印（`oneye_ble_plat_nimble.c` 的扫描响应逐字节行）。
     * ★ 2026-10-09：状态词按**实际是否在广播**打印（"广播中"/"待机"），
     *   ⛔ 不许在待机时沿用"广播中"——那会让串口说假话（本批第一个要防的形态）。
     * ⛔ 用户 2026-10-09 裁定（彻底删除 POP）：本行**不再读回 / 不再打印 POP**，
     *   改为打印**非敏感**信息（本机型是否需要激活凭证 + chal TTL 档位）。
     *   旧代码逐字留痕（⛔ 不抹）：`if (oneye_dev_ble_get_pop(s_pop, sizeof(s_pop)) ==
     *   ONEYE_DEV_SDK_OK) { … "POP=%s 仅兼容/产测档，TTL %d s" … }`。
     * ⛔ 无身份档位下**不打印** `node_id` / `SN`（它们本来就是空 —— ⛔ 不许编一个值出来充门面）。 */
    ESP_LOGI(TAG, "BLE 配网通道已就绪：%s；身份=%s；"
                  "配对判据 = 一次性 chal（合法＝存在/16 B/未过期/未用过；⛔ 配对阶段**不判 code**，chal TTL %d s）；"
                  "★ 本机档位=%s（依据 %s）⇒ prov.verify.req 处置=%s；%s激活凭证（cred_req=%d）",
             s_advertising ? "广播中" : "待机（未广播，等按键进入配网模式）",
             s_ids_ready
                 ? "已装载（激活码 node_id=**私密**（甲型核验基准）/ SN=公开（广播名+厂商字段+prov.pair.ok））"
                 : "**无实例身份**（乙 / 丙 的起点：空口 = `0x28` ＋ 空 sn_field 占位；上平台后由 cloud 赋身份）",
             (int)CONFIG_ONEYE_DEV_SDK_BLE_CHAL_TTL_S,
             oneye_dev_ble_device_tier_name((int)dev_onboarding_tier()), dev_onboarding_tier_basis(),
             (dev_onboarding_tier() == ONEYE_DEV_TIER_C_APP_ONBOARDED)
                 ? "SKIP（丙：⛔ 不发现场帧）"
                 : ((dev_onboarding_tier() == ONEYE_DEV_TIER_B_CERT_DEVICE)
                        ? "必经：node_id 形态/有效性（⛔ 非身份证明；权威在云侧准入）"
                        : "必经：与自身 creds.node_id 逐字比对"),
             dev_cred_required() ? "需要" : "不需要", dev_cred_required() ? 1 : 0);
    return ESP_OK;
#endif /* CONFIG_ONEYE_FW_ENABLE_BLE_PROV */
}

bool wifi_prov_ble_active(void)
{
    return s_started && s_advertising;
}

/* ⛔ 旧函数逐字留痕（⛔ 不抹）—— `const char *wifi_prov_ble_pop(void)`（返回当前配对 POP，
 *   兼容/产测档读数）：按用户 2026-10-09 裁定「彻底删除实现及契约」整函数删除
 *   （头文件声明同步删除；⛔ 不留不可达死代码）。 */

void wifi_prov_ble_stop_advertising(void)
{
    if (!s_started || !s_advertising) {
        return;
    }
    if (oneye_dev_ble_advertise(false) == ONEYE_DEV_SDK_OK) {
        s_advertising = false;
        /* ★ 2026-10-09：本函数有**三条**上游路径，日志必须按"配网模式的开关还在不在"分岔，
         *   ⛔ 不许写死成"已入网"（那会让②③两条路径在串口上说假话）：
         *     ① 入网成功 —— 唯一收敛点 `on_wifi_ready()`（`korvo2_oneye_main.c:730`）。
         *        ★ 这一条路径在这里**顺带关掉配网模式开关**：既然已经入网，"等配网"这件事就结束了
         *        ⇒ 设备回到「待机」而不是停在"配网模式但没广播"的中间态（否则下一次长按 SET
         *        会去"退出"一个早就结束的配网模式，打印一句与事实不符的日志）。
         *     ② 长按 SET 退出 —— `wifi_prov_ble_pairing_exit()` **先**清 s_pairing，再调本函数。
         *     ③ 配网窗口超时 —— `pairing_timeout_cb()`，此时 s_pairing 仍为真（由上一行的
         *        ESP_LOGW 留痕），随后由本函数停广播；下一次长按 SET 会走 enter（而不是 exit）。
         *   ⚠️ 这里只写一个 bool ⇒ 与 esp_timer 回调（另一任务上下文）之间**不加锁**：
         *      最坏后果是超时那一行日志与"实际连接"擦身而过（多打一行），
         *      ⛔ 不会误停广播、不会改凭据、不会多发起入网 —— 与之相比，为一次日志引入
         *      临界区/任务通知才是更大的复杂度。 */
        if (wifi_prov_is_connected()) {
            s_pairing = false;
            window_stop();
            ESP_LOGI(TAG, "已停止 BLE 广播（设备**已入网** ⇒ 配网模式结束、回到待机；"
                          "需要换网请走清除凭据/重新配网路径）");
        } else if (s_pairing) {
            ESP_LOGI(TAG, "已停止 BLE 广播（配网模式仍开着 ⇒ 可由超时或再长按 SET 退出）");
        } else {
            ESP_LOGI(TAG, "已停止 BLE 广播（配网模式已退出 ⇒ 回到待机）");
        }
    }
}
