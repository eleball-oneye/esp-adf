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

#include "oneye_dev_ble.h"
#include "oneye_dev_link.h"

#include "adv_identity.h"
#include "wifi_prov.h"
#include "wifi_prov_ble.h"

static const char *TAG = "wifi_prov_ble";

static bool s_started;
static bool s_advertising;
static char s_pop[ONEYE_DEV_BLE_POP_LEN];

/*
 * 设备身份 = **creds 分区的 node_id**（用户 2026-10-07 裁定：配对码用"能体现设备唯一性、出产贴码
 * 后不会变动"的标识符；本机 = `KORVO2-0000`，与 creds 里 `node_id`/`sn` 同值）。
 *
 * ★ 2026-10-07（本批，用户逐字裁定「广播名来源**改为由 node_id 派生**」）★
 *   这一份 node_id 现在是 BLE 配网路径上**唯一的身份来源**，同时喂给三个消费点：
 *     ① **广播名** = `ONEYE-<node_id 后 4 位>`（派生函数与调用点 = SDK
 *        `components/oneye-dev-sdk/src/oneye_dev_ble.c:471-474`，
 *        `oneye_snprintf(s_ble.adv_name, …, "ONEYE-%s", (dev_len > 4u) ? (dev + dev_len - 4u) : dev)`；
 *        本文件把 `bcfg.adv_name` 留空 ⇒ 走自动派生）；
 *     ② **配对 HKDF salt** = 同一个 `device_id`（`oneye_dev_ble.c:246`
 *        `oneye_ble_pair_derive_key(s_ble.pop, s_ble.device_id, s_ble.key)`，
 *        HKDF-SHA256(ikm=POP ASCII, salt=device_id UTF-8, info="oneye-link-v1", 16 B)，
 *        契约 `contracts/local/ble-gatt.md §4.3`）；
 *     ③ **BLE link 层节点 id**（`lcfg.node_id`，`oneye_dev_link.c:184`）—— 与 ①② 同一个串，
 *        否则设备会以两个身份对 App 说话（`prov.pair.ok` 的 `node_id` 来自 ②，而 link 帧 `from`
 *        来自 ③）。
 *   ⛔ **不许留第二个来源**：本文件内**不再**出现 `CONFIG_ONEYE_FW_DEVICE_ID` 作为设备身份
 *      （旧值逐字留痕：上一版 `lcfg.node_id = CONFIG_ONEYE_FW_DEVICE_ID;`（:119）与
 *      `bcfg.device_id = CONFIG_ONEYE_FW_DEVICE_ID;`（:135）—— 它们与厂商字段的 `sn`（creds node_id）
 *      **不同源**，导致 App 的「广播名后 4 位 == sn 后 4 位」自洽性校验**拒绝真机**
 *      （`OneyeDiscoveryPredicateTest.realMachineNameSuffixVsNodeIdTailIsRejected`）。
 *   ⛔ **读不到 ⇒ 本通道不启动**（fail-closed，见 `wifi_prov_ble_start()` 第 0 步）：
 *      回落编译期常量等于"替一台设备声称另一个身份"，且会让 App 算出的密钥与设备不一致。
 *   ⛔ 厂商字段（AD 0xFF）的 `sn` 仍走同一条读取路径（`s_adv_sn`）：读到 ⇒ 广播；读不到 ⇒
 *      **整段 0xFF 不产出**（见 `oneye_dev_ble_adv_mfg_encode` 的红线），⛔ 不冒充。
 * ⚠️ 读取在**独立 TU**（`adv_identity.c`）里做：`oneye_dev_creds.h` 与 `oneye_dev_link.h`
 *    在同一个 TU 里会撞 `oneye_dev_link_status_t`（SDK 既有缺陷，见该文件头）。顺序上**先读、
 *    取 node_id、立刻 free，再起 NimBLE**（避免 16 KiB 与 NimBLE 主机 12 KiB 栈同时在峰）。
 */
#define ADV_SN_BUF 72u /* node_id 契约上界 64 + 余量 */
static char s_adv_sn[ADV_SN_BUF];
static bool s_adv_sn_ready;

/* ------------------------------------------------------------------ provider（应用侧 Wi-Fi 动作） */

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

/* 清除凭据：只断开并清 Wi-Fi 配置，然后重新广播（不重启、不擦 NVS） */
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
    }
    return 0;
}

/* ------------------------------------------------------------------ 通道启动 */

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

    /* 0) 身份（本批的唯一来源）：**先读 creds 分区**，取 node_id（独立 TU，见 adv_identity.c）。
     *    顺序上必须先读、取完立刻 free，再起 NimBLE（避免 16 KiB 与 NimBLE 主机 12 KiB 栈同时在峰）。 */
    if (!s_adv_sn_ready) {
        s_adv_sn_ready = adv_identity_sn(s_adv_sn, sizeof(s_adv_sn), NULL);
    }
    if (!s_adv_sn_ready) {
        /* fail-closed：⛔ 不回落 CONFIG_ONEYE_FW_DEVICE_ID（那会造成"广播名与 salt 有两个来源"，
         * 且 App 会用错 salt ⇒ 配对必失败）。宁可本通道不可用，也不冒充身份。 */
        ESP_LOGE(TAG, "身份不可用（creds 分区的 node_id 读不到）⇒ **BLE 配网通道不启动**："
                      "广播名与配对 salt 都必须由真实 node_id 派生，"
                      "⛔ 不回落编译期 CONFIG_ONEYE_FW_DEVICE_ID（第二来源）");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "身份来源 = creds 分区 node_id「%s」⇒ 广播名 ONEYE-<后 4 位> 与 HKDF salt 同源于此",
             s_adv_sn);

    /* 1) link：prov.* 帧的内置处理器在 link 层（oneye_dev_link.c:267-331），必须先于 ble 起来。
     *    本工程只用它的 ble 信道，不启用 lan/wan（那两条是别的通道/别的决策）。
     *    ⚠️ node_id **必须**与 BLE 层的 device_id 同串：`prov.pair.ok.node_id` 与设备身份只应有一个。 */
    if (!oneye_dev_link_is_initialized()) {
        oneye_dev_link_config_t lcfg;
        memset(&lcfg, 0, sizeof(lcfg));
        ONEYE_DEV_STRUCT_INIT(lcfg);
        oneye_dev_link_config_defaults(&lcfg);
        lcfg.node_id = s_adv_sn;
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
    /* ★ 本批核心改动：device_id = creds 的 node_id ⇒ 广播名（SDK 自动派生 `ONEYE-<后 4 位>`）
     *   与配对 HKDF salt 同源（派生函数/调用点见文件头注释的 ①②）。 */
    bcfg.device_id = s_adv_sn;
    /* 广播厂商字段（AD 0xFF）的两段身份：SN = 同一个 node_id（同一条读取路径），品类走机型配置。 */
    bcfg.adv_sn = s_adv_sn;
    bcfg.adv_product_key = (CONFIG_ONEYE_FW_PRODUCT_KEY[0] != '\0') ? CONFIG_ONEYE_FW_PRODUCT_KEY : NULL;
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
    if (oneye_dev_ble_advertise(true) == ONEYE_DEV_SDK_OK) {
        s_advertising = true;
    } else {
        ESP_LOGW(TAG, "BLE 开始广播失败 ⇒ 手机可能扫不到本机");
    }

    s_started = true;
    s_pop[0] = '\0';
    /* ⚠️ 走到这里 `s_adv_sn_ready` 必为真（第 0 步 fail-closed 已保证）：故此处不再有"未带 SN"分支，
     *    ⛔ 该分支不是被放宽，而是**不可达**了 —— 读不到 creds 时本函数在上面就返回 ESP_FAIL。 */
    if (oneye_dev_ble_get_pop(s_pop, sizeof(s_pop)) == ONEYE_DEV_SDK_OK) {
        /* ★ 2026-10-07（本批）：就绪行必须把**身份来源**讲清楚 —— 广播名、配对 salt、厂商字段的 SN
         *   三者同源于 creds 的 node_id；App 用同一份 node_id 派生密钥、对配对码。
         *   这与"credSource≠partition 要告警"是同一条纪律（看不清有没有降级 = 最坏的形态）。
         *   ⚠️ 广播名的**后缀不在本文件里另算一遍**（那是第二处派生 = 第二事实源）：
         *      空口字节由 SDK 平台层开机打印（`oneye_ble_plat_nimble.c` 的扫描响应逐字节行）。 */
        ESP_LOGI(TAG, "BLE 配网通道已就绪：广播中（设备名 = SDK 由 node_id 派生 `ONEYE-<后 4 位>`，"
                      "本机 node_id=%s），配对 POP=%s（TTL %d s），"
                      "厂商字段(0xFF) 已带 SN（= 同一个 creds 分区 node_id）＋品类段",
                 s_adv_sn, s_pop, CONFIG_ONEYE_DEV_SDK_BLE_POP_TTL_S);
    } else {
        s_pop[0] = '\0';
        ESP_LOGI(TAG, "BLE 配网通道已就绪：广播中（POP 读回失败，串口另有 SDK 日志）；"
                      "node_id=%s（广播名由 SDK 由它派生）；"
                      "厂商字段(0xFF) 已带 SN（= 同一个 creds 分区 node_id）＋品类段",
                 s_adv_sn);
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
        ESP_LOGI(TAG, "已入网 ⇒ 停止 BLE 广播（契约：入网成功后不再保持可配网态）");
    }
}
