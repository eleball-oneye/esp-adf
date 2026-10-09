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

#include "oneye_dev_ble.h"
#include "oneye_dev_link.h"

#include "adv_identity.h"
#include "wifi_prov.h"
#include "wifi_prov_ble.h"

static const char *TAG = "wifi_prov_ble";

static bool s_started;
static bool s_advertising;
static char s_pop[ONEYE_DEV_BLE_POP_LEN];

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
 *       ② 配对 HKDF **salt**（`bcfg.device_id`；契约 §4.3：`salt = node_id`，⛔ 不做大小写折叠）；
 *       ③ `prov.pair.code` 的**逐字比对基准**（台账 #8：设备本地比对、fail-fast）。
 *   · **`sn` = SN**（**公开**：贴机身、**可广播**）
 *       ① **广播名** = `ONEYE-<SN 后 4 位>`（派生函数与调用点 = SDK
 *          `components/oneye-dev-sdk/src/oneye_dev_ble.c` 的 `oneye_dev_ble_init()`：
 *          `oneye_snprintf(s_ble.adv_name, …, "ONEYE-%s", (nm_len > 4u) ? (nm + nm_len - 4u) : nm)`；
 *          本文件把 `bcfg.adv_name` 留空 ⇒ 走自动派生）；⚠️ 名字只在 **GAP 层**（0x2A00），
 *          ⛔ 不进广播数据、⛔ 不进扫描响应（台账 #3）；
 *       ② **厂商字段 `sn_field(24)`**（`bcfg.adv_sn`，布局见 SDK 头「广播厂商自定义字段」段）；
 *       ③ **`prov.pair.ok.sn`**（⛔ 该帧**不再回 `node_id`**，台账 #7）。
 *   ⛔ **不许留第二个来源**：本文件内**不再**出现 `CONFIG_ONEYE_FW_DEVICE_ID` 作为设备身份
 *      （旧值逐字留痕：上一版 `lcfg.node_id = CONFIG_ONEYE_FW_DEVICE_ID;`（:119）与
 *      `bcfg.device_id = CONFIG_ONEYE_FW_DEVICE_ID;`（:135））。
 *   ⛔ **读不到 ⇒ 本通道不启动**（fail-closed，见 `wifi_prov_ble_start()` 第 0 步）：
 *      回落编译期常量等于"替一台设备声称另一个身份"，且会让 App 算出的密钥与设备不一致。
 *   ★ **`creds.sn` 为空 ⇒ 回落 `node_id`**（存量机；`s_ids.sn_fell_back`）—— 行为与 2026-10-07 那批一致，
 *      只是厂商字段会如实标注 `0x04`（老世代）。
 * ⚠️ 读取在**独立 TU**（`adv_identity.c`）里做：`oneye_dev_creds.h` 与 `oneye_dev_link.h`
 *    在同一个 TU 里会撞 `oneye_dev_link_status_t`（SDK 既有缺陷，见该文件头）。顺序上**先读、
 *    取两串、立刻 free，再起 NimBLE**（避免 16 KiB 与 NimBLE 主机 12 KiB 栈同时在峰）。
 */
static adv_identity_t s_ids;
static bool s_ids_ready;

/* ------------------------------------------------------------------ provider（应用侧 Wi-Fi 动作） */

/* 前置声明：provider 里的 `ble_prov_reset()`（清凭据 ⇒ 重新进入可配网态）与 enter/exit 共用配网窗口
 * 计时器，而它们的定义在下方「配网模式」段 —— C 的单遍扫描要求此处先声明。 */
static void window_restart(void);
static void window_stop(void);

/* 扫描：直接转发给 wifi_prov.c 的实现（Wi-Fi 生命周期归它管，本模块不碰 esp_wifi_*） */
static int ble_prov_scan(char *json_out, size_t cap, void *ctx)
{
    (void)ctx;
    return wifi_prov_scan_json(json_out, cap) == ESP_OK ? 0 : -1;
}

/*
 * 连接：**只**把凭据交给唯一入网入口，然后返回。
 * ⛔ 这里不调 oneye_start()、不建任务、不碰面板/授时/影子/埋点 —— 那些都在收敛点的下游。
 * ⚠️ 本函数在 NimBLE 主机任务上下文（`ble_on_rx → oneye_dev_link_inject_frame → link_handle_prov`
 *    → 本 provider，见 `oneye_dev_sdk/src/oneye_dev_ble.c:202-207` 与 `oneye_dev_link.c:289-320`）
 *    里被**同步**调用，因此会阻塞该任务至多 `wifi_prov_connect` 的超时（缺省 20 s）。
 *    这与既有例程 `korvo2_llm_chat/main/prov_service.c:280-287` 的口径一致；
 *    不另起任务的原因见本工程 `korvo2_oneye_main.c` 关于内部 RAM（`largest_internal_block`）的实测注释。
 * ★ 被优先级判据拒绝时（`ESP_ERR_INVALID_STATE`：槽里已有 SD 卡凭据，或设备已入网 ⇒ 只入网一次）
 *   本函数返回 **-1**，立刻返回、**不阻塞**；SDK 据此向对端报 `PROV_FAILED`
 *   （`oneye_dev_link.c:317-320`）⇒ 手机侧能立刻知道"这一次没被采信"，而不是静默超时。
 */
static int ble_prov_connect(const char *ssid, const char *password, const char *token, void *ctx)
{
    (void)token;    /* 契约 §4 的 rvd 自定义数据：korvo2_oneye 暂不使用（不臆造语义） */
    (void)ctx;
    ESP_LOGI(TAG, "BLE 通道收到凭据：ssid=%s（密码不打印）⇒ 交给唯一入网入口（含优先级判据）", ssid);
    esp_err_t rc = wifi_prov_submit_credentials(ssid, password, WIFI_PROV_CHAN_BLE);
    if (rc == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "本次 BLE 凭据**未被采信**（优先级判据拒绝；串口 [prov-priority] 有留痕）"
                      "⇒ 向对端报失败，不发起入网");
    }
    return (rc == ESP_OK) ? 0 : -1;
}

/* 清除凭据：只断开并清 Wi-Fi 配置，然后重新广播（不重启、不擦 NVS）。
 * ★ 2026-10-09：它把通道**重新开成可配网态** ⇒ 按配网模式口径处理：置 s_pairing 并重启超时窗口，
 *   ⛔ 否则会出现"在广播但不在配网模式"的第三种状态（超时回调与退出逻辑都判不准）。 */
static int ble_prov_reset(void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "BLE 通道请求清除凭据 ⇒ 断开并回到未配网态（重新广播）");
    wifi_prov_forget();
    if (s_started) {
        (void)oneye_dev_ble_unpair();
        if (oneye_dev_ble_advertise(true) == ONEYE_DEV_SDK_OK) {
            s_advertising = true;
        }
        s_pairing = true;
        window_restart();
    }
    return 0;
}

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
     *   本行是"进入"那一行；POP 只在串口，⛔ 不进任何上行帧。
     *   "剩余时间" = 整个窗口（刚进入），后续由超时那一行给出结论。 */
    ESP_LOGI(TAG, "已进入 BLE 配网模式：广播中，POP=%s（有效期 %d s），"
                  "配网窗口剩余 %d s；再长按 SET 退出",
             (s_pop[0] != '\0') ? s_pop : "(未读出，见上方就绪日志)",
             (int)CONFIG_ONEYE_DEV_SDK_BLE_POP_TTL_S,
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
 * 载体 = 本工程 Kconfig `ONEYE_FW_DEV_CRED_REQUIRED`（缺省 y），理由逐条写在 Kconfig 同名项里。
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

    /* 0) 身份（本批的唯一来源）：**先读 creds 分区**，一次取出 **node_id（激活码）与 sn（公开）**
     *    两串（独立 TU，见 adv_identity.c）。顺序上必须先读、取完立刻 free，再起 NimBLE
     *    （避免 16 KiB 与 NimBLE 主机 12 KiB 栈同时在峰）。 */
    if (!s_ids_ready) {
        s_ids_ready = adv_identity_load(&s_ids);
    }
    if (!s_ids_ready) {
        /* fail-closed：⛔ 不回落 CONFIG_ONEYE_FW_DEVICE_ID（那会造成身份的第二个来源，
         * 且 App 会用错 salt ⇒ 配对必失败）。宁可本通道不可用，也不冒充身份。 */
        ESP_LOGE(TAG, "身份不可用（creds 分区的 node_id 读不到）⇒ **BLE 配网通道不启动**："
                      "HKDF salt 与 prov.pair.code 的比对基准都必须来自真实 node_id，"
                      "⛔ 不回落编译期 CONFIG_ONEYE_FW_DEVICE_ID（第二来源）");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "身份已装载：激活码 node_id「%s」（私密：salt + code 比对基准）、"
                  "SN「%s」（公开：广播名/厂商字段/prov.pair.ok；%s）",
             s_ids.node_id, s_ids.sn,
             s_ids.sn_fell_back ? "**creds.sn 为空 ⇒ 回落 node_id（存量机）**" : "来源 creds.sn");

    /* 1) link：prov.* 帧的内置处理器在 link 层（oneye_dev_link.c:267-331），必须先于 ble 起来。
     *    本工程只用它的 ble 信道，不启用 lan/wan（那两条是别的通道/别的决策）。
     *    ⚠️ link 层节点 id = **激活码 node_id**（它不是空口值，只是帧内 `from`；
     *       2026-10-09 起 `prov.pair.ok` **不再回它**，见台账 #7）。 */
    if (!oneye_dev_link_is_initialized()) {
        oneye_dev_link_config_t lcfg;
        memset(&lcfg, 0, sizeof(lcfg));
        ONEYE_DEV_STRUCT_INIT(lcfg);
        oneye_dev_link_config_defaults(&lcfg);
        lcfg.node_id = s_ids.node_id;
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
     *   `device_id` = **激活码 node_id**（HKDF salt + `prov.pair.code` 比对基准）；
     *   `adv_sn`    = **SN**（广播名派生 + 厂商字段 + `prov.pair.ok.sn`）。 */
    bcfg.device_id = s_ids.node_id;
    bcfg.adv_sn = s_ids.sn;
    /* 机型级「是否要求激活凭证」⇒ `prov.pair.ok.cred_req`（可选字段，台账 #7 / #9）。
     * 载体 = 本工程 Kconfig（编译期）：配网期无网 + 厂商字段里已无 `pk_family`（台账 #3）
     * ⇒ 设备侧拿不到 cloud 机型表，只能由机型位承载；缺省 y（与 cloud 的 fail-closed 同向）。 */
    bcfg.adv_cred_required = dev_cred_required();
    bcfg.adv_cred_required_set = true;
    bcfg.auto_advertise = false;      /* 起完再显式开始广播，便于把结果讲清楚 */
    bcfg.prov.scan = ble_prov_scan;
    bcfg.prov.connect = ble_prov_connect;
    bcfg.prov.reset = ble_prov_reset;

    oneye_dev_sdk_err_t rc = oneye_dev_ble_init(&bcfg);
    if (rc != ONEYE_DEV_SDK_OK) {
        ESP_LOGE(TAG, "BLE 初始化失败（%d）⇒ 本通道不可用（其余通道不受影响）", (int)rc);
        return ESP_FAIL;
    }
    if (CONFIG_ONEYE_FW_BLE_POP[0] != '\0') {
        /* 固定 POP（产测/台面）；不是 6 位数字时 SDK 会拒，这里退回随机 POP 而不是硬失败 */
        if (oneye_dev_ble_set_pop(CONFIG_ONEYE_FW_BLE_POP) != ONEYE_DEV_SDK_OK) {
            ESP_LOGW(TAG, "固定 POP「%s」被拒（须 6 位数字）⇒ 改用 SDK 随机 POP",
                     CONFIG_ONEYE_FW_BLE_POP);
        }
    }
    rc = oneye_dev_ble_start();
    if (rc != ONEYE_DEV_SDK_OK) {
        ESP_LOGE(TAG, "BLE 启动失败（%d）⇒ 本通道不可用", (int)rc);
        return ESP_FAIL;
    }
    /* 3) 绑定为 link 的 ble 信道发送器（链路回帧用；失败只影响 link 帧，不影响配网本身） */
    if (oneye_dev_ble_bind_to_link() != ONEYE_DEV_SDK_OK) {
        ESP_LOGW(TAG, "BLE 绑定 link 信道失败 ⇒ link/组域帧不走 BLE（配网仍可用）");
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
    s_pop[0] = '\0';
    /* ⚠️ 走到这里 `s_ids_ready` 必为真（第 0 步 fail-closed 已保证）：故此处不再有"身份缺失"分支，
     *    ⛔ 该分支不是被放宽，而是**不可达**了 —— 读不到 creds 时本函数在上面就返回 ESP_FAIL。 */
    if (oneye_dev_ble_get_pop(s_pop, sizeof(s_pop)) == ONEYE_DEV_SDK_OK) {
        /* ★ 2026-10-09（台账 #1 / #3 / #7）：就绪行必须把**两串**讲清楚 ——
         *   · 激活码 `node_id`（私密）只喂：link 节点 id + HKDF salt + `prov.pair.code` 比对基准；
         *   · SN（公开）喂：广播名（GAP 层，SDK 派生 `ONEYE-<SN 后 4 位>`）、厂商字段、`prov.pair.ok.sn`。
         *   ⛔ 不再写"三者同源于 node_id"（那是 2026-10-07 那批的口径，本批已就地作废）。
         *   ⚠️ 广播名的**后缀不在本文件里另算一遍**（那是第二处派生 = 第二事实源）：
         *      空口字节由 SDK 平台层开机打印（`oneye_ble_plat_nimble.c` 的扫描响应逐字节行）。
         * ★ 2026-10-09：状态词按**实际是否在广播**打印（"广播中"/"待机"），
         *   ⛔ 不许在待机时沿用"广播中"——那会让串口说假话（本批第一个要防的形态）。 */
        ESP_LOGI(TAG, "BLE 配网通道已就绪：%s；激活码 node_id=%s（私密：salt+code 比对基准）；"
                      "SN=%s（公开：广播名 `ONEYE-<后 4 位>`/厂商字段(24 B)/prov.pair.ok；%s）；"
                      "配对凭据 = 激活码＋一次性 chal（POP=%s 仅兼容/产测档，TTL %d s）；cred_req=%d",
                 s_advertising ? "广播中" : "待机（未广播，等按键进入配网模式）",
                 s_ids.node_id, s_ids.sn,
                 s_ids.sn_fell_back ? "creds.sn 为空⇒回落 node_id（SDK 将标 0x04 老世代）" : "来源 creds.sn",
                 s_pop, CONFIG_ONEYE_DEV_SDK_BLE_POP_TTL_S,
                 dev_cred_required() ? 1 : 0);
    } else {
        s_pop[0] = '\0';
        ESP_LOGI(TAG, "BLE 配网通道已就绪：%s（POP 读回失败，串口另有 SDK 日志）；"
                      "激活码 node_id=%s（私密）；SN=%s（公开；%s）",
                 s_advertising ? "广播中" : "待机（未广播，等按键进入配网模式）",
                 s_ids.node_id, s_ids.sn,
                 s_ids.sn_fell_back ? "creds.sn 为空⇒回落 node_id" : "来源 creds.sn");
    }
    return ESP_OK;
#endif /* CONFIG_ONEYE_FW_ENABLE_BLE_PROV */
}

bool wifi_prov_ble_active(void)
{
    return s_started && s_advertising;
}

const char *wifi_prov_ble_pop(void)
{
    return s_pop;
}

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
