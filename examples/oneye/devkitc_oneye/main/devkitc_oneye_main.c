/*
 * devkitc_oneye_main.c —— ESP32-S3-DevKitC-1 上的**最小、零音频依赖** oneye 配网固件
 *
 * ============================================================================
 * ★ 这个工程存在的理由（⛔ 本文件不自证"能跑音频"，它只跑配网 + 绑定）★
 *
 * 验证目标：**乙类(BETA)** 与 **丙类(GAMMA)** 的「配网 + 绑定」闭环。
 *   · 乙：无 creds ⇒ **无实例身份**，但**带厂商字段广播**（App 能筛选发现）、
 *         `prov.verify.req` **必经**、绑定成功后由 **App 重发同一帧**置 NVS `commit=1`；
 *   · 丙：无 creds、**不连云**，设备侧复核 **SKIP（零帧）**、**不落地身份**，
 *         入网/绑定由 **App 代发**。
 *
 * 为什么不用 `examples/oneye/korvo2_oneye`（已登记的结论，⛔ 不重复发现）：
 *   ① 它的 `app_main()` 无条件调 `audio_board_init()`（ADF 板级：ES8311/ES7210/TCA9554…）；
 *   ② `main/board_expect.h` 是 **Korvo-2 板级参数的编译期断言**；
 *   ③ `CONFIG_ONEYE_FW_SELFTEST_STRICT=y`（缺省）时自检失败即 **死等**（`while(1)`）。
 *   ⇒ 三件都在本工程**不存在**：本工程不链接 ADF（见顶层 `CMakeLists.txt`），
 *     不引任何 audio/display/camera 组件（见 `main/CMakeLists.txt` 的 REQUIRES）。
 *
 * ★ 本文件**逐字复用**的"配网件"（从源目录复制，见 `main/CMakeLists.txt`）★
 *   · `wifi_prov_ble.c`  —— BLE 通道 C1/C2：档位感知的身份检查 / 配对 / 厂商字段 /
 *                           配网窗口计时 / `prov.status{connected}` 上报（判据在
 *                           `wifi_prov_status.h`，与主机夹具同源）；
 *   · `wifi_prov.c`      —— Wi-Fi 生命周期 ＋ **入网后收敛点**（唯一 `oneye_start()` 触发边）；
 *   · `adv_identity.c`   —— 设备身份的**唯一**读取点（`creds` 的 node_id / sn 两串）。
 *
 * ⛔ 本文件**不新增**任何帧 / 错误码 / 状态；⛔ 不改 `contracts/local` 下任何文件一个字节。
 * ⛔ 本文件**不**读 SD 卡（DevKitC-1 无卡槽）⇒ 不引 `WIFI_FILE` 通道（见 sdkconfig.defaults）。
 * ============================================================================
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "oneye_dev_sdk.h"
#include "oneye_dev_creds.h"

#include "adv_identity.h"      /* ⛔ 只用于声明核对：身份的**唯一**读取点在 adv_identity.c */
#include "ble_sig_material.h"  /* 乙档 prov.pair.ok.sig 的注入点（窄接口） */
#include "wifi_prov.h"
#include "wifi_prov_ble.h"
#include "wifi_prov_status.h"
/* ★ 2026-10-13〔本批 ③b〕启动期授时（乙档严格 TLS 的"先有钟再建链"）——
 *   与 korvo2_oneye/main/sntp_boot.c **同源复制**（逐字节），接入点见 `on_wifi_ready()`。 */
#include "sntp_boot.h"

static const char *TAG = "devkitc_oneye";

/* ============================================================================
 * 0. 板级常量（ESP32-S3-DevKitC-1 官方板）
 *
 * ★ **零音频依赖之后，"板级"只剩一颗按键与一个 LED** —— 因此这里只有两个常量，
 *   而没有 `board_expect.h` 那样的几十项编译期断言（⛔ 本工程不假装做过板级核对）。
 *
 *   BOOT 键 = **GPIO0**，低有效，板上有外部上拉（原理图：R35 10k 到 3V3）；
 *             它同时是 strapping 脚 ⇒ 上电瞬间的电平会被 ROM 采样（下载模式），
 *             我们**只在启动完成之后**读它（`app_main` 末尾起任务），故不影响下载。
 *   状态 LED = **GPIO2**（板载单色蓝灯，低有效）。⚠️ GPIO2 在 ESP32-S3 上兼作
 *             strapping/启动指示，本工程**只**把"配网窗口开着"这一个状态打上去，
 *             不影响其它功能；⛔ 不据此声称任何板级能力。
 * ============================================================================ */

#define DEVKITC_BOOT_GPIO          GPIO_NUM_0
#define DEVKITC_STATUS_LED_GPIO    GPIO_NUM_2
#define DEVKITC_KEY_HOLD_MS        1000u   /* "长按" = 持续低 ≥ 1 s */
#define DEVKITC_KEY_POLL_MS        50u

/* ============================================================================
 * 1. 诊断读数（`[res]`）—— **逐字沿用** korvo2_oneye 的 `res_report()` 口径
 *
 * 为什么保留阶段标签：多处读数没有标签就分不清"哪一刻的量"。本工程的三处：
 *   · `pre-prov…`    —— app_main 里、配网与 NimBLE **之前**（两种档位同一位置 ⇒ 可比）；
 *   · `ble-ready…`   —— `wifi_prov_boot()` 里 BLE 通道**已起栈、尚未入网**
 *                       （与上一行相减 = NimBLE 主机栈的净代价）；
 *   · `cloud-start…` —— `cloud_start_task()` 里（入网之后、SDK 起来之后）。
 * 关键量是 `largest_internal_block`：任何 ≥16 KB 的新任务栈都要**连续**内部块。
 * ⛔ 不许为此新增常驻任务/定时器（本函数只读 `heap_caps_*`、不阻塞、不分配）。
 * ⛔ 不许去掉 `phase`（那等于把"哪一刻"这个歧义换个形状留下）。
 * ⚠️ 本工程**不开 PSRAM**（DevKitC-1 的模组 PSRAM 情况随批次不同）⇒ `free_psram` 恒 0，
 *    这是**如实读数**，⛔ 不是"PSRAM 用完了"。
 * ============================================================================ */
static void res_report(const char *phase)
{
    ESP_LOGI(TAG, "[res] phase=%s free_internal=%u B / largest_internal_block=%u B / free_psram=%u B",
             phase,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ============================================================================
 * 2. 档位（甲 / 乙 / 丙）的**可读名**（只用于日志；⛔ 判据本体在 wifi_prov_ble.c）
 *
 * ★ 为什么这里再写一次 `#if`：打印"本机档位"时若写错分支，串口就会**说假话**，
 *   而串口日志正是本次验证的第一手证据。⛔ 这里**不**决定任何行为（行为全在
 *   `wifi_prov_ble.c` 的 `dev_onboarding_tier()`，那里是唯一分派点）。
 * ⚠️ `sdkconfig.h` 只对 **y 的 bool** 产出 `#define` ⇒ 必须用 `#if X / #elif / #else`
 *    （⛔ 不写 `CONFIG_X ? a : b`，也不写 `!CONFIG_X`：后者在 n 时被静默翻成 1）。
 * ============================================================================ */
static const char *fw_tier_name(void)
{
#if CONFIG_ONEYE_FW_ONBOARDING_ROLE_BETA
    return "乙 BETA";
#elif CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA
    return "丙 GAMMA";
#else
    return "甲 ALPHA";
#endif
}

static const char *fw_tier_basis(void)
{
#if CONFIG_ONEYE_FW_ONBOARDING_ROLE_BETA
    return "CONFIG_ONEYE_FW_ONBOARDING_ROLE_BETA";
#elif CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA
    return "CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA";
#else
    return "CONFIG_ONEYE_FW_ONBOARDING_ROLE_ALPHA";
#endif
}

/* ============================================================================
 * 3. 资源 / 档位 / 身份的自证行（开机与心跳共用一份判据 ⇒ ⛔ 不两处各写一遍）
 * ============================================================================ */
static void boot_identity_report(void)
{
    esp_err_t nvs;
    adv_identity_t ids;
    adv_identity_status_t why = ADV_IDENTITY_ERR_CREDS;
    adv_identity_status_t st;

    memset(&ids, 0, sizeof(ids));
    st = adv_identity_load_ex(&ids, &why);

    ESP_LOGI(TAG, "==== devkitc_oneye 启动（ESP32-S3-DevKitC-1 · 零音频依赖）====");
    ESP_LOGI(TAG, "固件 %s / oneye-dev-sdk %s", CONFIG_ONEYE_FW_VERSION, oneye_dev_sdk_version());
    ESP_LOGI(TAG, "★ 本机档位 = %s（依据 %s）", fw_tier_name(), fw_tier_basis());
    ESP_LOGI(TAG, "厂商标识 product_key = %s（机型属性；⛔ 不是身份）", CONFIG_ONEYE_FW_PRODUCT_KEY);

    /* 身份：**唯一**读取点是 adv_identity.c；本行只是把它的结论原样讲清。
     * ⚠️ 乙 / 丙 的 `ADV_IDENTITY_ERR_CREDS` / `_EMPTY` **不是故障** ——
     *    契约 §4.4.8（一）的 T1 / T2 时点，它们本来就**没有实例身份**。
     * ⛔ 本工程**不**回落 `CONFIG_ONEYE_FW_DEVICE_ID` 当身份（第二来源 = 冒充）。 */
    if (st == ADV_IDENTITY_OK) {
        ESP_LOGI(TAG, "身份：creds 分区可用（node_id=**私密**（甲型核验基准）；SN=公开"
                      "（广播名/厂商字段/prov.pair.ok）；sn_fell_back=%d）",
                 ids.sn_fell_back ? 1 : 0);
        ESP_LOGW(TAG, "⚠️ 本构建是乙 / 丙 档，但 creds 分区里**有**身份 —— 这会让乙 / 丙 的"
                      "「无实例身份」前提不成立（请确认这是台面桩件，而不是量产形态）");
    } else {
        ESP_LOGW(TAG, "身份：**无实例身份**（%s）—— 对乙 / 丙 这是**正常起点**"
                      "（契约 §4.4.8（一）：上平台前无身份）；空口按档位产出占位形态，"
                      "配对判据只剩 chal；启动日志里的节点名只是**占位**",
                 (why == ADV_IDENTITY_ERR_EMPTY) ? "creds.node_id 为空" : "creds 读取失败");
    }

    /* NVS：契约 §4.4.10（十）2. 的两档 `nvs` / `ram-only` 的前提就是"NVS 能不能用"。
     * ⚠️ 本工程**只读**探测并如实打印；真正的档位判定与写路径在 SDK 内部
     *   （`oneye_ble_bindid_*`，2026-10-10 新增），⛔ 本文件不替它表态。 */
    nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        nvs = nvs_flash_init();
    }
    if (nvs == ESP_OK) {
        ESP_LOGI(TAG, "NVS：可用（partition=nvs）⇒ 乙档的「绑定期身份记录」写路径的媒介前提成立"
                      "（写失败时 SDK 按契约自动降 `ram-only`，⛔ 不阻断流程）");
    } else {
        ESP_LOGW(TAG, "NVS：不可用（%s）⇒ 乙档的持久化按契约落 `ram-only` 档"
                      "（**本会话有效、上电即失效**，是**合法配置**，⛔ 不是故障）",
                 esp_err_to_name(nvs));
    }

    /* 乙档的签名材料自证（⛔ 只打"有没有"，⛔ 不打材料内容）。 */
#if CONFIG_ONEYE_FW_ONBOARDING_ROLE_BETA
    ESP_LOGI(TAG, "乙档 sig 材料：%s（prov.pair.ok 的 `sig` = 内嵌证书对 chal 的签名）",
             devkitc_ble_sig_material_present() ? "已注入" : "**未注入**（本构建没嵌入证书材料）");
#else
    ESP_LOGI(TAG, "丙档：⛔ 不产 `prov.pair.ok.sig`（契约 §4.4.2：丙的 sig 不承载任何证明力）");
#endif
}

/* ============================================================================
 * 4. `prov.status{state=connected, ip=…}` 上报（契约 §4.1 的**既有**状态）
 *
 * ★ 判据本体在 `wifi_prov_status.h`（固件与主机夹具**编译同一份源**，⛔ 不在这里抄一份）；
 *   本函数的职责只有"什么时候问一次判据、以及失败不置位"。
 * ⛔ 非阻塞：只发**一帧**，走 SDK **既有**发送路径（`wifi_prov_ble_report_prov_connected()`
 *   → `oneye_dev_link_prov_report_status()`），天然经过它的 TX 串行化锁。
 * ⛔ 失败不置位 ⇒ 下一次 GOT_IP 会再试；⛔ 不假装成功（App 会一直等 connected）。
 * ============================================================================ */

/* ★ 2026-10-13〔本批 ③a · 从 korvo2_oneye 同步 e00aa9c9〕**幂等位提到文件作用域**（不是函数内静态）
 *   —— 因为它的**复位**来自 `wifi_prov.c` 的"会话结束"通告（见下面 `prov_status_reset_after_disconnect()`）。
 *   ① 置位：只有**真的发出**（`wifi_prov_ble_report_prov_connected()` 返回 `ESP_OK`）才置位；
 *   ② 复位：Wi-Fi 断开（**这一次配网会话结束**）⇒ 下一段会话拿到 IP 可以再报一次。
 *   ⛔ 判据本体（`wifi_prov_status.h`）一个字都没改：本批只把"复位状态"这件事接上。 */
static bool reported;

/* ★ 2026-10-13〔本批 ③a〕**会话结束 ⇒ 复位"已报过 connected"这一位**
 *   （判据本体 = `wifi_prov_status.h` 的 ④ `wifi_prov_connected_reported_after_disconnect()`）。
 *
 * 真机证据在 korvo2_oneye 侧（⛔ 不重复采集，2026-10-12 两轮）：第一轮 GOT_IP 报过一次 connected，
 *   第二轮（App 走 `[prov-switch]` 断旧连新 → GOT_IP、provider 返回 OK）之后**再也没有报**
 *   ⇒ App 一直停在"配网中"直到超时。成因 = 幂等位**跨会话不复位**（旧形态：函数内静态、只置位、无人复位）。
 *
 * 为什么复位边 = **Wi-Fi 断开**（"这一次配网是新的一次"的语义点）：
 *   · 这一位回答的是"**这一段连接会话**里报过没有"；断开之后这一段的 `connected` 已不成立
 *     （`wifi_prov_ip()` 清空、`wifi_prov_is_connected()` 转 false）⇒ 位必须随之作废；
 *   · 三条重新配网路径**都物理经过断开边**（App/权威通道的断旧连新、`prov.reset`、
 *     重新配网后再 GOT_IP）⇒ **一个复位边覆盖三者**，⛔ 不必在各处各写一遍；
 *   · ⛔ 不会退化成"每次都报"：复位只发生在**真实断开**时；同一段连接里的重复 GOT_IP
 *     （DHCP 续租等）**不经过**这条边 ⇒ "本会话只报一次"原样成立。
 *   ⛔ 不许把它挪进 `prov_status_report_connected_once()`（那等于每次 GOT_IP 都复位 ⇒ 每次都报）。 */
static void prov_status_reset_after_disconnect(void)
{
    if (reported) {
        ESP_LOGW(TAG, "[diag] Wi-Fi 会话结束 ⇒ 复位 connected 幂等位"
                      "（这一次配网到此为止；下一次 GOT_IP 可以再报一次）");
    }
    reported = wifi_prov_connected_reported_after_disconnect(reported);
}

static void prov_status_report_connected_once(void)
{
    const char *ip = wifi_prov_ip();
    esp_err_t rc;

    if (!wifi_prov_connected_should_report(wifi_prov_is_connected(), ip, reported)) {
        /* 没拿到 IP，或**本次会话**已经报过（多为同一段连接里的重复 GOT_IP）⇒ 什么都不做，⛔ 不刷屏 */
        ESP_LOGI(TAG, "[diag] connected 上报判据=否：wifi_connected=%d ip=%s already_reported=%d"
                      "（前者为 0 ⇒ 还没拿到 IP，如实停在「配网中」）",
                 wifi_prov_is_connected() ? 1 : 0, (ip != NULL) ? ip : "(空)", reported ? 1 : 0);
        return;
    }
    rc = wifi_prov_ble_report_prov_connected(wifi_prov_ssid(), ip, wifi_prov_source());
    reported = wifi_prov_connected_reported_after(reported, rc == ESP_OK);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "prov.status{state=connected, ip=%s} **未发出**（%s）⇒ 幂等位未置，"
                      "下一次 GOT_IP 会再试（⛔ 不把『未发出』写成『已上报』）",
                 ip, esp_err_to_name(rc));
        return;
    }
    ESP_LOGI(TAG, "★ prov.status{state=connected, ip=%s, ssid=%s, source=%s} 已发出"
                  "（本次配网会话内只报一次；Wi-Fi 断开后复位 ⇒ 重新配网可再报一次）",
             ip, wifi_prov_ssid(), wifi_prov_source());
}

/* ============================================================================
 * 5. 上云（`oneye_start()`）—— 唯一调用点，只在 `cloud_start_task` 内
 *
 * ★ 「入网 ⇒ 上云」的**唯一收敛点**（ADR-0017 D2）：
 *   `wifi_prov.c` 的 `IP_EVENT_STA_GOT_IP` → ready 回调 → 本文件的 `on_wifi_ready()`
 *   → `xTaskCreate(cloud_start_task)` → `oneye_start()`。
 *   配网通道侧**不**知道这里做什么，⛔ 也不许自己调 `oneye_start()`。
 *
 * ★ 本函数与 korvo2_oneye 的差异（**逐条都在注释里**，⛔ 不静默省略）：
 *   ① ⛔ 无 `board_selftest()` / `board_init_peripherals()` / `keys_start()` /
 *      `panel_start_if_enabled()` / `aec` / `player` / `camera` —— 零音频/显示/相机依赖；
 *   ② ⛔ 无 track 埋点 / 影子补发 / log 取证 三条产出方的常驻维护循环
 *      （它们是 korvo2 的取证设施，与"配网+绑定闭环"无关；本工程不引入，也不冒充有）；
 *   ③ ✅ 保留资源读数（`[res]`）与 NVS 探测 —— 它们是"两档 `nvs`/`ram-only`"的前提证据；
 *   ④ ✅ 保留 `creds` 分区优先的凭据装载语义（含"坏了不回退"的 fail-closed）。
 * ============================================================================ */
/* ★ 承载选择：丙档下 `oneye_start()` 被 `#if` 掏空 ⇒ 本函数**没有调用点**。
 *  `__attribute__((unused))` 是**如实**的标注（"本构建可能用不到它"），
 *  ⛔ 不是把告警"堵掉"：真正需要拦的是**判据被写错**，而那由上面的 `#if` 承担。 */
static oneye_dev_transport_t fw_transport(void) __attribute__((unused));
static oneye_dev_transport_t fw_transport(void)
{
#if CONFIG_ONEYE_FW_TRANSPORT_WS
    return ONEYE_DEV_TRANSPORT_MQTT_WS;
#elif CONFIG_ONEYE_FW_TRANSPORT_TLS
    return ONEYE_DEV_TRANSPORT_MQTT_TLS;
#elif CONFIG_ONEYE_FW_TRANSPORT_WSS
    return ONEYE_DEV_TRANSPORT_MQTT_WSS;
#else
    return ONEYE_DEV_TRANSPORT_MQTT_TCP;
#endif
}

/* ★ 上云本体：丙档下**整段被掏空**（丙按契约不建云链路；见 `on_wifi_ready()` 的分派）。
 *  ⚠️ 本函数**保留调用点**（`cloud_start_task`），理由：丙**仍然**要执行"身份装载 + 档位播报"，
 *     而"丙不连云"这件事**只在函数体里**由 `#if` 分派 —— 把它做成"整个函数不编"会让
 *     "丙的设备侧一道闸都没有"（§4.4.3 缺口 1）在**日志上也看不见**，那与本工程的验证目的相反。
 *  ⇒ ⛔ 不要用 `#if !GAMMA` 把整个函数包起来（已实测：那样会引入 `unterminated #if`，
 *     而且会让丙档的启动自证行消失）。 */
static void oneye_start(void) __attribute__((unused));
static void oneye_start(void)
{
#if CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA
    /* 丙（GAMMA）：**⛔ 不建云链路**（契约 §4.4.2 丙列：入网/绑定由 **App 代发**）。
     * ⚠️ 这里**不读** creds（丙的设备侧不参与身份）、**不碰** base_cfg —— 只播报一行，
     *    把"没调 oneye_start 的下游"讲清楚（⛔ 不是"忘了建链"）。 */
    ESP_LOGW(TAG, "丙（GAMMA）档：**不建立云链路**（设备侧 prov.verify.req = SKIP、⛔ 不落地身份）；"
                  "入网与绑定由 **App 代发** `POST /api/v1/bindings` —— 本档不调用 base_init/base_start");
    return;
#else
    oneye_dev_base_config_t base_cfg;
    oneye_dev_log_config_t log_cfg;
    oneye_dev_event_config_t ev_cfg;
    oneye_dev_caps_t caps;
    oneye_dev_sdk_err_t rc;

    /* 静态身份缓冲：creds 里那两串要活到 base 生命周期结束（SDK **借用**指针）。 */
    static oneye_dev_creds_t s_creds;
    oneye_dev_creds_source_t cred_src = ONEYE_DEV_CREDS_SOURCE_UNKNOWN;

    ESP_LOGI(TAG, "oneye-dev-sdk %s（base/mpp/event/log）", oneye_dev_sdk_version());

    memset(&base_cfg, 0, sizeof(base_cfg));
    ONEYE_DEV_STRUCT_INIT(base_cfg);
    oneye_dev_base_config_defaults(&base_cfg);

    /* ---- 身份（1/2）：`creds` 分区优先（量产口径：通用固件 + 产线写一次凭证）----
     * 三种情形（契约 backend/contracts/domain/设备凭据分区与产测自证契约.md §2，失败姿态冻结）：
     *   · 有合法镜像 ⇒ 用分区里的 node_id / 证书 / 私钥 / CA；
     *   · **坏了**（CORRUPT/IO）⇒ **一律不联网**，且⛔ 不回退（回退 = 拿公用身份冒充这台设备）；
     *   · **没写过**（NOT_FOUND）⇒ **乙 / 丙 的正常起点**：没有实例身份。
     *     量产固件用 `ONEYE_DEV_CREDS_REQUIRED=y` 把它也变成"不联网"；
     *     ⚠️ 本工程乙 / 丙 两档取 `n`（见 sdkconfig.defaults / Kconfig.provbuild 的说明）。 */
    rc = oneye_dev_creds_load(&s_creds);
    if (rc == ONEYE_DEV_SDK_OK) {
        if (oneye_dev_creds_apply(&base_cfg, &s_creds) != ONEYE_DEV_SDK_OK) {
            ESP_LOGE(TAG, "凭据写入 base 配置失败（缺私钥 / 身份不合法）—— **不联网**");
            return;
        }
        cred_src = ONEYE_DEV_CREDS_SOURCE_PARTITION;
        ESP_LOGI(TAG, "凭据来源：**creds 分区** node=%s（cert %u B / key %u B / CA %u B）",
                 s_creds.node_id, (unsigned)s_creds.cert_len, (unsigned)s_creds.key_len,
                 (unsigned)s_creds.ca_len);
    } else if (!oneye_dev_creds_is_absent(rc)) {
        ESP_LOGE(TAG, "凭据不可用：%s ⇒ **不联网**（分区坏了却回退内嵌证书 = 拿公用凭据冒充这台设备）",
                 oneye_dev_sdk_strerror(rc));
        return;
    } else {
        /* ---- 身份（2/2）：没写过分区 ----
         * 🔴 **乙 / 丙 的档位感知分派（本工程的核心差异，逐条对照契约）**：
         *   · **乙（BETA）** 的物理事实是「**能连云** ＋ **有品类级内嵌证书**」
         *     （用户逐字：「乙型有内嵌证书只是作为设备自身入网准入前提」）
         *     ⇒ 有证书材料 ⇒ 走内嵌证书连平台（**入网准入前提**）；
         *   · **丙（GAMMA）** 的物理事实是「**连不上云**（入网能力全交 App）」
         *     ⇒ **⛔ 不连平台**（连了反而会让"丙"这件事在空口上说不清）；
         *   · **甲（ALPHA）** 必须**有** creds 分区；没有 ⇒ 不联网（fail-closed）。
         * ⛔ 三档都**不**回落编译期的 `CONFIG_ONEYE_FW_DEVICE_ID` 当身份（第二来源 = 冒充）。 */
#if CONFIG_ONEYE_FW_ONBOARDING_ROLE_BETA
        {
            if (strlen(CONFIG_ONEYE_FW_DEVICE_ID) == 0u) {
                ESP_LOGE(TAG, "乙档：节点标识为空 ⇒ 不联网（⛔ 不用任何编译期常量冒充身份）");
                return;
            }
            /* 节点标识 = **本机自称**的台面占位（⛔ 不是实例身份）。
             * 契约 §4.4.10（三）：甲 / 乙 的 `prov.verify.req` 必经，且**乙的判据 = 形态/有效性**
             * ⇒ 乙在拿到 App 交来的 `node_id` 之前就已有"自己该用哪个节点名"的问题；
             * 本工程的处理 = 用编译期占位名连平台（**并如实登记**：这不是"身份已赋予"）。 */
            base_cfg.device_id = CONFIG_ONEYE_FW_DEVICE_ID;
            base_cfg.username  = CONFIG_ONEYE_FW_DEVICE_ID;
            base_cfg.client_id = CONFIG_ONEYE_FW_DEVICE_ID;
#if defined(ONEYE_FW_EMBED_CERTS)
            /* 内嵌证书 = **品类级凭据 / 入网准入前提**（⛔ 不是一机一密、⛔ 不是身份）。
             * `main/CMakeLists.txt` 用 objcopy 把三件 PEM 嵌成 `_binary_*` blob，末尾带一个 NUL，
             * 这里裁掉，避免把 NUL 交给 mbedtls 解析。 */
            {
                extern const uint8_t oneye_cert_ca_start[]  asm("_binary_ca_crt_start");
                extern const uint8_t oneye_cert_ca_end[]    asm("_binary_ca_crt_end");
                extern const uint8_t oneye_cert_crt_start[] asm("_binary_client_crt_start");
                extern const uint8_t oneye_cert_crt_end[]   asm("_binary_client_crt_end");
                extern const uint8_t oneye_cert_key_start[] asm("_binary_client_key_start");
                extern const uint8_t oneye_cert_key_end[]   asm("_binary_client_key_end");
                size_t ca_len  = (size_t)(oneye_cert_ca_end  - oneye_cert_ca_start);
                size_t crt_len = (size_t)(oneye_cert_crt_end - oneye_cert_crt_start);
                size_t key_len = (size_t)(oneye_cert_key_end - oneye_cert_key_start);

                if (ca_len > 0u && oneye_cert_ca_start[ca_len - 1u] == '\0')   { ca_len--; }
                if (crt_len > 0u && oneye_cert_crt_start[crt_len - 1u] == '\0') { crt_len--; }
                if (key_len > 0u && oneye_cert_key_start[key_len - 1u] == '\0') { key_len--; }

                base_cfg.credential         = oneye_cert_crt_start;
                base_cfg.credential_len     = crt_len;
                base_cfg.credential_key     = oneye_cert_key_start;
                base_cfg.credential_key_len = key_len;
                base_cfg.tls_ca_pem         = oneye_cert_ca_start;
                base_cfg.tls_ca_pem_len     = ca_len;
                cred_src = ONEYE_DEV_CREDS_SOURCE_EMBEDDED;
                ESP_LOGI(TAG, "乙档入网准入前提：已嵌入**品类级**证书（client %u B / key %u B / CA %u B）"
                              "—— ⛔ 非一机一密、⛔ 非身份；身份在**绑定之后**由 App↔cloud 核验赋予",
                         (unsigned)crt_len, (unsigned)key_len, (unsigned)ca_len);
            }
#else
            /* 契约要求乙「有内嵌证书」⇒ 缺件时**不假装具备**：不联网并把缺口讲清。 */
            ESP_LOGE(TAG, "乙档**缺内嵌证书材料**：本构建没有 `-DONEYE_FW_CERT_DIR=<非空目录>` "
                          "⇒ 品类级入网准入前提不成立 ⇒ **不联网**（⛔ 不用无证书形态冒充乙）");
            return;
#endif /* ONEYE_FW_EMBED_CERTS */
        }
#elif CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA
        /* 丙：**⛔ 不连云**（契约 §4.4.2 丙列：入网能力全交 App；设备侧不参与上平台）。
         * 丙的"配网"在这里就结束了：BLE 通道由 wifi_prov_boot() 起（或由按键进入），
         * Wi-Fi 入网（若 App 反向下发凭据）之后**只**上报 prov.status，⛔ 不建云链路。 */
        ESP_LOGW(TAG, "丙（GAMMA）档：**不建立云链路**（契约 §4.4.2 丙列：入网/绑定由 **App 代发**；"
                      "设备侧 prov.verify.req = SKIP、⛔ 不落地身份）⇒ 本次不调用 base_init/start");
        return;
#else
        ESP_LOGE(TAG, "甲（ALPHA）档要求 creds 分区里有本机身份，而本机**没写过** ⇒ **不联网**"
                      "（契约 §4.4.2 甲列：配对凭据的基准就是 creds.node_id）");
        return;
#endif
    }

    base_cfg.cloud_host = CONFIG_ONEYE_FW_CLOUD_HOST;
    base_cfg.cloud_port = (uint16_t)CONFIG_ONEYE_FW_CLOUD_PORT;
    base_cfg.cloud_ws_path = (CONFIG_ONEYE_FW_CLOUD_WS_PATH[0] != '\0') ? CONFIG_ONEYE_FW_CLOUD_WS_PATH : NULL;
    base_cfg.transport = fw_transport();
    base_cfg.token = (CONFIG_ONEYE_FW_CLOUD_TOKEN[0] != '\0') ? CONFIG_ONEYE_FW_CLOUD_TOKEN : NULL;
#if CONFIG_ONEYE_FW_TLS_INSECURE
    base_cfg.tls_insecure_skip_verify = true;    /* 仅 dev/产测；⛔ 生产必须 n 并投放 CA */
#else
    base_cfg.tls_insecure_skip_verify = false;
#endif
    base_cfg.enable_status_topic = true;   /* status/up（retained + 遗嘱） */

    rc = oneye_dev_base_init(&base_cfg);
    if (rc != ONEYE_DEV_SDK_OK) {
        ESP_LOGE(TAG, "base init 失败：%s", oneye_dev_sdk_strerror(rc));
        return;
    }

    /* 能力集：**严格口径**（默认不声明任何能力位）。
     * ⛔ 本工程无音频/视频/显示 ⇒ caps = NONE 是**如实声明**，⛔ 不是"还没打开开关"。 */
    memset(&caps, 0, sizeof(caps));
    ONEYE_DEV_STRUCT_INIT(caps);
    caps.caps = (uint32_t)ONEYE_DEV_CAP_NONE;
    caps.model_version = "1";
    caps.fw_version = CONFIG_ONEYE_FW_VERSION;
    (void)oneye_dev_base_set_caps(&caps);

    /* log（自检结论走这里，不新造通道） */
    memset(&log_cfg, 0, sizeof(log_cfg));
    ONEYE_DEV_STRUCT_INIT(log_cfg);
    log_cfg.level = ONEYE_DEV_LOG_LEVEL_INFO;
    log_cfg.uplink_enabled = true;
    (void)oneye_dev_log_init(&log_cfg);

    /* event */
    memset(&ev_cfg, 0, sizeof(ev_cfg));
    ONEYE_DEV_STRUCT_INIT(ev_cfg);
    ev_cfg.enabled = true;
    ev_cfg.track_enabled = false;   /* ⛔ 本工程不做埋点取证（korvo2 的那三条产出方不在此） */
    ev_cfg.offline_bytes = 16 * 1024;
    (void)oneye_dev_event_init(&ev_cfg);

    /* 链路 */
    rc = oneye_dev_base_start();
    if (rc != ONEYE_DEV_SDK_OK) {
        ESP_LOGE(TAG, "base start 失败：%s", oneye_dev_sdk_strerror(rc));
        return;
    }
    (void)oneye_dev_base_wait_event(3000);

    /* 自检结论 + 凭据来源上报（影子 `reported` 的 `credSource`：⛔ "看不清有没有回退"不可以）。
     * ⚠️ 本工程**没有**不透明回退路径：乙用内嵌证书（`embedded`）、丙不连网、
     *    甲必须有分区。因此这一项在乙档 = `embedded`，是**如实**值。 */
    {
        char shadow[192];
        char src_json[64];

        if (oneye_dev_creds_state_json(cred_src, src_json, sizeof(src_json)) == ONEYE_DEV_SDK_OK) {
            snprintf(shadow, sizeof(shadow), "{\"firmwareVersion\":\"%s\",\"power\":true,\"%s\":%s}",
                     CONFIG_ONEYE_FW_VERSION, ONEYE_DEV_CREDS_SHADOW_KEY,
                     (src_json[0] == '{') ? src_json + 1 : "\"unknown\"}");
            /* 上面这行把 `{"credSource":"x"}` 的**内层**并进本对象：去掉开头的 `{`
             * 之后，结尾那个 `}` 正好当本对象的收尾 —— ⛔ 不再拼第二个 `}`（那会产出非法 JSON，
             * 而 `report_state()` 会以 INVALID 静默失败，串口上看起来一切正常）。 */
            rc = oneye_dev_base_report_state(shadow);
            if (rc == ONEYE_DEV_SDK_OK) {
                ESP_LOGI(TAG, "影子上报成功（shadow/up）：%s", shadow);
            } else {
                ESP_LOGW(TAG, "影子上报失败（%s）—— 平台侧将看不到 %s",
                         oneye_dev_sdk_strerror(rc), ONEYE_DEV_CREDS_SHADOW_KEY);
            }
        } else {
            ESP_LOGW(TAG, "凭据来源未知，不上报 %s（⛔ 报一个含糊值比不报更坏）",
                     ONEYE_DEV_CREDS_SHADOW_KEY);
        }
    }

    ONEYE_LOGI(ONEYE_DEV_LOG_TAG_BASE,
               "devkitc_oneye 就绪：档位 %s；固件 %s；⛔ 零音频依赖（无板级自检项）",
               fw_tier_name(), CONFIG_ONEYE_FW_VERSION);
#endif /* CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA（oneye_start 的档位分派） */
}

/* ★ 上云任务：**仅乙（与甲）需要**。丙（GAMMA）按契约**不建云链路**（§4.4.2 丙列），
 * 它连 `oneye_start()` 都不会被调用 ⇒ 本函数**不编进丙档固件**（否则 `-Werror=unused-function`
 * 会把"丙不连云"这条判据编成构建失败 —— 那是**正确的**告警，别用 `(void)` 去堵它）。 */
#if !CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA
static void cloud_start_task(void *arg)
{
    (void)arg;
    oneye_start();
    res_report("cloud-start（入网后）");
    /* ⚠️ 本任务**常驻**（不 `vTaskDelete`）：`oneye_start` 之后没有周期活要干，
     *    留一个 1 s 的静默 tick 只为让"任务还在"这件事可被 `[res]` 之外的读数佐证。
     *    ⛔ 本工程**不**在此起 track/影子/log 三条维护循环（korvo2 的取证设施，不属本次验证面）。 */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif /* !CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA */

/* ============================================================================
 * 6. 入网后收敛点（**唯一**触发边；所有通道共用）
 * ============================================================================ */
static void on_wifi_ready(void)
{
    static bool started;

    wifi_prov_ble_stop_advertising();     /* 已入网 ⇒ 停 BLE 广播（幂等，重连时重复调无害） */

    /* 上报 `prov.status{connected, ip}`：**每次** GOT_IP 都要考虑（幂等位在函数内）。
     * ⚠️ 位置**故意在** 下面那次一次性守卫之前：本行是事件级动作，与"上云链路只启动一次"
     *    那把守卫**不是**同一件事（否则内存不足时 App 会"明明联网却收不到 connected"）。 */
    prov_status_report_connected_once();

    if (started) {
        return;
    }
    started = true;

    ESP_LOGI(TAG, "Wi-Fi 已连接：ip=%s（ssid=%s，来源 %s）",
             wifi_prov_ip(), wifi_prov_ssid(), wifi_prov_source());

#if CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA
    /* 丙：**⛔ 不建云链路** —— 收敛点下游对丙**只有**上面那条 prov.status 与这一行。
     * ⚠️ 这与契约 §4.4.2 丙列一致（入网/绑定全交 App）；⛔ 不是"忘了调 oneye_start"。 */
    ESP_LOGW(TAG, "丙（GAMMA）档：Wi-Fi 已入网，但按契约**不建云链路**（⛔ 不调用 oneye_start）；"
                  "绑定由 **App 代发** `POST /api/v1/bindings`（设备侧不参与）");
    return;
#else
    /* ★ 2026-10-13〔本批 ③b〕**启动期授时**：乙档是**严格 TLS**（`CONFIG_ONEYE_FW_TLS_INSECURE`
     *   在台面档为 y、量产为 n），而板子**没有 RTC** ⇒ 未授时时系统时间是 1970、服务端证书的
     *   notBefore 落在未来 ⇒ mbedtls 必然报 `BADCERT_FUTURE`。故"**先有钟、再建链**"是硬顺序：
     *   本行必须在下面 `xTaskCreate*`（→ `oneye_start()`）**之前**（与 korvo2_oneye 的
     *   `on_wifi_ready()` 同一口径；那边的门禁 A11/A12 断言这条顺序）。
     *   ⚠️ 本调用在**系统事件任务**上下文，是有界的同步等待：最多 2×10 s（两台服务器各 10 s；
     *   真机正常路径实测 ≈1 s）。失败**不阻塞**上云（见 `sntp_boot.h:9-11`：那是**故意**的可见失败）。
     *   ⛔ 丙（GAMMA）档不调用它：丙按契约**不建云链路**（设备侧零 TLS）⇒ 不需要钟。 */
    (void)sntp_boot_sync();

    if (xTaskCreate(cloud_start_task, "cloud_start", 6144, NULL, 4, NULL) != pdPASS) {
        started = false;
        ESP_LOGE(TAG, "上云任务创建失败（内存不足）—— 设备已联网但不会上平台");
        return;
    }
    ESP_LOGI(TAG, "已联网 → 启动上云（oneye-dev-sdk）");
#endif
}

/* ============================================================================
 * 7. 配网启动（本机型**只有** BLE 一条通道 ⇒ ⛔ 无 SD 卡 / 无凭据文件分支）
 *
 * ★ 与 korvo2_oneye 的差异：那里是「C3 SD 凭据文件（rank 0）→ C1/C2 BLE（rank 1）
 *   → C5 Kconfig 兜底（rank 2）」三段；本工程**只有 BLE**（DevKitC-1 无卡槽），
 *   连 `CONFIG_ONEYE_FW_ENABLE_WIFI_FILE` 这个符号都**不定义** ⇒ C3 那一段编不进来。
 *   ⛔ 这不是"少做了一步"，是**机型事实**：无卡槽机型按 ADR-0017 D3 就该 WIFI_FILE=n。
 * ⛔ 本函数**不做任何「入网后」动作**：那些全在 `on_wifi_ready()` 的收敛点下游。
 * ============================================================================ */
static void wifi_prov_boot(void)
{
#if CONFIG_ONEYE_FW_ENABLE_BLE_PROV
#  ifdef CONFIG_ONEYE_FW_BLE_PROV_KEY_GATE
    const bool key_gate = true;
#  else
    /* ⚠️ `sdkconfig.h` 只对 **y 的 bool** 产出 `#define`；置 n 时那个标识符**根本不存在**
     * ⇒ `!CONFIG_X` 会被预处理成 `!0` = 1（**编得过，但语义被静默翻成 y**）。
     * 这里显式判 `#ifdef`：宏缺失 ⇒ key_gate=false ⇒ "配置不明时不外扩"。 */
    const bool key_gate = false;
#  endif

    wifi_prov_ble_set_autostart(!key_gate);
    ESP_LOGI(TAG, "启动 BLE 配网通道 C1/C2（手机 App 或另一台嵌入式设备经 BLE 下发 SSID/密码）：%s",
             key_gate ? "按键门控模式 —— 开机不广播（待机），长按 BOOT(GPIO0) 进入配网模式"
                      : "开机即广播（长按 BOOT(GPIO0) 可退出 / 重新进入配网模式）");

    if (wifi_prov_ble_start() == ESP_OK) {
        if (key_gate) {
            ESP_LOGW(TAG, "下一步：**长按 BOOT 键**进入 BLE 配网模式"
                          "（串口会打印本机档位与剩余时间；再长按一次退出，%d s 未配网成功自动退出）",
                     (int)CONFIG_ONEYE_FW_BLE_PROV_PAIRING_WINDOW_S);
        } else {
            ESP_LOGW(TAG, "下一步：手机 App 扫描并连接 **ONEYE-<广播名后缀>**，"
                          "配对凭据 = **一次性 chal**（⛔ 配对阶段不判 code，契约 §4.4.10（三））；"
                          "本机档位 %s ⇒ %s",
                     fw_tier_name(),
#if CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA
                     "prov.verify.req **SKIP**（⛔ 一帧不发）、⛔ 不落地身份、绑定由 App 代发"
#else
                     "prov.verify.req **必经**（判据 = node_id 的形态 / 有效性；权威在云侧准入）"
#endif
                     );
        }
        /* 空口字节由 SDK 平台层开机打印（厂商字段逐字节行）⇒ ⛔ 本文件不重算一遍
         * （第二处派生 = 第二事实源）。这里只给"起栈后、入网前"的资源读数。 */
        res_report("ble-ready（NimBLE 已起栈；gate=y 时未广播、入网前）");
        return;   /* 等通道提交凭据；入网后由收敛点统一触发上云 */
    }
    ESP_LOGE(TAG, "BLE 配网通道启动失败 ⇒ 本次启动不会上云（本机型**没有**第二条配网通道）");
#else
    ESP_LOGE(TAG, "本构建未启用 BLE 配网通道（CONFIG_ONEYE_FW_ENABLE_BLE_PROV=n）"
                  "且无 SD 卡通道 ⇒ **一条配网通道都没有**（属配置错误，请修正机型 defaults）");
#endif
}

/* ============================================================================
 * 8. 按键：长按 BOOT(GPIO0) 进入 / 退出配网窗口
 *
 * ★ 与 korvo2_oneye 的差异（⛔ 必须讲清）：korvo2 的按键是**板载 6 键 ADC 梯形**
 *   （ADF 的 `input_key_service`，共用 GPIO5），本工程**没有**那颗 ADC —— 用的是
 *   DevKitC-1 上的 **BOOT 键（GPIO0，低有效、有外部上拉）**，直接走 IDF 的 `driver/gpio`。
 *   ⛔ 本工程不链接 `input_key_service` / `esp_peripherals`（那是 ADF 面）。
 *
 * ★ 门控语义（逐字沿用 `wifi_prov_ble.h` / Kconfig 的口径）：
 *   `wifi_prov_ble_set_autostart(false)`（= `ONEYE_FW_BLE_PROV_KEY_GATE=y`）时开机**不广播**，
 *   长按进入配网模式；`autostart=true`（本工程缺省）时开机即广播，长按用于**退出 / 重新进入**。
 *   ⇒ 本任务**两种情况都要挂**（否则 gate=y 的构建会"没人能进配网模式"）。
 * ============================================================================ */
static void key_window_toggle(void)
{
    if (wifi_prov_ble_pairing_is_active()) {
        (void)wifi_prov_ble_pairing_exit();
    } else {
        (void)wifi_prov_ble_pairing_enter();
    }
}

static void key_gate_task(void *arg)
{
    (void)arg;
    bool pressed = false;
    int64_t t0 = 0;

    for (;;) {
        /* 低有效：0 = 按下。上拉由板级外部电阻 + 本处内部上拉**双保险**（IO0 是 strapping 脚，
         * 悬空会让读数抖动 ⇒ 内部上拉是必要的，不是冗余）。 */
        const bool down = (gpio_get_level(DEVKITC_BOOT_GPIO) == 0);
        const int64_t now = esp_timer_get_time();

        if (down && !pressed) {
            pressed = true;
            t0 = now;
        } else if (!down && pressed) {
            pressed = false;
        } else if (down && pressed && (now - t0) >= (int64_t)DEVKITC_KEY_HOLD_MS * 1000) {
            /* 长按落地一次；把 t0 往后推，避免一直按住时反复触发。 */
            pressed = false;
            ESP_LOGI(TAG, "检测到长按 BOOT(GPIO0) ≥ %u ms", (unsigned)DEVKITC_KEY_HOLD_MS);
            key_window_toggle();
        }
        vTaskDelay(pdMS_TO_TICKS(DEVKITC_KEY_POLL_MS));
    }
}

static void key_gate_start(void)
{
    gpio_config_t io = { 0 };

    io.pin_bit_mask = (1ULL << (int)DEVKITC_BOOT_GPIO);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGW(TAG, "BOOT 键（GPIO%d）配置失败 ⇒ 长按进入配网模式不可用"
                      "（开机即广播的形态不受影响）", (int)DEVKITC_BOOT_GPIO);
        return;
    }
    if (xTaskCreate(key_gate_task, "key_gate", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "按键任务创建失败（内存不足）⇒ 长按进入配网模式不可用");
        return;
    }
    ESP_LOGI(TAG, "按键就绪：长按 **BOOT(GPIO0)** ≥ %u ms 切换 BLE 配网模式（进入 / 退出）",
             (unsigned)DEVKITC_KEY_HOLD_MS);
}

/* ============================================================================
 * 9. 心跳（仅诊断构建；口径沿用 korvo2_oneye 的 `diag` 批）
 *
 * 为什么要心跳：串口"只有一行行启动日志、之后什么都没有"时，无法区分
 *   「日志级别把 INFO 滤掉了」与「抓取窗口在开机之后才开始」。心跳每 5 s 一行 ⇒ 歧义消失：
 *   · 有心跳 ⇒ 串口是活的、级别确实到了 INFO（本行就是一条 INFO）；
 *   · 没心跳 ⇒ 是抓取 / 级别问题，而不是设备行为。
 * ⛔ 只读状态：⛔ 不发帧、⛔ 不改任何判据、⛔ 不打任何机密（密码 / 激活码 / chal / 令牌）。
 * ============================================================================ */
#if CONFIG_ONEYE_FW_DIAG_PROV_TRACE
static void diag_heartbeat_cb(void *arg)
{
    static unsigned tick;

    (void)arg;
    tick++;
    ESP_LOGI(TAG, "[diag] 心跳 #%u：uptime=%lld s 档位=%s 运行期级别(*)=%d "
                  "wifi_connected=%d ip=%s ssid=%s 来源=%s 配网模式=%d 广播中=%d",
             tick, (long long)(esp_timer_get_time() / 1000000), fw_tier_name(),
             (int)esp_log_level_get("*"),
             wifi_prov_is_connected() ? 1 : 0, wifi_prov_ip(), wifi_prov_ssid(), wifi_prov_source(),
             wifi_prov_ble_pairing_is_active() ? 1 : 0, wifi_prov_ble_active() ? 1 : 0);
    wifi_prov_ble_diag_dump();
    res_report("diag-heartbeat");
}

static void diag_boot_report(void)
{
    esp_timer_handle_t hb = NULL;
    esp_timer_create_args_t ta = { 0 };

    ESP_LOGI(TAG, "[diag] 日志级别自证：运行期 esp_log_level_get(\"*\")=%d；编译期"
                  " CONFIG_LOG_DEFAULT_LEVEL=%d / CONFIG_LOG_MAXIMUM_LEVEL=%d"
                  "（OFF=0 E=1 W=2 I=3 D=4 V=5；**上限 < 4 时 ESP_LOGD 根本不编进来**）",
             (int)esp_log_level_get("*"), (int)CONFIG_LOG_DEFAULT_LEVEL,
             (int)CONFIG_LOG_MAXIMUM_LEVEL);
    ESP_LOGI(TAG, "[diag] 本构建 = 诊断版（CONFIG_ONEYE_FW_DIAG_PROV_TRACE=y）：每 5 s 一条心跳；"
                  "⛔ 两档固件的交付口径都不含本开关（见 sdkconfig.defaults.diag）");
    /* 把配网链路上的 tag 放宽到 DEBUG（⛔ 只放宽这些，⛔ 不动全局级别：
     * 否则 BT / Wi-Fi 的 DEBUG 会把串口淹掉，反而看不到配网行）。 */
    esp_log_level_set("wifi_prov", ESP_LOG_DEBUG);
    esp_log_level_set("wifi_prov_ble", ESP_LOG_DEBUG);
    esp_log_level_set("oneye_ble", ESP_LOG_DEBUG);
    esp_log_level_set("oneye_link", ESP_LOG_DEBUG);
    esp_log_level_set("oneye", ESP_LOG_DEBUG);

    ta.callback = diag_heartbeat_cb;
    ta.name = "diag_hb";      /* ⚠️ esp_timer 名上限 16 字符（含 NUL） */
    if (esp_timer_create(&ta, &hb) == ESP_OK) {
        (void)esp_timer_start_periodic(hb, 5000000ULL);
    } else {
        ESP_LOGW(TAG, "[diag] 心跳计时器创建失败 ⇒ 本次没有周期心跳（不影响配网）");
    }
}
#endif /* CONFIG_ONEYE_FW_DIAG_PROV_TRACE */

/* ============================================================================
 * 10. 乙档的**编译期成对断言**（⛔ 缺一则"乙 = 有内嵌证书"这条判据在构建期不成立）
 * ============================================================================ */
#if defined(CONFIG_ONEYE_FW_ONBOARDING_ROLE_BETA) && !defined(ONEYE_FW_EMBED_CERTS)
#error "乙（BETA）档要求「品类级内嵌证书」作为设备自身入网准入前提（契约 §4.4.8（〇）），但本构建没有嵌入证书材料。请用 -DONEYE_FW_CERT_DIR=<含 ca.crt/client.crt/client.key 的目录>。"
#endif

/* ============================================================================
 * 11. app_main
 * ============================================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "devkitc_oneye 启动：ESP32-S3-DevKitC-1 · 零音频依赖（无 codec / 无 LCD / "
                  "无摄像头 / 无 SD 卡；IDF 5.5 / 不引 ADF）");

    boot_identity_report();      /* 档位 / 身份 / NVS / 签名材料 自证（含 nvs_flash_init） */

#if CONFIG_ONEYE_FW_DIAG_PROV_TRACE
    diag_boot_report();
#endif

    /* 配网/NimBLE **之前**的基线读数（两种档位在同一位置打 ⇒ 跨档可比）。 */
    res_report("pre-prov（配网与 NimBLE 之前；本机型无外设可初始化）");

    key_gate_start();            /* 长按 BOOT(GPIO0) 切换配网模式 */

    wifi_prov_set_ready_cb(on_wifi_ready);   /* **唯一**收敛点注册点 */
    /* ★ 2026-10-13〔本批 ③a · 同步 korvo2 e00aa9c9〕**会话结束边**（Wi-Fi 断开）⇒ 复位
     *   `prov.status{connected}` 的幂等位：注册点与 ready 回调**同一处**（应用生命周期的唯一注册点）。
     *   ⛔ 不复位就报不出第二轮 connected ⇒ App 卡在"配网中"（korvo2 真机 2026-10-12 第二轮）；
     *   置位/复位语义见上面 `prov_status_reset_after_disconnect()`。 */
    wifi_prov_set_disconnected_cb(prov_status_reset_after_disconnect);
    wifi_prov_boot();                        /* 起 BLE 通道（或按键盘控待机） */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
