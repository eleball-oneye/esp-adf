/*
 * main.c —— thermometer-t31 双角色固件入口（**编译期**二选一）
 * =============================================================================
 *   CONFIG_ONEYE_T31_ROLE_GATEWAY（默认） = COM12 网关，甲型 OY-GAW-01：
 *       App BLE 配网 → 拿 creds 分区的一机一密 → 上云（影子/status）→ **释放 NimBLE**
 *       → oneye-link-sdk 重建 NimBLE 作**中心**连 T31 → 用 T31 的温度代报影子属性。
 *   CONFIG_ONEYE_T31_ROLE_SUBDEV          = COM13 温感子设备模拟器：
 *       只起 NimBLE **外设**模拟健奥 T31（见 t31_sim.c），⛔ 不连云、⛔ 不读 creds。
 *
 * ★ 网关启动序列（逐条对应 devkitc_oneye 的甲型口径，**去掉板级自检/摄像头/音频**）：
 *   app_main
 *     ① NVS init（+ erase 重试）
 *     ② boot_identity_report()        —— 广播身份 + creds 分区是否可用（只读，不决定行为）
 *     ③ ready_worker_start()          —— 8192 栈任务，入网后的**重活**都在它里面
 *     ④ wifi_prov_set_ready_cb(on_wifi_ready) / set_disconnected_cb(on_wifi_disconnected)
 *     ⑤ key_gate_start()              —— BOOT 长按 3 s = 忘记 WiFi + `esp_restart()`
 *     ⑥ wifi_prov_boot()              —— NVS 凭据优先，否则起 BLE 配网通道
 *   入网（IP_EVENT_STA_GOT_IP → wifi_prov.c → ready_cb）：
 *     on_wifi_ready（**sys_evt**）只做两件常数级动作：停广播 + `xTaskNotifyGive(wifi_ready)`
 *     wifi_ready_work（8192 栈）：
 *       ① wifi_prov_ble_deinit_after_provisioned()  —— **释放 NimBLE 整栈**（阻塞动作，必须在本任务）
 *       ② sntp_boot_sync()                          —— 严格 TLS 需"先有钟再建链"（失败仅 WARN）
 *       ③ xTaskCreate(cloud_start_task, 6144)
 *     cloud_start_task：
 *       creds 分区（**甲型无分区/坏 = 不联网，fail-closed**）→ base_init/set_caps →
 *       event_init（`event/up` 通道）+ 起 gw_child 任务 → base_start →
 *       上报初始影子 → **BLE 移交**（oneye_link_init 重建 NimBLE 作中心）→
 *       恢复云端下发的档案 → 起 stats_task
 *
 * ★ 子设备（用户 2026-10-23 裁定：独立实体，网关代传）见 gw_child.c：
 *   云端 `command/down` 下发 `child.pair` → 档案运行时注册 + 绑定落 NVS；
 *   子设备事件 → `event/up` 的 `sub.online` / `sub.offline` / `sub.telemetry` / `sub.error`。
 *   ⛔ 网关影子**只含网关自身键**（firmwareVersion/model/power+credSource），不塞子设备数据。
 *
 * ★ BLE 双角色移交（本工程的硬顺序，⛔ 不能换序）：
 *     peripheral（SDK 配网 GATT）──deinit 整栈──► central（link-sdk 扫描/连 T31）
 *   link-sdk **没有 stop/deinit** ⇒ 「重新配网」一律 = 清 NVS Wi-Fi 凭据后 `esp_restart()`
 *   （BOOT 长按就是这条路径，⛔ 不新增 SDK API、⛔ 不在运行期反切回 peripheral）。
 * =============================================================================
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#if CONFIG_ONEYE_T31_ROLE_GATEWAY
static const char *TAG = "t31gw";
#else
static const char *TAG = "t31sub";
#endif

/* ============================================================================
 * 0. 共享：NVS 初始化（**两个角色都要**；NimBLE 的 bonding store / SDK 的持久化面都依赖它）
 *    ⚠️ 本函数**必须定义在角色分派之前** —— 否则子设备构建会报
 *       `implicit declaration of function 'boot_nvs_init'`（本工程实测踩到过一次）。
 * ============================================================================ */
static esp_err_t boot_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除（%s）⇒ erase 后重试", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

#if CONFIG_ONEYE_T31_ROLE_GATEWAY

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "oneye_dev_sdk.h"      /* → oneye_dev_base.h / oneye_dev_types.h */
#include "oneye_dev_creds.h"    /* creds 分区：load / apply / is_absent / state_json */
#include "oneye_link.h"         /* link-sdk：中心侧接入（⛔ 不含 oneye_dev_link.h，见头文件） */
#include "oneye_link_profile.h"
#include "oneye_link_tlv.h"

#include "adv_identity.h"       /* 身份的**唯一**读取点（creds 的 node_id / sn） */
#include "oneye_status_period.h"/* 机型 → status/up 周期 H（与 cloud 机型配置同源） */
#include "sntp_boot.h"
#include "wifi_prov.h"          /* Wi-Fi 生命周期 + 入网后**唯一收敛点** */
#include "wifi_prov_ble.h"      /* BLE 配网通道；配网后 deinit 整栈 */
#include "board_key.h"          /* Korvo-2(DU1906) 板载 ADC 按键（任意键长按 ≥3 s = 重新配网） */
#include "gw_child.h"           /* 子设备绑定（child.pair/unpair）+ sub.* 事件上报 */

#else  /* ======== 子设备角色：只依赖 NimBLE 外设模拟器 ======== */
#include "t31_sim.h"
#endif /* CONFIG_ONEYE_T31_ROLE_GATEWAY（头文件分派） */

#if CONFIG_ONEYE_T31_ROLE_GATEWAY

/* 任务规格（栈字节数）：8 KB 是 devkitc 实测"sys_evt 溢出"的根治值，⛔ 不要压小 */
#define GW_READY_TASK_STACK 8192
#define GW_READY_TASK_PRIO  5
#define GW_CLOUD_TASK_STACK 6144
#define GW_CLOUD_TASK_PRIO  4
#define GW_STATE_TASK_STACK 4096
#define GW_STATE_TASK_PRIO  4
#define GW_STATS_TASK_STACK 4096
#define GW_STATS_TASK_PRIO  3

/* BOOT 键（ESP32-S3 板上 GPIO0，低有效、有外部上拉；同时是 strapping 脚 ⇒ 只在启动后读） */
#define GW_BOOT_GPIO        GPIO_NUM_0
#define GW_KEY_HOLD_MS      3000u   /* 长按阈值：3 s ⇒ 忘记 WiFi + 重启（破坏性动作，⛔ 不要缩短） */
#define GW_KEY_POLL_MS      50u

/* 影子 state 的 JSON 上限（temperature/battery/rssi/model/power + 余量） */
#define GW_STATE_MAX        224

/* ============================================================================
 * 1. 网关：诊断读数（`[res]`）—— 沿用 devkitc_oneye / korvo2_oneye 的口径
 *
 * 为什么保留阶段标签：多处读数没有标签就分不清"哪一刻的量"。本工程四处：
 *   pre-prov（配网与 NimBLE 之前）/ ble-ready（已起栈未入网）/
 *   pre-ble-deinit / post-ble-deinit（释放 NimBLE 前后，差额 = 回收量）。
 * 关键量是 `largest_internal_block`：任何 ≥8 KB 的新任务栈都要**连续**内部块。
 * ⛔ 只为读数新增常驻任务/定时器（本函数只读 heap_caps_*、不阻塞、不分配）。
 * ============================================================================ */
static void res_report(const char *phase)
{
    ESP_LOGI(TAG, "[res] phase=%s free_internal=%u B / largest_internal=%u B / free_psram=%u B",
             phase,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ============================================================================
 * 2. 开机自证：身份（creds 分区两串）+ 云端点 + 周期 H
 *
 * ⚠️ 本函数**只读**、⛔ 不决定任何行为（连不联网由 cloud_start_task 判）。
 *    node_id = **激活码（私密）**、sn = **公开**；两者⛔ 不得互推（adv_identity.h）。
 * ============================================================================ */
static void boot_identity_report(void)
{
    adv_identity_t ids;
    adv_identity_status_t why = ADV_IDENTITY_ERR_CREDS;
    adv_identity_status_t st;
    oneye_dev_creds_t probe;
    oneye_dev_sdk_err_t rc;
    uint16_t h_s = ONEYE_STATUS_INTERVAL_SEC();

    memset(&ids, 0, sizeof(ids));
    st = adv_identity_load_ex(&ids, &why);

    ESP_LOGI(TAG, "==== thermometer-t31 网关（甲型 OY-GAW-01 · ESP32-S3）启动 ====");
    ESP_LOGI(TAG, "固件 %s / oneye-dev-sdk %s / oneye-link-sdk %s",
             CONFIG_ONEYE_FW_VERSION, oneye_dev_sdk_version(), oneye_link_version());
    ESP_LOGI(TAG, "机型 product_key=%s（机型属性，⛔ 不是身份）/ 占位 device_id=%s",
             CONFIG_ONEYE_FW_PRODUCT_KEY, CONFIG_ONEYE_FW_DEVICE_ID);
    ESP_LOGI(TAG, "云端点 %s:%d（%s）/ status/up 周期 H=%us（cloud 在线键 TTL=6×H=%us）",
             CONFIG_ONEYE_FW_CLOUD_HOST, (int)CONFIG_ONEYE_FW_CLOUD_PORT,
#if CONFIG_ONEYE_FW_TRANSPORT_TLS
             "mqtts / 严格 TLS",
#else
             "非 TLS（仅台面）",
#endif
             (unsigned)h_s, (unsigned)(6u * (uint32_t)h_s));

    /* 广播身份：甲型必须来自 creds 分区（⛔ 不回落编译期常量：那是第二来源 = 冒充） */
    if (st == ADV_IDENTITY_OK) {
        ESP_LOGI(TAG, "广播身份：creds 可用 ⇒ SN=%s（公开，进广播名/厂商字段）"
                      " / node_id=**私密**（甲型配对核验基准；⛔ 不广播）/ sn_fell_back=%d",
                 ids.sn, ids.sn_fell_back ? 1 : 0);
    } else {
        ESP_LOGE(TAG, "广播身份：读不到（%s）⇒ 甲型 BLE 配网将 fail-closed"
                      "（配对凭据的基准就是 creds.node_id，⛔ 不回落 CONFIG_ONEYE_FW_DEVICE_ID）",
                 (why == ADV_IDENTITY_ERR_EMPTY) ? "creds.node_id 为空" : "creds 读取失败");
    }

    /* creds 分区的**可用性**（与上面同一来源，这里多打镜像读数，便于产线对账） */
    memset(&probe, 0, sizeof(probe));
    rc = oneye_dev_creds_load(&probe);
    if (rc == ONEYE_DEV_SDK_OK) {
        ESP_LOGI(TAG, "creds 分区：可用 node=%s（cert %u B / key %u B / CA %u B，crc32=0x%08X）"
                      " ⇒ 甲型可上云（fail-closed 条件满足）",
                 probe.node_id, (unsigned)probe.cert_len, (unsigned)probe.key_len,
                 (unsigned)probe.ca_len, (unsigned)probe.image_crc32);
        oneye_dev_creds_free(&probe);
    } else if (oneye_dev_creds_is_absent(rc)) {
        ESP_LOGE(TAG, "creds 分区：**未写入/不存在**（%s）⇒ 甲型**不联网**（fail-closed）",
                 oneye_dev_sdk_strerror(rc));
    } else {
        ESP_LOGE(TAG, "creds 分区：**损坏/读失败**（%s）⇒ 甲型**不联网**（⛔ 不回退任何公用凭据）",
                 oneye_dev_sdk_strerror(rc));
    }
}

/* ============================================================================
 * 3. 影子状态：latest 缓存（云链路重连后要"立刻补报一次"，见 on_dev_event）
 *
 * 并发：写入者是 link-sdk 事件任务 / cloud_start_task；读出者是 state_task 与事件回调
 * ⇒ 用一把轻量临界区（`portMUX`）保护整串拷贝。串 ≤224 B，临界区极短。
 * ============================================================================ */
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_latest_state[GW_STATE_MAX];

static void latest_state_set(const char *json)
{
    taskENTER_CRITICAL(&s_state_mux);
    (void)snprintf(s_latest_state, sizeof(s_latest_state), "%s", json);
    taskEXIT_CRITICAL(&s_state_mux);
}

static bool latest_state_get(char *out, size_t cap)
{
    bool ok;

    taskENTER_CRITICAL(&s_state_mux);
    ok = (s_latest_state[0] != '\0');
    if (ok) {
        (void)snprintf(out, cap, "%s", s_latest_state);
    }
    taskEXIT_CRITICAL(&s_state_mux);
    return ok;
}

/* ============================================================================
 * 4. 影子上报（**唯一**发送方：state_task）
 *
 * ⚠️ 为什么不在事件回调里直接 `oneye_dev_base_report_state()`：
 *    SDK 文档（oneye_link.h:14）明确"事件回调在 SDK 任务上下文执行，不得阻塞超过 20 ms"，
 *    而 `oneye_dev_base_report_state()` 是**另一个** SDK 的调用（序列化 + 入 MQTT 队列）。
 *    ⇒ 回调只做"解析 + 更新缓存 + 节流判断 + 通知"，真正的上报在本任务里做。
 *    这样既满足用户"节流后调 report_state"的口径，也不违反 SDK 的非阻塞约束。
 * ============================================================================ */
static TaskHandle_t s_state_task;
static volatile int64_t s_last_report_ms;   /* 上次**发起**上报的时刻（只在通知方写；失败时本任务清零） */

static void state_task(void *arg)
{
    (void)arg;
    for (;;) {
        char st[GW_STATE_MAX];

        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!latest_state_get(st, sizeof(st))) {
            continue;
        }
        oneye_dev_sdk_err_t rc = oneye_dev_base_report_state(st);
        if (rc == ONEYE_DEV_SDK_OK) {
            ESP_LOGI(TAG, "影子上报成功（shadow/up）：%s", st);
        } else {
            ESP_LOGW(TAG, "影子上报失败（%s）⇒ 不清节流位，下一帧再试（⛔ 不假装已上报）",
                     oneye_dev_sdk_strerror(rc));
            s_last_report_ms = 0;   /* 让下一帧立刻重试（否则要等满一个节流窗） */
        }
    }
}

/* ============================================================================
 * 5. 云事件：ONYE_DEV_SDK_EVT_CLOUD_LINK_UP ⇒ **立刻**补报当前影子；
 *    ONEYE_DEV_SDK_EVT_COMMAND_RECEIVED（`command/down`）⇒ 转交 gw_child
 *
 * ⚠️ 本回调在 SDK 内部上下文执行 ⇒ ⛔ 不直接调 SDK（重入），只置位 + 通知 state_task；
 *    命令面同理：本回调里**只解析 + 投递队列**，ack/档案注册/NVS 全在 gw_child 的任务里
 *    （oneye_link.h:15 "事件回调在 SDK 任务上下文执行，不得阻塞超过 20 ms"）。
 * ============================================================================ */
static void on_dev_event(oneye_dev_event_t evt, const void *payload, size_t len, void *user_ctx)
{
    (void)user_ctx;

    switch (evt) {
    case ONEYE_DEV_SDK_EVT_CLOUD_LINK_UP:
        ESP_LOGI(TAG, "云链路已建立（CLOUD_LINK_UP）⇒ 立刻补报一次当前影子");
        s_last_report_ms = 0;                    /* 补报不受节流限制 */
        if (s_state_task != NULL) {
            (void)xTaskNotifyGive(s_state_task);
        }
        break;
    case ONEYE_DEV_SDK_EVT_CLOUD_LINK_DOWN: {
        /* 🔴 2026-10-23 真机：TLS 握手 `E esp-aes: Failed to allocate memory` ⇒ SDK 反复重连
         *   （`mqtt link down (reason=-6)` → 立刻 up → 又 down，attempt 7/8）。
         *   这里加**重连风暴检测**（60 s 滚动窗口）：窗口内第 3 次起打印内部堆读数，
         *   便于烧后直接确认"是否还是内部 RAM 不足"（判据 free/largest 是否 < 8 KB）。 */
        static uint32_t s_down_cnt;
        static int64_t  s_down_win_ms;
        const int64_t now_ms = esp_timer_get_time() / 1000;

        if (s_down_cnt == 0u || (now_ms - s_down_win_ms) > 60000) {
            s_down_cnt = 0u;
            s_down_win_ms = now_ms;
        }
        s_down_cnt++;
        ESP_LOGW(TAG, "云链路断开（CLOUD_LINK_DOWN）⇒ SDK 自动重连；重连成功后补报影子"
                      "（60 s 窗口内第 %u 次）", (unsigned)s_down_cnt);
        if (s_down_cnt >= 3u) {
            ESP_LOGW(TAG, "[diag] 重连风暴（%u 次/60 s）⇒ 内部堆 free=%u B / largest=%u B"
                          "（<8 KB 即 TLS 握手内存不足；已关硬件 AES/MPI）",
                     (unsigned)s_down_cnt,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        break;
    }
    case ONEYE_DEV_SDK_EVT_COMMAND_RECEIVED:
        /* child.pair / child.unpair 等 `command/down`：解析与回执都在 gw_child 的任务里 */
        gw_child_on_command(payload, len);
        break;
    default:
        break;
    }
}

/* ============================================================================
 * 6. 子设备数据：本函数**只打日志**（本地可读性）；
 *    对云的那一路在 gw_child.c（`event/up` 的 `sub.telemetry`，节流后上报）
 *
 * TLV 口径（oneeye_link_tlv.h，⛔ 与帧体 LE 不同）：
 *   · ONEYE_TLV_TEMP        0x10 = i16，单位 0.01 ℃，**大端**
 *     （oneye_link_tlv.c 的 put_u16 是 `{v>>8, v&0xFF}`）
 *   · ONEYE_TLV_BATTERY_PCT 0x08 = u8，0–100（0xFF = 未知）
 * 网关影子键（**逐字**，必须与云端 OY-GAW-01 的 TSL 一致）：
 *   firmwareVersion / model / power（+ credSource）
 * ⛔ temperature/battery/rssi 属于**子设备自己的 TSL**（OY-THE-01），⛔ 不再由网关影子上报。
 * ============================================================================ */
static void gw_on_sub_data(const oneye_link_event_t *evt, const char *addr)
{
    oneye_link_tlv_reader_t rd;
    uint16_t type = 0;
    const uint8_t *val = NULL;
    uint8_t vlen = 0;
    bool has_temp = false;
    float temp_c = 0.0f;
    int batt = -1;
    char pretty[192] = { 0 };
    char raw_hex[49] = { 0 };       /* 2026-10-23 诊断：原始帧 hex，最多 24 B（49 = 48+1） */
    char state[GW_STATE_MAX];
    int64_t now_ms;
    {
        static const char k_hex[] = "0123456789ABCDEF";
        size_t n = (evt->raw != NULL) ? (size_t)evt->raw_len : 0u;
        size_t o = 0;

        if (n > 24u) {
            n = 24u;
        }
        for (size_t i = 0; i < n && (o + 2u) < sizeof(raw_hex); i++) {
            raw_hex[o++] = k_hex[(evt->raw[i] >> 4) & 0x0Fu];
            raw_hex[o++] = k_hex[evt->raw[i] & 0x0Fu];
        }
        raw_hex[o] = '\0';
    }

    if (evt->tlv != NULL && evt->tlv_len > 0) {
        (void)oneye_link_tlv_format(evt->tlv, evt->tlv_len, pretty, sizeof(pretty));
    }
    /* 🔴 2026-10-23 诊断（真机"温度解不出"时要有帧可看）：raw 原始帧 hex(≤24 B) + 解析出的 cmd
     *   一并打出来；`tlv` 仍是归一化结果。判据：cmd=0x01 且 TLV 里出现 TEMP(0x10)。 */
    ESP_LOGI(TAG, "[数据] %s cmd=0x%02X(has_cmd=%d) rssi=%d raw(%u B)=%s TLV(%u B)=%s",
             addr, evt->has_cmd ? evt->cmd : 0, (int)evt->has_cmd, (int)evt->rssi,
             (unsigned)((evt->raw != NULL) ? evt->raw_len : 0u),
             raw_hex[0] ? raw_hex : "(无)",
             (unsigned)evt->tlv_len, pretty[0] ? pretty : "(空)");

    oneye_link_tlv_reader_init(&rd, evt->tlv, evt->tlv_len);
    while (oneye_link_tlv_next(&rd, &type, &val, &vlen)) {
        if (type == ONEYE_TLV_TEMP && vlen >= 2 && val != NULL) {
            int16_t centi = (int16_t)(((uint16_t)val[0] << 8) | (uint16_t)val[1]);  /* 大端 */
            temp_c = (float)centi / 100.0f;
            has_temp = true;
        } else if (type == ONEYE_TLV_BATTERY_PCT && vlen >= 1 && val != NULL) {
            batt = (int)val[0];
        }
    }

    if (!has_temp) {
        /* 没解出温度：只留日志（可能是透传帧 —— 真 T31 的 cmd=0x01 body 编码未解出时就是这样） */
        ESP_LOGW(TAG, "[数据] %s 本帧**未解出温度**（TLV 无 0x10；原始帧见上行日志）", addr);
        return;
    }

    /* 🔴 2026-10-23（用户裁定 C：子设备是独立实体）：温度**不再**作为网关自己的影子属性上报。
     * 对云的那一路 = `event/up` 的 `sub.telemetry`（gw_child.c，节流后发）：
     *   data = {node:<子设备 node_id>, model:<档案 model>, rssi, cmd, tlv, raw}
     * 由云端按子设备机型（OY-THE-01）的 TSL 校验并写**子设备自己**的影子/时序。
     * ⚠️ 只有**已配对**的子设备才代传（node_id 来自 child.pair）；未配对的在 gw_child 里丢弃并计数。 */
    ESP_LOGI(TAG, "[子设备] %s 温度 %.2f ℃（battery=%d rssi=%d）⇒ 对云走 sub.telemetry（gw_child）",
             addr, (double)temp_c, batt, (int)evt->rssi);
    (void)state;
    (void)now_ms;
}

/* ============================================================================
 * 7. link-sdk 事件回调（**SDK 任务上下文** ⇒ ⛔ 不阻塞、⛔ 重活）
 *
 * 本函数只做两件事：① 把事件原样转交 gw_child（它只拷贝 + 投递队列，对云上报在自己的任务里）；
 * ② 打本地日志。⛔ 不在这里调 oneye_dev_* 的任何上报 API。
 * ============================================================================ */
static void on_link_event(const oneye_link_event_t *evt, void *ctx)
{
    char addr[18] = { 0 };

    (void)ctx;
    oneye_link_addr_str(&evt->sub, addr, sizeof(addr));

    /* ① 先转交（原始帧指针只在本回调内有效，必须由 gw_child 当场拷走） */
    gw_child_on_link_event(evt);

    switch (evt->type) {
    case ONEYE_LINK_EVT_SUB_ONLINE:
        /* 上线：model 进日志；影子上的 power 保持 true（子设备在线 = 在工作） */
        ESP_LOGI(TAG, "[上线] %s model=%s rssi=%d（继续按温度帧代报影子）",
                 addr, evt->model, (int)evt->rssi);
        break;
    case ONEYE_LINK_EVT_SUB_OFFLINE:
        /* 离线：按用户口径**只记日志**，不把 power 改成 false
         * （T31 是体温贴，"离线"多为超出范围/休眠，不代表设备关机；影子里的最后一次温度仍需可读） */
        ESP_LOGW(TAG, "[离线] %s model=%s（⛔ 不改 power；只保留最后一次影子值）",
                 addr, evt->model);
        break;
    case ONEYE_LINK_EVT_SUB_DATA:
        gw_on_sub_data(evt, addr);
        break;
    case ONEYE_LINK_EVT_SUB_ERROR:
        ESP_LOGW(TAG, "[错误] %s err=%d（link-sdk 未提供 err_str ⇒ 只打码）", addr, (int)evt->err);
        break;
    case ONEYE_LINK_EVT_CLOUD:
        ESP_LOGI(TAG, "[链路] link-sdk 云状态事件（本工程上行由 oneye-dev-sdk 负责，此处仅留痕）");
        break;
    default:
        break;
    }
}

/* ============================================================================
 * 8. 统计任务（保留旧例程风格：30 s 一行 + 逐子设备明细）
 * ============================================================================ */
static void stats_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        oneye_link_stats_t st;
        if (oneye_link_get_stats(&st) == ONEYE_LINK_OK) {
            ESP_LOGI(TAG, "统计: 在线=%u 已知=%u 收帧=%llu 解析=%llu 透传=%llu 丢弃=%llu "
                          "上行=%llu 缓存=%llu 云=%d",
                     (unsigned)st.subdevs_online, (unsigned)st.subdevs_known,
                     (unsigned long long)st.frames_rx, (unsigned long long)st.frames_parsed,
                     (unsigned long long)st.frames_passthrough,
                     (unsigned long long)st.frames_dropped,
                     (unsigned long long)st.uplink_sent, (unsigned long long)st.uplink_queued,
                     st.cloud_connected ? 1 : 0);
        }

        size_t n = oneye_link_subdev_count();
        for (size_t i = 0; i < n; i++) {
            oneye_link_subdev_info_t info;
            char a[18] = { 0 };
            memset(&info, 0, sizeof(info));
            ONEYE_LINK_STRUCT_INIT(info);
            if (!oneye_link_subdev_get(i, &info)) {
                continue;
            }
            oneye_link_addr_str(&info.addr, a, sizeof(a));
            ESP_LOGI(TAG, "  子设备 %s model=%s state=%d rssi=%d batt=%u frames=%llu dropped=%llu",
                     a, info.model, (int)info.state, (int)info.rssi, (unsigned)info.battery,
                     (unsigned long long)info.frames, (unsigned long long)info.dropped);
        }
        res_report("stats-tick");
    }
}

/* ============================================================================
 * 9. 云链路（**唯一**调用点，只在 cloud_start_task 内）
 *
 * 甲型的失败方向（契约 §4.4.2 甲列 / 凭据分区契约 §7 红线）：
 *   · creds 分区**没有/没写过** ⇒ 不联网（fail-closed）；
 *   · creds 分区**坏了**         ⇒ 不联网（⛔ 不回退任何公用凭据）。
 *   ⚠️ 本工程**不内嵌任何证书**（main/CMakeLists.txt 无 EMBED_TXTFILES）：甲型的一机一密
 *      材料只在 creds 分区里，回退路径根本不存在 ⇒ 「坏了也照跑」这条路被物理掐掉。
 * ============================================================================ */
/* ★ 2026-10-23（用户裁定）：「本次开机是否 **经 BLE 配网** 刚入网」。
 * 定义放在 cloud_start_task **之前**：该任务要用它打「上云路径」日志。
 * 写方 = wifi_ready_work（单写，且只写一次）；读方 = cloud_start_task。
 * ⚠️ 用 volatile：让编译器⛔ 不把它缓存进寄存器（跨任务传递的意图很明确）。 */
static volatile bool s_ble_fresh_prov;

static void cloud_start_task(void *arg)
{
    static oneye_dev_creds_t s_creds;          /* SDK **借用**其中的指针直到 base_init 结束 */
    oneye_dev_base_config_t cfg;
    oneye_dev_caps_t caps;
    oneye_dev_sdk_err_t rc;
    oneye_dev_creds_source_t cred_src = ONEYE_DEV_CREDS_SOURCE_UNKNOWN;
    char shadow[GW_STATE_MAX];

    (void)arg;

    /* ★ 2026-10-23：把「本次是 BLE 首次配网（已过宽限窗）」这一事实带进云任务日志，
     *   便于真机对照两种开机路径（NVS 直连 vs BLE 配网）。只读，⛔ 不改任何行为。 */
    ESP_LOGI(TAG, "上云路径：%s（BLE 宽限窗=%d s）",
             s_ble_fresh_prov ? "BLE 配网成功（已过宽限窗）" : "NVS 凭据直连（未走宽限窗）",
             (int)CONFIG_ONEYE_T31_PROV_GRACE_SEC);

    memset(&cfg, 0, sizeof(cfg));
    ONEYE_DEV_STRUCT_INIT(cfg);
    oneye_dev_base_config_defaults(&cfg);

    /* ---- 身份（甲型的**唯一**来源）：creds 分区 ---------------------------------- */
    rc = oneye_dev_creds_load(&s_creds);
    if (rc == ONEYE_DEV_SDK_OK) {
        if (oneye_dev_creds_apply(&cfg, &s_creds) != ONEYE_DEV_SDK_OK) {
            ESP_LOGE(TAG, "凭据写入 base 配置失败（缺私钥 / 身份不合法）⇒ **不联网**");
            vTaskDelete(NULL);
            return;
        }
        cred_src = ONEYE_DEV_CREDS_SOURCE_PARTITION;
        ESP_LOGI(TAG, "凭据来源：**creds 分区** node=%s（cert %u B / key %u B / CA %u B）",
                 s_creds.node_id, (unsigned)s_creds.cert_len, (unsigned)s_creds.key_len,
                 (unsigned)s_creds.ca_len);
    } else if (oneye_dev_creds_is_absent(rc)) {
        ESP_LOGE(TAG, "甲型无 creds 分区（%s）→ **不联网**（fail-closed；⛔ 不回落任何公用凭据）",
                 oneye_dev_sdk_strerror(rc));
        vTaskDelete(NULL);
        return;
    } else {
        ESP_LOGE(TAG, "creds 分区损坏（%s）→ **不联网**（⛔ 坏了不回退；串口原因见上，设备送修）",
                 oneye_dev_sdk_strerror(rc));
        vTaskDelete(NULL);
        return;
    }

    /* ---- 云端点与承载（与 Kconfig 逐项对应；⛔ 不用 backend_uri，避免两个事实源）---- */
    cfg.cloud_host = CONFIG_ONEYE_FW_CLOUD_HOST;
    cfg.cloud_port = (uint16_t)CONFIG_ONEYE_FW_CLOUD_PORT;
#if CONFIG_ONEYE_FW_TRANSPORT_TLS
    cfg.transport = ONEYE_DEV_TRANSPORT_MQTT_TLS;
#else
    cfg.transport = ONEYE_DEV_TRANSPORT_MQTT_TCP;
#endif
#if CONFIG_ONEYE_FW_TLS_INSECURE
    cfg.tls_insecure_skip_verify = true;    /* 仅 dev/产测；⛔ 生产必须 n（信任链来自 creds 的 CA） */
#else
    cfg.tls_insecure_skip_verify = false;
#endif
    cfg.enable_status_topic = true;         /* status/up（retained + 遗嘱 LWT） */
    cfg.status_interval_s = ONEYE_STATUS_INTERVAL_SEC();
    cfg.event_cb = on_dev_event;
    cfg.user_ctx = NULL;

    rc = oneye_dev_base_init(&cfg);
    if (rc != ONEYE_DEV_SDK_OK) {
        ESP_LOGE(TAG, "base init 失败：%s ⇒ 不联网", oneye_dev_sdk_strerror(rc));
        vTaskDelete(NULL);
        return;
    }

    /* 能力集：**严格口径**（本工程无音视频/显示 ⇒ NONE 是如实声明，⛔ 不是"开关没开"） */
    memset(&caps, 0, sizeof(caps));
    ONEYE_DEV_STRUCT_INIT(caps);
    caps.caps = (uint32_t)ONEYE_DEV_CAP_NONE;
    caps.model_version = "1";
    caps.fw_version = CONFIG_ONEYE_FW_VERSION;
    if (oneye_dev_base_set_caps(&caps) != ONEYE_DEV_SDK_OK) {
        ESP_LOGW(TAG, "set_caps 失败（仅告警：不影响影子/温度上报）");
    }

    /* ---- event 库与子设备任务：**推迟到 link_start 之后**（2026-10-23 内存批，两轮订正）------
     * 旧行为（旧字留痕，⛔ 不抹）：这里就 `oneye_dev_event_init()` —— SDK 缺省实占 ≈34 KB
     *   内部 RAM（queue 4 KB + offline 4 KB + 固定 16 KB track + scratch）。它在
     *   `oneye_link_init()` 之前就把内部堆吃掉 ⇒ 真机 `nimble_port_init 失败: ESP_ERR_NO_MEM`。
     * 第一轮：挪到 `oneye_link_init()` 成功之后、`oneye_link_start()` 之前。
     *   真机读数：`post-link-init largest_internal=31744` → event 库一起掉到 `7680`
     *   ⇒ `oneye_link_start 失败`（start 也要连续内部块）。
     * 第二轮（现行为）：再挪到 `oneye_link_start()` **之后**（见下方 link 块末尾）。
     * ⚠️ 风险（如实登记）：base_start 之后、event 库起来之前的窗口内若云端下发 `child.pair`，
     *   该命令的**事件侧**可能丢（base 仍会把 COMMAND_RECEIVED 分发给 on_dev_event，但
     *   `oneye_dev_event_report` 依赖 event 库）。判据：`event 库已起（link_start 之后）`
     *   出现在任何 `child.pair` 之前即无影响。
     */

    res_report("pre-base-start（creds 已装载、MQTT 将申请 RX/TXQ）");
    rc = oneye_dev_base_start();
    if (rc != ONEYE_DEV_SDK_OK) {
        ESP_LOGE(TAG, "base start 失败：%s ⇒ 不联网", oneye_dev_sdk_strerror(rc));
        vTaskDelete(NULL);
        return;
    }
    if (oneye_dev_base_wait_event(3000) != ONEYE_DEV_SDK_OK) {
        ESP_LOGW(TAG, "3 s 内未等到首个云事件（链路可能仍在建；⛔ 继续走，影子会在 LINK_UP 时补报）");
    }

    /* ---- 影子上报任务（要早于 link-sdk 事件，否则首帧会丢通知）-------------------- */
    if (xTaskCreate(state_task, "gw_state", GW_STATE_TASK_STACK, NULL, GW_STATE_TASK_PRIO,
                    &s_state_task) != pdPASS) {
        s_state_task = NULL;
        ESP_LOGE(TAG, "影子任务创建失败（内存不足）⇒ 温度帧只进缓存、不上云");
    }

    /* ---- 初始影子（**逐字**按云端 TSL 的键）------------------------------------
     * 2026-10-23 用户裁定 C（子设备独立实体）后，网关影子**只含网关自身键**：
     *   {"firmwareVersion":…,"model":"T31","power":true}
     *  + credSource（`oneye_dev_creds_state_json()` 产出后**内层**拼入；取不到就省略该键）。
     * ⛔ 不再含 temperature/battery/rssi —— 那些属于子设备自己的 TSL（OY-THE-01）。
     * ⚠️ JSON 合法性的钉子（devkitc 2026-10-20 踩过）：`oneye_dev_creds_state_json()` 产出的
     *    是**整个对象** `{"credSource":"partition"}` ⇒ 去掉开头的 `{` 后，结尾那个 `}`
     *    正好当本对象的收尾，⛔ **不再补第二个 `}`、⛔ 也不重复写键名**。
     */
    {
        char src_json[64];
        if (oneye_dev_creds_state_json(cred_src, src_json, sizeof(src_json)) == ONEYE_DEV_SDK_OK) {
            const char *inner = (src_json[0] == '{') ? src_json + 1 : "\"credSource\":\"unknown\"}";
            (void)snprintf(shadow, sizeof(shadow),
                           "{\"firmwareVersion\":\"%s\",\"model\":\"T31\",\"power\":true,%s",
                           CONFIG_ONEYE_FW_VERSION, inner);
        } else {
            (void)snprintf(shadow, sizeof(shadow),
                           "{\"firmwareVersion\":\"%s\",\"model\":\"T31\",\"power\":true}",
                           CONFIG_ONEYE_FW_VERSION);
            ESP_LOGW(TAG, "凭据来源未知 ⇒ 初始影子省略 credSource（⛔ 报含糊值比不报更坏）");
        }
        latest_state_set(shadow);
        rc = oneye_dev_base_report_state(shadow);
        if (rc == ONEYE_DEV_SDK_OK) {
            ESP_LOGI(TAG, "初始影子已上报：%s", shadow);
        } else {
            ESP_LOGW(TAG, "初始影子上报失败（%s）⇒ 链路 UP 时会自动补报同一份：%s",
                     oneye_dev_sdk_strerror(rc), shadow);
        }
    }

    /* ========================================================================
     * ★ BLE 双角色**移交**：此刻 NimBLE 已被 `wifi_prov_ble_deinit_after_provisioned()`
     *   整栈释放（wifi_ready_work 里做的，**早于**本任务），所以这里可以让
     *   oneye-link-sdk **重新起 NimBLE 作中心**。⛔ 顺序不可换：
     *     peripheral 起栈 → 配网成功 → deinit 整栈 → central 起栈
     *   若把 deinit 挪到本函数或更晚：`nimble_port_init()` 会返回"已初始化"，
     *   link-sdk 的 central 起不来（link-sdk 没有 stop/deinit，⛔ 也不能在运行期反切）。
     *   ⚠️ 2026-10-23 内存批：这里打印 `[res] pre-link-init` —— 它就是
     *      `nimble_port_init()` 前的**唯一判据点**（真机要求 largest_internal ≥ 12 KB）。
     * ======================================================================== */
    res_report("pre-link-init（NimBLE central 申请内部 RAM 前）");
    {
        oneye_link_config_t lcfg;

        memset(&lcfg, 0, sizeof(lcfg));
        ONEYE_LINK_STRUCT_INIT(lcfg);
        /* ⚠️ node_id 取 **creds 分区的 node_id**（= 云端认得的那台设备），
         *    ⛔ 不用 CONFIG_ONEYE_FW_DEVICE_ID（那只是台面占位；甲型必然有分区凭据）。 */
        lcfg.node_id = s_creds.node_id;
        lcfg.max_links = 4;
        lcfg.scan_interval_ms = 1000;
        lcfg.scan_window_ms = 200;
        lcfg.cache_entries = 512;
        lcfg.passthrough_unknown = true;

        if (oneye_link_init(&lcfg) != ONEYE_LINK_OK) {
            ESP_LOGE(TAG, "oneye_link_init 失败 ⇒ **T31 子设备接入不可用**"
                          "（云端仍在线，但不会有温度；检查 BLE deinit 是否早于本调用）");
            res_report("link-init-FAILED（NimBLE 没起来）");
        } else {
            res_report("post-link-init（NimBLE central 已起）");
            const char *t31 = oneye_link_profile_builtin_t31();
            if (oneye_link_profile_register_json(t31, strlen(t31)) != ONEYE_LINK_OK) {
                ESP_LOGE(TAG, "内置 T31 档案注册失败");
            }

            /* ---- 云端下发过的档案（NVS 里的 linkProfile）先注册回来 --------------------
             * 🔴 2026-10-23 真机订正（顺序是硬的，与上一版**相反**）：
             *   上一版 = 内置 → SIM → NVS；真机结果仍是 `cmd=0x00 + RAW_FRAME[8]`（解码走
             *   passthrough），说明**运行时生效的不是 SIM 档**（NVS 里那条云端扁平档按同 model
             *   后来居上覆盖了 SIM）。⇒ 本仓自测阶段把顺序钉成：
             *     ① 内置 t31.json（保证有基础档）
             *     ② NVS/云端下发档（先让它进来，校验不合法则回退 SIM 并写回自愈）
             *     ③ **SIM 完整档（最后注册，同 model 一定赢）** ⇒ 保证 COM13 模拟器解码走 SIM
             *   ⚠️ 接真 T31 时把 ③ 去掉（改为云端档最后注册）。
             *   顺序仍必须早于 `oneye_link_start()`（link-sdk 要求"先有档案再 start"）。 */
            gw_child_restore_profiles();

            /* ③ 覆盖为"模拟器自洽档案"（**同一 model = 覆盖**）：
             * 内置 t31.json 的 cmd 0x01 `fields: []`（真 T31 的 body 编码尚未解出）
             * ⇒ 用本仓 COM13 模拟器的帧体自洽口径补上字段映射，温度才能进 TLV 0x10。
             * 🔴 2026-10-23：常量已**上移为共享事实源** `gw_child_sim_profile_t31()`
             *   （gw_child.c）—— 档案回退（下发残缺 / NVS 不合法）用的也是这一份。
             *   `body_len: 0` = 按帧动态取长度（0x01=4 B、0xB1=3 B）。
             * ⚠️ 接**真 T31** 时删掉这份覆盖、改用内置 t31.json。 */
            {
                const char *sim = gw_child_sim_profile_t31();
                oneye_link_err_t prc =
                    oneye_link_profile_register_json(sim, strlen(sim));
                if (prc == ONEYE_LINK_OK) {
                    ESP_LOGI(TAG, "已注册**模拟器** T31 档案覆盖（cmd 0x01: temp i16le@0 → TLV 0x10；"
                                  "%u B；真 T31 编码未解出，本档案只保证本仓两块板自洽）",
                             (unsigned)strlen(sim));
                } else {
                    ESP_LOGW(TAG, "模拟器档案注册失败（%d）⇒ 退回内置档案：温度将只能靠透传原始帧",
                             (int)prc);
                }
                /* 🔴 2026-10-23 开机自检（用户要求，避免再盲烧）：硬编码样本帧验解码路径，
                 *   目标 `cmd=0x01 temp=3668`；打印运行时档案数/模型与生效档命令/字段数。 */
                gw_child_profile_selftest();
            }

            if (oneye_link_register_event_cb(on_link_event, NULL) != ONEYE_LINK_OK) {
                ESP_LOGE(TAG, "link 事件回调注册失败 ⇒ 收不到子设备数据");
            }

            /* ⚠️ 2026-10-23 内存批（第二轮，真机反馈后）：判据点 = `link_start` **之前**。
             *   第一轮真机读数：`post-link-init largest_internal=31744` → event 库一起就掉到
             *   `7680` → `oneye_link_start 失败`。⇒ event 库必须再往后挪到 start **之后**。 */
            res_report("pre-link-start（NimBLE central 已起、link_start 前）");
            if (oneye_link_start() != ONEYE_LINK_OK) {
                ESP_LOGE(TAG, "oneye_link_start 失败 ⇒ 不扫描子设备");
            } else {
                ESP_LOGI(TAG, "★ T31 接入已启动：扫描 1000/200 ms、最多 4 链路、"
                              "命中 `T31*` + 服务 FFF0、notify FFF1");
                res_report("post-link-start（扫描已启动）");
            }

            /* ★🔴 2026-10-23（真机 `子设备事件队列满`）：**先建队列 + 任务，再建 event 库**。
             *   上一版顺序 = 先 init event（≈34 KB 内部 RAM）→ 再 `xTaskCreate(gw_child, 8192)`；
             *   内存一紧任务创建就会失败，而**队列仍在**（`on_link_event` 照发）⇒ 从此无人出队，
             *   真机表现正是「队列满 + 后续无任何上报日志」。
             *   任务先起：event 库尚未 init 时上报只会返回 STATE（带 rc/耗时日志），
             *   队列始终有人排空 ⇒ ⛔ 不会堵死。 */
            if (!gw_child_report_start()) {
                ESP_LOGE(TAG, "子设备任务未起 ⇒ child.pair 与 sub.* 都不可用（影子与温度日志仍工作）");
            }

            /* ========================================================================
             * ★ 2026-10-23 内存批（第二轮）：event 库挪到 `oneye_link_start()` **之后**。
             *   第一轮放在 start 之前 ⇒ 真机 `largest_internal=7680` 不够，start 失败。
             * ⚠️ 风险（如实登记）：start 后到本段执行之间的毫秒级窗口内，若子设备立刻上报，
             *   首帧会因 event 库未起而丢（日志里有 `rc=STATE` 可判；扫描命中通常 ≥1 s）。
             * ⛔ 不要再挪回 start 之前：那是真机验证过的 OOM 点。
             * ======================================================================== */
            {
                oneye_dev_event_config_t ev_cfg;
                oneye_dev_sdk_err_t erc;

                memset(&ev_cfg, 0, sizeof(ev_cfg));
                ONEYE_DEV_STRUCT_INIT(ev_cfg);
                ev_cfg.enabled = true;
                ev_cfg.track_enabled = false;
                ev_cfg.queue_bytes = 4u * 1024u;
                ev_cfg.offline_bytes = 4u * 1024u;
                ev_cfg.event_cb = on_dev_event;
                ev_cfg.user_ctx = NULL;

                erc = oneye_dev_event_init(&ev_cfg);
                if (erc == ONEYE_DEV_SDK_OK) {
                    ESP_LOGI(TAG, "event 库已起（link_start 之后；queue=%u B / offline=%u B / track=off）"
                                  "⇒ sub.* 可上行",
                             (unsigned)ev_cfg.queue_bytes, (unsigned)ev_cfg.offline_bytes);
                } else {
                    ESP_LOGE(TAG, "event 库初始化失败（%s）⇒ 子设备事件不会上行（影子与回执不受影响）",
                             oneye_dev_sdk_strerror(erc));
                }
                res_report("event-init（link_start 之后）");
            }
        }
    }

    if (xTaskCreate(stats_task, "gw_stats", GW_STATS_TASK_STACK, NULL, GW_STATS_TASK_PRIO,
                    NULL) != pdPASS) {
        ESP_LOGW(TAG, "统计任务创建失败（仅影响日志，不影响上云）");
    }

    res_report("cloud-start（SDK + link 都已起步）");
    ESP_LOGI(TAG, "云链路 + T31 接入均已就绪；本任务退出（栈让给 BLE/TLS）");
    vTaskDelete(NULL);
}

/* ============================================================================
 * 10. 入网后收敛点（**唯一**触发边；所有配网通道共用）
 *
 * ★ 执行上下文与触发边分离（2026-10-20 devkitc 真机踩到栈溢出后的根治口径）：
 *   `on_wifi_ready()` 跑在 **sys_evt**（CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE，缺省 2304）
 *   ⇒ 这里只许**常数级**动作：停广播 + 任务通知。
 *   重活（加密上报 / mbedtls 授时 / deinit NimBLE / 建上云任务）全在 `wifi_ready`（8192）。
 * ============================================================================ */
static TaskHandle_t s_ready_task;
static bool s_cloud_started;

/* `s_ble_fresh_prov` 的**定义**在 cloud_start_task 之前（那里要用它打"上云路径"日志）；
 * 这里是它的**写入点**说明，⛔ 不要在此重复定义。 */

/* 本次开机是否「经 BLE 配网成功」刚入网 —— 判据 = 凭据来源的规范标签 == "ble"
 * （取值闭集见 `wifi_prov.h:248`）。**只读、可重入、无副作用**。
 * 宽限窗关闭（`CONFIG_ONEYE_T31_PROV_GRACE_SEC=0`）时恒 false ⇒ 完全退回旧行为。 */
static bool wifi_ready_is_ble_fresh(void)
{
#if CONFIG_ONEYE_T31_PROV_GRACE_SEC > 0
    const char *src = wifi_prov_source();
    return src != NULL && strcmp(src, "ble") == 0;
#else
    return false;
#endif
}

static void wifi_ready_work(void)
{
    if (s_cloud_started) {
        return;                        /* 上云链路只启动一次（重连不重建） */
    }
    s_cloud_started = true;

    ESP_LOGI(TAG, "Wi-Fi 已入网：ip=%s（ssid=%s，来源 %s）",
             wifi_prov_ip(), wifi_prov_ssid(), wifi_prov_source());

    /* ★★ 2026-10-23 用户裁定：「deinit BLE 的时机需要重新设计，要确保给 App 返回结果
     *    让其能发起绑定请求后才能执行」。⇒ 本次经 BLE 配网时：
     *      ① 先把 `prov.status{connected, ip}` 发给 App（BLE 仍活着，走 SDK 既有发送路径）；
     *      ② 再进宽限窗：保持 BLE 广播/可连接 N 秒（CONFIG_ONEYE_T31_PROV_GRACE_SEC），
     *         期间⛔ 不 deinit、⛔ 不启云链路；**会话断开即提前结束**（2026-10-23 用户反馈
     *         「添加成功，但用时很久」⇒ 不再死等满窗）；
     *      ③ 窗满/提前结束才 deinit（下面的既有路径）。
     *    NVS 已有凭据的普通开机⛔ 不走本窗（尽快上云，保持旧行为）。 */
    bool prov_status_sent = false;
    s_ble_fresh_prov = wifi_ready_is_ble_fresh();
    if (s_ble_fresh_prov) {
        esp_err_t prc = wifi_prov_ble_report_prov_connected(wifi_prov_ssid(), wifi_prov_ip(),
                                                            wifi_prov_source());
        if (prc == ESP_OK) {
            prov_status_sent = true;
            ESP_LOGI(TAG, "★ prov.status{state=connected, ip=%s} 已交给 BLE 发送路径 ⇒ "
                          "App 可立即发起绑定请求", wifi_prov_ip());
        } else if (prc == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "prov.status 未发出（link 未 init/未 start）⇒ App 可能收不到配网结果");
        } else {
            ESP_LOGW(TAG, "prov.status 发送失败（%s）⇒ App 可能收不到配网结果；"
                          "宽限窗仍保持 BLE 可连接", esp_err_to_name(prc));
        }

#if CONFIG_ONEYE_T31_PROV_GRACE_SEC > 0
        {
            /* 提前结束（2026-10-23 用户反馈「添加成功，但用时很久」）：
             *   App 完成绑定后会**断开 BLE** ⇒ 每秒轮询 SDK 只读快照
             *   （`wifi_prov_ble_session_connected()` → `oneye_dev_ble_get_status()->connected`），
             *   观察到「有连接 → 断开」即立刻结束宽限；
             *   最短驻留 PROV_GRACE_MIN_S = 5 s（防 App 刚报完 connected 就闪断把窗口掐死）。 */
            const int PROV_GRACE_MIN_S = 5;
            const int total = (int)CONFIG_ONEYE_T31_PROV_GRACE_SEC;
            bool seen_conn = false;
            int disc_polls = 0;
            int elapsed = 0;

            ESP_LOGI(TAG, "[prov-grace] 进入宽限窗 %d s：BLE 保持广播/可连接、"
                          "⛔ 不 deinit、⛔ 不启动云链路（等 App 绑定；会话断开则提前结束）", total);
            for (elapsed = 0; elapsed < total; elapsed++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (wifi_prov_ble_session_connected()) {
                    disc_polls = 0;
                    if (!seen_conn) {
                        seen_conn = true;
                        ESP_LOGI(TAG, "[prov-grace] 检测到配网会话已连接（App 正在与设备交互）");
                    }
                } else if (seen_conn) {
                    disc_polls++;
                    if (disc_polls >= 2 && (elapsed + 1) >= PROV_GRACE_MIN_S) {
                        ESP_LOGI(TAG, "[prov-grace] 会话断开 ⇒ 提前结束宽限（已等 %d s / 上限 %d s）",
                                 elapsed + 1, total);
                        break;
                    }
                } else if (prov_status_sent && (elapsed + 1) >= PROV_GRACE_MIN_S) {
                    /* 窗口内一次都没观察到连接：App 已结束配网会话（或从未重连）⇒ 不再干等 */
                    ESP_LOGI(TAG, "[prov-grace] 未观察到配网会话（App 未重连）⇒ 提前结束宽限"
                                  "（已等 %d s / 上限 %d s）", elapsed + 1, total);
                    break;
                }
                if (((elapsed + 1) % 5) == 0 && (elapsed + 1) < total) {
                    ESP_LOGI(TAG, "[prov-grace] 剩余 %d s（保持 BLE 可连接，等待 App 绑定）",
                             total - (elapsed + 1));
                }
            }
            if (elapsed >= total) {
                ESP_LOGI(TAG, "[prov-grace] 宽限窗满 %d s（未收到会话断开信号）", total);
            }
            ESP_LOGI(TAG, "[prov-grace] 结束宽限 ⇒ 释放 NimBLE 整栈、切换 link-sdk 作中心");
        }
#endif
    }

    /* ---- ① 释放 NimBLE 整栈：把 BLE 让给 link-sdk 作中心 -------------------------
     * 硬要求：必须在 **8192 栈的任务**里调（`nimble_port_stop()` 要等主机任务退出，属阻塞动作），
     * ⛔ 不能放 sys_evt，⛔ 不能在 cloud_start_task 里（那时 LInk 已要 init，晚一步就起不来）。 */
    res_report("pre-ble-deinit（已入网；宽限窗已结束）");
    {
        esp_err_t ble_rc = wifi_prov_ble_deinit_after_provisioned();
        if (ble_rc == ESP_OK) {
            ESP_LOGI(TAG, "★ NimBLE 整栈已释放（配网外设使命结束）⇒ 交由 oneye-link-sdk 重建作中心");
        } else {
            ESP_LOGW(TAG, "NimBLE deinit 未成功（%s）⇒ link-sdk 可能起不来（看下面 oneye_link_init 结果）",
                     esp_err_to_name(ble_rc));
        }
    }
    res_report("post-ble-deinit（建云链路前）");

    /* ---- ② 授时：严格 TLS 必须"先有钟再建链"（失败仅 WARN，不阻塞上云）----------- */
    if (!sntp_boot_sync()) {
        ESP_LOGW(TAG, "SNTP 授时未成功（仅 WARN）⇒ 严格 TLS 下可能 BADCERT_FUTURE"
                      "（可临时 CONFIG_ONEYE_FW_TLS_INSECURE=y 定位，⛔ 生产禁止）");
    }

    /* ---- ③ 建上云任务 ------------------------------------------------------------ */
    if (xTaskCreate(cloud_start_task, "gw_cloud", GW_CLOUD_TASK_STACK, NULL, GW_CLOUD_TASK_PRIO,
                    NULL) != pdPASS) {
        s_cloud_started = false;       /* 内存不足 ⇒ 允许下一次 GOT_IP 再试 */
        ESP_LOGE(TAG, "上云任务创建失败（内存不足）—— 设备已联网但不会上平台");
        return;
    }
    ESP_LOGI(TAG, "已联网 → 启动上云（oneye-dev-sdk）");
}

static void ready_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* 等 on_wifi_ready() 的一次通知 */
        wifi_ready_work();
    }
}

static void ready_worker_start(void)
{
    if (xTaskCreate(ready_worker_task, "wifi_ready", GW_READY_TASK_STACK, NULL,
                    GW_READY_TASK_PRIO, &s_ready_task) != pdPASS) {
        s_ready_task = NULL;
        ESP_LOGE(TAG, "入网后处理器（8192 B）创建失败 ⇒ 入网后重活退回 sys_evt（⚠️ 退化路径，有溢出风险）");
        return;
    }
    ESP_LOGI(TAG, "入网后处理器已起步（task=wifi_ready stack=%d）：BLE deinit / SNTP / 建云任务都在它里面",
             (int)GW_READY_TASK_STACK);
}

/* 收敛点本体：**运行在 sys_evt** ⇒ 只许常数级动作 */
static void on_wifi_ready(void)
{
    if (!s_cloud_started && wifi_ready_is_ble_fresh()) {
        /* ★ 宽限窗：⛔ 不停广播 —— App 收到 prov.status{connected} 后要**再连一次设备**
         *   发绑定请求；此刻停广播它就找不到设备（用户 2026-10-23 逐字裁定）。
         *   ⚠️ 本分支只打一行日志，真正的等待在 wifi_ready_work（8192 任务，可阻塞）。 */
        ESP_LOGI(TAG, "[prov-grace] 本次为 BLE 配网成功 ⇒ **保持广播**（宽限 %d s，等 App 绑定）",
                 (int)CONFIG_ONEYE_T31_PROV_GRACE_SEC);
    } else {
        wifi_prov_ble_stop_advertising();     /* 已入网 ⇒ 停广播（幂等） */
    }

    if (s_ready_task != NULL) {
        if (xTaskNotifyGive(s_ready_task) != pdPASS) {
            ESP_LOGW(TAG, "通知 wifi_ready 失败（本次不入网后流程；下一段 GOT_IP 会再来）");
            return;
        }
        return;
    }
    ESP_LOGW(TAG, "[diag] wifi_ready 未起步 ⇒ 退回 sys_evt 直接执行（⚠️ 退化路径）");
    wifi_ready_work();
}

/* Wi-Fi 会话结束（断开）：只留痕。
 * ⚠️ 本工程**不**在这里做任何重活：`s_cloud_started` 保持 true（上云链路不重建），
 *    SDK 自己重连；link-sdk 的 BLE 中心也不受 Wi-Fi 影响。 */
static void on_wifi_disconnected(void)
{
    ESP_LOGW(TAG, "Wi-Fi 断开（本段配网会话结束）⇒ SDK 自动重连；"
                  "BLE 中心（T31 接入）不受影响");
}

/* ============================================================================
 * 11. 配网启动：NVS 凭据优先 → 否则 BLE 配网通道
 *
 * ⚠️ 本机型**只有 BLE 一条交互式通道**（无 SD 卡槽）：`CONFIG_ONEYE_FW_ENABLE_WIFI_FILE`
 *    这个符号在本工程根本不存在 ⇒ C3 凭据文件那一段编不进来（不是"忘了打开"）。
 * ⛔ 本函数**不做任何"入网后"动作**：那些全在 on_wifi_ready() 的收敛点下游。
 * ============================================================================ */
static void gw_wifi_prov_boot(void)
{
    char ssid[WIFI_PROV_SSID_MAX] = { 0 };
    char pass[WIFI_PROV_PASS_MAX] = { 0 };

    if (wifi_prov_load_nvs(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK && ssid[0] != '\0') {
        if (wifi_prov_submit_credentials(ssid, pass, WIFI_PROV_CHAN_NVS) == ESP_OK) {
            ESP_LOGI(TAG, "NVS 记忆的 Wi-Fi 入网成功（ssid=%s）⇒ **本次不进 BLE 配网广播**；"
                          "上云链路由收敛点下游自动启动", ssid);
            return;
        }
        ESP_LOGW(TAG, "NVS 凭据未能入网（ssid=%s）⇒ 继续起 BLE 配网通道（AP 暂时不可达属预期）",
                 ssid);
    }

#if CONFIG_ONEYE_FW_ENABLE_BLE_PROV
    wifi_prov_ble_set_autostart(true);    /* 开机即广播（本工程缺省不按键门控） */
    if (wifi_prov_ble_start() == ESP_OK) {
        ESP_LOGW(TAG, "下一步：手机 App 扫描并连接 **ONEYE-<广播名后缀>**，"
                      "配对凭据 = **一次性 chal**（⛔ 配对阶段不判 code）；"
                      "本机档位 甲 ALPHA ⇒ prov.verify.req **必经**（与 creds.node_id 逐字比对）");
        ESP_LOGW(TAG, "长按 BOOT(GPIO0) ≥ %u ms = **忘记 WiFi + 重启**（重新配网的唯一路径）",
                 (unsigned)GW_KEY_HOLD_MS);
        res_report("ble-ready（NimBLE 已起栈；入网前）");
        return;
    }
    ESP_LOGE(TAG, "BLE 配网通道启动失败 ⇒ 本次启动不会上云（本机型没有第二条配网通道）");
#else
    ESP_LOGE(TAG, "本构建未启用 BLE 配网通道（CONFIG_ONEYE_FW_ENABLE_BLE_PROV=n）"
                  "且无 SD 卡通道 ⇒ **一条配网通道都没有**（配置错误）");
#endif
}

/* ============================================================================
 * 12. 按键：**长按 BOOT ≥3 s = 忘记 WiFi + esp_restart()**
 *
 * 为什么是"重启"而不是"就地重新配网"（本工程的硬约束）：
 *   `oneye-link-sdk` **没有 stop/deinit** ⇒ 一旦 NimBLE 被它接管作中心，就无法在运行期
 *   退回 peripheral 重新配网。⇒ 重新配网 = 清 NVS Wi-Fi 凭据 + `esp_restart()`，
 *   下次上电走完整启动序列（NVS 无凭据 ⇒ 进 BLE 广播）。⛔ 不新增 SDK API。
 * ⚠️ 短按（< 3 s）无副作用；RESET = 硬件复位。
 * ============================================================================ */
static void key_gate_task(void *arg)
{
    bool pressed = false;
    bool long_fired = false;
    int64_t t0 = 0;

    (void)arg;
    for (;;) {
        const bool down = (gpio_get_level(GW_BOOT_GPIO) == 0);
        const int64_t now = esp_timer_get_time();

        if (down && !pressed) {
            pressed = true;
            long_fired = false;
            t0 = now;
            ESP_LOGI(TAG, "[key] BOOT(GPIO0) 按下：按住 ≥ %u ms = 忘记 WiFi 凭据并重启；短按无动作",
                     (unsigned)GW_KEY_HOLD_MS);
        } else if (!down && pressed) {
            pressed = false;
            if (!long_fired) {
                ESP_LOGI(TAG, "[key] BOOT(GPIO0) 短按（%lld ms < %u ms）⇒ 无动作",
                         (long long)((now - t0) / 1000), (unsigned)GW_KEY_HOLD_MS);
            }
        } else if (down && pressed && !long_fired &&
                   (now - t0) >= (int64_t)GW_KEY_HOLD_MS * 1000) {
            long_fired = true;
            ESP_LOGW(TAG, "[key] 长按 BOOT(GPIO0) ≥ %u ms ⇒ 忘记 WiFi 凭据 + 重启（重新配网）",
                     (unsigned)GW_KEY_HOLD_MS);
            /* `wifi_prov_forget_wifi()` 会 esp_wifi_stop()（阻塞）⇒ 必须在任务上下文调用 */
            esp_err_t rc = wifi_prov_forget_wifi("BOOT-3s-reprovision");
            if (rc != ESP_OK) {
                ESP_LOGE(TAG, "[key] 忘记 WiFi 失败（%s）⇒ 仍重启；若下次开机仍自动连旧网，请再按一次"
                              "或擦 nvs 分区", esp_err_to_name(rc));
            }
            ESP_LOGW(TAG, "[key] 重启中…… 下次上电 NVS 无 Wi-Fi 凭据 ⇒ 直接进 BLE 配网广播");
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(GW_KEY_POLL_MS));
    }
}

static void key_gate_start(void)
{
    gpio_config_t io = { 0 };

    io.pin_bit_mask = (1ULL << (int)GW_BOOT_GPIO);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGW(TAG, "BOOT 键（GPIO%d）配置失败 ⇒ 长按重配不可用（只能 RESET / 重烧）",
                 (int)GW_BOOT_GPIO);
        return;
    }
    if (xTaskCreate(key_gate_task, "gw_key", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "按键任务创建失败（内存不足）⇒ 长按重配不可用");
        return;
    }
    ESP_LOGI(TAG, "按键就绪：长按 BOOT(GPIO0) ≥ %u ms = 忘记 WiFi + 重启（重新配网）；短按无副作用",
             (unsigned)GW_KEY_HOLD_MS);
}

#endif /* CONFIG_ONEYE_T31_ROLE_GATEWAY */

/* ============================================================================
 * 13. app_main（按角色分派）
 * ============================================================================ */
void app_main(void)
{
    esp_err_t err = boot_nvs_init();

    if (err != ESP_OK) {
        /* NVS 不可用：网关必然失败（creds/wifi/SDK 都依赖），子设备也起不来（NimBLE store）
         * ⇒ 如实报错并停在这里（⛔ 不静默继续跑一个功能残缺的固件）。 */
        ESP_LOGE(TAG, "NVS 初始化失败（%s）⇒ 停止启动", esp_err_to_name(err));
        return;
    }

#if CONFIG_ONEYE_T31_ROLE_GATEWAY
    /* ① 自证（身份 / creds / 云端点 / 周期）——只读，不影响行为 */
    boot_identity_report();

    /* ①b 子设备绑定表：从 NVS 恢复（只读；档案注册要等 oneye_link_init 之后） */
    gw_child_boot();

    res_report("pre-prov（配网与 NimBLE 之前）");

    /* ② 入网后处理器**必须先于** ready_cb 与 wifi_prov_boot：
     *    前者回调里要往该任务投递；后者一起栈就可能入网。 */
    ready_worker_start();

    /* ③ 收敛点注册（**唯一**注册点） */
    wifi_prov_set_ready_cb(on_wifi_ready);
    wifi_prov_set_disconnected_cb(on_wifi_disconnected);

    /* ④ 按键（长按 = 忘记 WiFi + 重启）
     * 🔴 2026-10-23：Korvo-2（DU1906）**没有 BOOT 键**，它的 VOLUP/VOLDOWN/MUTE/SET 四键挂在
     *   GPIO2 的 ADC 电阻梯上（`components/audio_board/esp32_korvo_du1906/board_def.h:84-116`）；
     *   ⇒ 增加板载 ADC 按键长按触发（任意键长按 ≥3 s = 重新配网广播）。 */
    key_gate_start();
    (void)board_key_start();

    /* ⑤ 配网：NVS 凭据优先，否则 BLE 通道 */
    gw_wifi_prov_boot();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
#else
    /* ---- 子设备角色：只起 NimBLE 外设（⛔ 不连云、⛔ 不读 creds、⛔ 不做配网）---- */
    ESP_LOGI(TAG, "==== thermometer-t31 温感子设备模拟器（ESP32-S3）启动 ====");
    ESP_LOGI(TAG, "固件 %s / 角色 SUBDEV / 周期 %d ms",
             CONFIG_ONEYE_FW_VERSION, (int)CONFIG_ONEYE_T31_SUB_TEMP_REPORT_MS);

    err = t31_sim_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "子设备模拟器启动失败（%s）⇒ 本次不广播", esp_err_to_name(err));
        return;
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
#endif
}
