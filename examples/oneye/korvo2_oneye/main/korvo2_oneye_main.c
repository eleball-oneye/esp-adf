/*
 * korvo2_oneye —— ESP32-S3-Korvo-2 板级固件（ESP-ADF 工程）
 *
 * 本轮目标（板级参数核对 + 固件构建）：
 *   ① 板级硬件参数**编译期断言**（main/board_expect.h）；
 *   ② 板级硬件参数**运行期自检**（board_selftest()）：逐行打印 expect/actual + PASS/FAIL；
 *   ③ ADF 板级初始化（ES8311 回放 + ES7210 采集 + 6 键；SD/LCD 由 Kconfig 开关）；
 *   ④ oneye-dev-sdk 接入：base（建链/能力集/影子/命令回执/在线状态）+ log + event（mpp 由开关）；
 *   ⑤ 按键 → `event/up`（`type=device_event`，见 backend/contracts/domain/事件与埋点上报.md §3.1）。
 *
 * 契约纪律（勿越界）：
 *   - 不发明通道/字段：自检结论走 `log/up` 文本；影子只写**已登记键**（`firmwareVersion` / `power`）；
 *   - 能力位严格：默认 `ONEYE_DEV_CAP_NONE`；`video.live` 需真机取帧取证后才可声明（Kconfig 开关）；
 *     `audio.intercom` 在 WebRTC 数据面落地前**一律不得声明**（本固件不提供开关）；
 *   - 事实源：backend/contracts/api/mqtt/Korvo-2设备能力与协议映射.md（板级能力↔协议唯一对照页）。
 *
 * 事实源（硬件）：
 *   docs/zh_CN/design-guide/dev-boards/user-guide-esp32-s3-korvo-2.rst（V3.1）
 *   components/audio_board/esp32_s3_korvo2_v3/{board.c,board_def.h,board_pins_config.c}
 */

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "board.h"
#include "es7210.h"
#include "esp_peripherals.h"
#include "input_key_service.h"

#include "oneye_dev_sdk.h"

#include "board_expect.h"
#include "panel_api.h"
#include "aec_capture.h"
#include "player.h"
#include "lcd_ui.h"
#include "camera_api.h"
#include "wifi_prov.h"
#include "wifi_prov_ble.h"     /* 配网通道 C1/C2（BLE）；入网后收敛点仍在 wifi_prov.c */
#include "net_probe.h"
#include "oneye_dev_creds.h"
#include "sntp_boot.h"

static const char *TAG = "korvo2_oneye";

#define ONEYE_FW_VERSION   "0.1.0"

/* 板级查询 API：`board_pins_config.c:164-167` 有实现，但上游 `board_pins_config.h` **未声明**
 * （上游头文件缺声明，非本工程缺陷）——此处按实现签名补本地声明，用于运行期核对。 */
extern int8_t get_es8311_mclk_src(void);

/* 契约已登记的 `device_event` 事件类型（事件与埋点上报.md §3.1：未知 type 由服务端按 device_event 兜底） */
#define ONEYE_FW_EVENT_TYPE_DEVICE_EVENT  "device_event"

/* ---------------------------------------------------------------- 板级自检 */

static int s_check_total;
static int s_check_failed;

static void chk_int(const char *item, int expect, int actual)
{
    s_check_total++;
    char se[16], sa[16];
    snprintf(se, sizeof(se), "%d", expect);
    snprintf(sa, sizeof(sa), "%d", actual);
    if (expect == actual) {
        ESP_LOGI(TAG, "[board-check] %-34s expect=%-6d actual=%-6d PASS", item, expect, actual);
        panel_api_record_check(item, se, sa, true);
    } else {
        s_check_failed++;
        ESP_LOGE(TAG, "[board-check] %-34s expect=%-6d actual=%-6d FAIL", item, expect, actual);
        panel_api_record_check(item, se, sa, false);
    }
}

static void chk_str(const char *item, const char *expect, const char *actual)
{
    s_check_total++;
    const char *act = actual ? actual : "(null)";
    if (actual != NULL && strcmp(expect, actual) == 0) {
        ESP_LOGI(TAG, "[board-check] %-34s expect=%-6s actual=%-6s PASS", item, expect, act);
        panel_api_record_check(item, expect, act, true);
    } else {
        s_check_failed++;
        ESP_LOGE(TAG, "[board-check] %-34s expect=%-6s actual=%-6s FAIL", item, expect, act);
        panel_api_record_check(item, expect, act, false);
    }
}

static void chk_ok(const char *item, bool ok, const char *note)
{
    s_check_total++;
    if (ok) {
        ESP_LOGI(TAG, "[board-check] %-34s %s PASS", item, note ? note : "ok");
        panel_api_record_check(item, "ok", note ? note : "ok", true);
    } else {
        s_check_failed++;
        ESP_LOGE(TAG, "[board-check] %-34s %s FAIL", item, note ? note : "fail");
        panel_api_record_check(item, "ok", note ? note : "fail", false);
    }
}

/*
 * 提示级检查（WARN）：**不计入 s_check_failed**，因此不会触发 STRICT 中止。
 * 用于"配件类"模块（如摄像头）：它缺失时板子其余功能（上云/按键/录音/回放/LCD）仍应可用，
 * 面板照实渲染为红行即可（/api/status.panel.camera 同时也给 inited=false）。
 * 真机教训（2026-09-16）：把摄像头探测算作硬失败 ⇒ 未装配/探测失败时整机中止上云，
 * 设备连 IP 都拿不到，反而**看不到**失败原因。
 */
static int s_check_warn;

static void chk_warn(const char *item, bool ok, const char *note)
{
    s_check_total++;
    if (ok) {
        ESP_LOGI(TAG, "[board-check] %-34s %s PASS", item, note ? note : "ok");
        panel_api_record_check(item, "ok", note ? note : "ok", true);
    } else {
        s_check_warn++;
        ESP_LOGW(TAG, "[board-check] %-34s %s WARN（不计入 STRICT 中止）", item, note ? note : "fail");
        panel_api_record_check(item, "ok", note ? note : "fail", false);
    }
}

/*
 * 运行期自检：期望值取自 rst（V3.1），实际值取自板级查询 API。
 * 编译期可断言的宏在 board_expect.h 中已断言，本函数不重复。
 */
static void board_selftest(void)
{
    ESP_LOGI(TAG, "==== Korvo-2 板级参数自检（rst V3.1 ↔ board_pins_config.c）====");

    /* I2C：rst「I2C 测试点 J18」CLK=IO18 / SDA=IO17；board_pins_config.c:35-48 */
    i2c_config_t i2c = { 0 };
    if (get_i2c_pins(I2C_NUM_0, &i2c) == ESP_OK) {
        chk_int("i2c.sda (rst J18)", 17, i2c.sda_io_num);
        chk_int("i2c.scl (rst J18)", 18, i2c.scl_io_num);
    } else {
        chk_ok("i2c pins 查询", false, "get_i2c_pins 返回失败");
    }

    /* I2S0：rst「编解码器测试点 J15」「ADC 测试点 J16」；board_pins_config.c:50-72 */
    board_i2s_pin_t i2s = { 0 };
    if (get_i2s_pins(0, &i2s) == ESP_OK) {
        chk_int("i2s0.mclk (rst J15/J16)", 16, i2s.mck_io_num);
        chk_int("i2s0.bck  (rst SCLK)", 9, i2s.bck_io_num);
        chk_int("i2s0.ws   (rst LRCK)", 45, i2s.ws_io_num);
        chk_int("i2s0.dout (rst DSDIN)", 8, i2s.data_out_num);
        chk_int("i2s0.din  (rst SDOUT)", 10, i2s.data_in_num);
    } else {
        chk_ok("i2s pins 查询", false, "get_i2s_pins 返回失败");
    }

    /* 其余单脚 API：rst「管脚分配列表」/「IO 扩展器 GPIO 分配」 */
    chk_int("pa_enable_gpio (IO48)", 48, get_pa_enable_gpio());
    chk_int("headphone_detect", -1, get_headphone_detect_gpio());
    chk_int("sdcard_intr_gpio", -1, get_sdcard_intr_gpio());
    chk_int("sdcard_pwr_ctrl_gpio", -1, get_sdcard_power_ctrl_gpio());
    chk_int("sdcard_open_file_num_max", 5, get_sdcard_open_file_num_max());
    chk_int("green_led_gpio (无)", -1, get_green_led_gpio());
    /* ADF 的 getter 返回 **int8_t**，而 TCA9554 P7 位 = BIT(7) = 0x80 ⇒ 经 int8_t 回传为 -128。
     * 真机实测即 -128（不是板子的问题，是取值类型口径）：期望值按同口径折算再比对。 */
    chk_int("blue_led_gpio (TCA9554 P7)", (int)(int8_t)(1 << 7), get_blue_led_gpio());
    chk_int("es8311_mclk_src (ESP MCLK)", 0, get_es8311_mclk_src());

    /* ADC 输入通道格式：rst「AEC 电路」——4 通道，R = 硬件 AEC 回采；board_def.h:126 */
    chk_str("adc_input_ch_format", "RMNM", AUDIO_ADC_INPUT_CH_FORMAT);

    ESP_LOGI(TAG, "[board-check] 结果：%d 项，失败 %d 项", s_check_total, s_check_failed);
}

/* ---------------------------------------------------------------- 板级初始化 */

static esp_periph_set_handle_t s_periph_set;
static audio_board_handle_t    s_board;

static void board_init_peripherals(void)
{
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    s_periph_set = esp_periph_set_init(&periph_cfg);
    if (s_periph_set == NULL) {
        chk_ok("esp_periph_set_init", false, "外设集合创建失败");
        return;
    }

    audio_board_handle_t board = audio_board_init();
    s_board = board;
    chk_ok("audio_board_init(ES8311+ES7210)",
           board != NULL && board->audio_hal != NULL && board->adc_hal != NULL,
           board == NULL ? "board=NULL" : (board->audio_hal == NULL ? "codec=NULL" :
                                          (board->adc_hal == NULL ? "adc=NULL" : "codec+adc ok")));

#if CONFIG_ONEYE_FW_ENABLE_KEYS
    esp_err_t key_rc = audio_board_key_init(s_periph_set);
    chk_ok("audio_board_key_init (6 键 ADC)", key_rc == ESP_OK, esp_err_to_name(key_rc));
#endif

#if CONFIG_ONEYE_FW_ENABLE_SDCARD
    /* rst：一线模式 microSD（无卡检测；board.c:173-201）——无卡时失败属预期，单独告警 */
    if (audio_board_sdcard_init(s_periph_set, SD_MODE_1_LINE) == ESP_OK) {
        chk_ok("sdcard mount (1-line)", true, "mounted");
        aec_capture_set_sd_mounted(true);
    } else {
        ESP_LOGW(TAG, "[board-check] %-34s fail（无卡检测引脚 ⇒ 无卡时属预期，需人工核对）",
                 "sdcard mount (1-line)");
        aec_capture_set_sd_mounted(false);      /* AEC 录音改用 SPIFFS 兜底 */
    }
#else
    aec_capture_set_sd_mounted(false);          /* 未启用 SD ⇒ 录音落 SPIFFS */
#endif

#if CONFIG_ONEYE_FW_ENABLE_LCD
    /* rst：ILI9341 320x240 + TCA9554 CS/RST/BL（board.c:89-153）——与 ADF 例程同源
     * （examples/display/lcd_jpeg|lcd_camera 均用 audio_board_lcd_init + esp_lcd_panel_draw_bitmap）。 */
    void *lcd_panel = audio_board_lcd_init(s_periph_set, NULL);
    chk_ok("lcd init (ILI9341 320x240)", lcd_panel != NULL, "panel");
    if (lcd_panel != NULL && lcd_ui_attach(lcd_panel) == ESP_OK) {
        (void)lcd_ui_set_pattern("status");        /* 初始状态屏（后续随按键/链路刷新） */
    }
#endif
}

/* ---------------------------------------------------------------- SDK 事件 */

static const char *evt_name(oneye_dev_event_t evt)
{
    switch (evt) {
    case ONEYE_DEV_SDK_EVT_CLOUD_LINK_UP:     return "CLOUD_LINK_UP";
    case ONEYE_DEV_SDK_EVT_CLOUD_LINK_DOWN:   return "CLOUD_LINK_DOWN";
    case ONEYE_DEV_SDK_EVT_MESSAGE:           return "MESSAGE";
    case ONEYE_DEV_SDK_EVT_CAPS_APPLIED:      return "CAPS_APPLIED";
    case ONEYE_DEV_SDK_EVT_CAPS_MISMATCH:     return "CAPS_MISMATCH";
    case ONEYE_DEV_SDK_EVT_SHADOW_DELTA:      return "SHADOW_DELTA";
    case ONEYE_DEV_SDK_EVT_COMMAND_RECEIVED:  return "COMMAND_RECEIVED";
    case ONEYE_DEV_SDK_EVT_OTA_NOTIFY:        return "OTA_NOTIFY";
    case ONEYE_DEV_SDK_EVT_TIME_SYNCED:       return "TIME_SYNCED";
    case ONEYE_DEV_EVENT_EVT_ACK:             return "EVENT_ACK";
    case ONEYE_DEV_EVENT_EVT_CLOUD_DIRECTIVE: return "EVENT_DIRECTIVE";
    case ONEYE_DEV_LOG_EVT_LEVEL_CHANGED:     return "LOG_LEVEL_CHANGED";
    case ONEYE_DEV_MPP_EVT_SESSION_STATE:     return "MPP_SESSION_STATE";
    case ONEYE_DEV_MPP_EVT_TRANSPORT_VERDICT: return "MPP_TRANSPORT";
    case ONEYE_DEV_MPP_EVT_ERROR:             return "MPP_ERROR";
    default:                                  return "OTHER";
    }
}

/* ---- 授时状态（契约 §7：`caps/down.cloud_ts` 首选来源）----
 * 未授时时 offset=0 ⇒ 时间戳为**运行时刻**（并已在串口打告警），授时后立即纠偏；
 * 面板/按键历史用同一 offset 显示真实 UTC 时间。 */
static bool     s_time_synced;
static uint64_t s_cloud_ts_ms;
static int64_t  s_time_offset_ms;

static uint64_t fw_wall_ms(void)
{
    uint64_t up = (uint64_t)(esp_timer_get_time() / 1000);
    return s_time_synced ? (uint64_t)((int64_t)up + s_time_offset_ms) : up;
}

static void on_time_synced(const void *payload, size_t len)
{
    uint64_t cts = 0;
    if (payload == NULL || len < sizeof(cts)) {
        return;
    }
    memcpy(&cts, payload, sizeof(cts));
    uint64_t up = (uint64_t)(esp_timer_get_time() / 1000);
    s_cloud_ts_ms = cts;
    s_time_offset_ms = (int64_t)cts - (int64_t)up;
    s_time_synced = true;
    panel_api_set_time(true, cts, s_time_offset_ms, "caps/down.cloud_ts");
    ESP_LOGI(TAG, "[time] 已授时：cloud_ts=%llu ms、本地偏移=%lld ms（此后所有 ts 为 UTC 毫秒）",
             (unsigned long long)cts, (long long)s_time_offset_ms);
}

/* 授时请求任务：`oneye_dev_base_sync_time()` 会**阻塞等待**云端应答，**不得**在 SDK 回调
 * （网络线程）里调用（会自锁）；故链路建立后由本任务发起，并重试到成功或放弃
 * （放弃后仍可由服务端在 `status/up` 时的周期性再同步兜底）。 */
static volatile bool s_sync_task_running;

static void time_sync_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 6 && !s_time_synced; i++) {
        oneye_dev_sdk_err_t rc = oneye_dev_base_sync_time(5000);
        if (rc == ONEYE_DEV_SDK_OK) {
            break;
        }
        ESP_LOGW(TAG, "授时请求第 %d 次未成功：%s（5 s 后重试）", i + 1, oneye_dev_strerror(rc));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    s_sync_task_running = false;
    vTaskDelete(NULL);
}

static void request_time_sync(void)
{
    if (s_time_synced || s_sync_task_running) {
        return;
    }
    s_sync_task_running = true;
    if (xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 3, NULL) != pdPASS) {
        s_sync_task_running = false;
        ESP_LOGW(TAG, "授时任务创建失败（内存不足），等待服务端周期性再同步");
    }
}

/* ---------------------------------------------------------------- LCD 状态屏
 * 板载 ILI9341 的**本地验证面**展示：链路 + 最近按键（人眼/拍照即可核对，
 * 与验证面板显示的是同一份事实）。未启用 LCD（Kconfig）时全部为 no-op。 */
static bool s_lcd_cloud_up;
static char s_lcd_key[32] = "KEY -";

static void lcd_update(const char *key_line)
{
#if CONFIG_ONEYE_FW_ENABLE_LCD
    if (!lcd_ui_ready()) {
        return;
    }
    if (key_line != NULL) {
        snprintf(s_lcd_key, sizeof(s_lcd_key), "%s", key_line);
    }
    char l2[32];
    snprintf(l2, sizeof(l2), "CLOUD %s", s_lcd_cloud_up ? "UP" : "DOWN");
    (void)lcd_ui_show_status("LCD OK 320X240", l2, s_lcd_key);
#else
    (void)key_line;
#endif
}

/* 云确认关联的前向声明（sdk_event_cb 早于其定义使用；实现见「按键 → event/up」段） */
static const char *key_pending_pop(void);
static bool ack_payload_is_ok(const char *payload, size_t len);

/* -------------------------------------------------------- command/down → command/up
 *
 * 契约（正本 = `cloud/contracts/api/mqtt/asyncapi.yaml`，设备面唯一事实源）：
 *   `rmng/dev/{node}/command/down` → `ControlCommand`  required `[id, params]`（**RAW，不套信封**）
 *   `rmng/dev/{node}/command/up`   → `ControlAck`      required `[id, status]`、`status ∈ {ok, error}`、
 *                                                      可选 `error`（**RAW，不套信封**）
 *   `id` **由服务端生成**，是回执关联的唯一依据（`backend/contracts/domain/控制通路.md` §2）。
 *
 * 为什么这里回的是 `error` 而不是 `ok`：本固件**没有任何命令执行面**（PTZ/命令执行属 S16，未落地），
 * `caps/up` 的 `cmds[]` 为空 ⇒ 按契约每条命令都是**未知命令**：
 * 《物模型与能力集.md》§5 逐字「命令可用集合 = 物模型 `cmds`；**下发未知命令应回执 `status=error`**」，
 * 《Korvo-2设备能力与协议映射.md》§2 第 6 行同判词。⇒ 回 `error` 是**契约规定的正确行为**；
 * 回 `ok` 才是假回执（把"没做"报成"做了"）。
 *
 * 这一步补的是「收到命令完全不回执」的缺口：不回执时云端 `dev_command.status` 会一直停在 `SENT`
 * 直到超时（`控制通路.md` §4「设备离线」行：无存储转发 ⇒ 只能等超时）。
 *
 * `error` 是 free text（asyncapi `ControlAck.error` = `type: string`，非闭集），措辞取 SDK 自身
 * 文档与单测里的既有字面量（`docs/API-base.md:486` / `tests/test_base.c:541`），**不新造名字**。
 */
static void command_ack_unknown(const void *payload, size_t len)
{
    cJSON *doc;
    const cJSON *id;
    oneye_dev_sdk_err_t rc;

    /* MQTT 载荷**不保证 NUL 结尾** ⇒ 必须用带长度的解析（SDK 内部同样按 len 处理，
     * 固件此前的打印也用 `%.*s` 而不是 `%s`）。 */
    doc = cJSON_ParseWithLength((const char *)payload, len);
    if (doc == NULL) {
        ESP_LOGW(TAG, "[command] 负载不是合法 JSON（%u B）⇒ 无法回执（契约要求按 id 关联）",
                 (unsigned)len);
        return;
    }

    id = cJSON_GetObjectItemCaseSensitive(doc, "id");
    if (!cJSON_IsString(id) || id->valuestring == NULL || id->valuestring[0] == '\0') {
        /* 没有 id 就没有关联依据：不回执。云侧对 `id` 为空/非法的回执也是直接丢弃
         *（`控制通路.md` §3「无回执丢弃」）⇒ 此处不回执与契约不冲突。 */
        ESP_LOGW(TAG, "[command] 负载缺可用 id（非字符串或空串）⇒ 无法回执");
        cJSON_Delete(doc);
        return;
    }

    /* 回执很短（SDK 单帧内构造），可在 SDK 回调内直接调用，不阻塞、不起任务。 */
    rc = oneye_dev_base_ack_command(id->valuestring, "unknown command");
    if (rc != ONEYE_DEV_SDK_OK) {
        /* 失败即上报不出去（未在线/队列满/超单帧）—— 只看返回值，不假装成功 */
        ESP_LOGW(TAG, "[command] command/up 回执发送失败：%s（id=%s）",
                 oneye_dev_strerror(rc), id->valuestring);
    } else {
        ESP_LOGI(TAG, "[command] 未知命令 ⇒ command/up{id=%s,status=error}", id->valuestring);
    }
    cJSON_Delete(doc);
}

static void sdk_event_cb(oneye_dev_event_t evt, const void *payload, size_t len, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "[sdk-event] %s len=%u", evt_name(evt), (unsigned)len);
    if (evt == ONEYE_DEV_SDK_EVT_TIME_SYNCED) {
        on_time_synced(payload, len);
    }
    if (evt == ONEYE_DEV_SDK_EVT_CLOUD_LINK_UP) {
        s_lcd_cloud_up = true;
        lcd_update(NULL);
        /* 链路建立（含重连）后按需发起授时请求；不在本回调内阻塞（见 request_time_sync 注释） */
        request_time_sync();
    }
    if (evt == ONEYE_DEV_SDK_EVT_CLOUD_LINK_DOWN) {
        s_lcd_cloud_up = false;
        lcd_update(NULL);
    }
    if (evt == ONEYE_DEV_EVENT_EVT_ACK) {
        /* 云端 ack：契约 §4.3 的 `data.ref` = **上行信封 id**（SDK 生成的 uuid），
         * 不是本固件的事件 id ⇒ 先按"ref 内含本地 id"匹配（台面手动 ack 路径），
         * 未命中再按**上报 FIFO** 关联（真链路路径：一次 report = 一帧 = 一 ack）。 */
        const char *payload_s = (const char *)payload;
        if (!panel_api_key_ack(payload_s, len)) {
            const char *id = key_pending_pop();
            if (id != NULL) {
                bool ok = ack_payload_is_ok(payload_s, len);
                (void)panel_api_key_ack_item(id, ok);
                ESP_LOGI(TAG, "[cloud-ack] 云端确认批次（ref=%.*s）→ 本地事件 %s ⇒ %s",
                         (int)(len < 48 ? len : 48), payload_s, id, ok ? "acked" : "failed");
            } else {
                ESP_LOGW(TAG, "[cloud-ack] 收到 ack 但无待确认本地事件（可能已确认或重启后到达）");
            }
        }
    }
    if (evt == ONEYE_DEV_SDK_EVT_COMMAND_RECEIVED && payload != NULL && len > 0) {
        /* 真机产品：解析命令 → 执行 → oneye_dev_base_ack_command(id, err)
         * 命令执行面属 S16，未落地 ⇒ 按契约对**未知命令**回 `status=error`（不是假回执）。
         * 必须回执：不回时云端 `dev_command.status` 会停在 `SENT` 直到超时。 */
        ESP_LOGI(TAG, "[sdk-event] command payload: %.*s", (int)len, (const char *)payload);
        command_ack_unknown(payload, len);
    }
}

/* ---------------------------------------------------------------- 按键 → event/up */

static const char *key_action_str(int type)
{
    switch (type) {
    case INPUT_KEY_SERVICE_ACTION_CLICK:          return "click";
    case INPUT_KEY_SERVICE_ACTION_CLICK_RELEASE:  return "click_release";
    case INPUT_KEY_SERVICE_ACTION_PRESS:          return "press";
    case INPUT_KEY_SERVICE_ACTION_PRESS_RELEASE:  return "press_release";
    default:                                      return "unknown";
    }
}

static const char *key_user_str(int user_id)
{
    switch (user_id) {
    case INPUT_KEY_USER_ID_REC:     return "rec";
    case INPUT_KEY_USER_ID_MUTE:    return "mute";
    case INPUT_KEY_USER_ID_PLAY:    return "play";
    case INPUT_KEY_USER_ID_SET:     return "set";
    case INPUT_KEY_USER_ID_MODE:    return "mode";
    case INPUT_KEY_USER_ID_VOLUP:   return "volup";
    case INPUT_KEY_USER_ID_VOLDOWN: return "voldown";
    default:                        return "unknown";
    }
}

/* ---- 云确认关联：本端已上报 item id 的 FIFO ----
 * 契约 §4.3 的 ack `data.ref` 指向**上行信封 id**，而信封 id 由 SDK 生成（uuid），不等于本固件
 * 的 `key-%05u`。因此不能靠"在 ack 负载里找子串"关联，而按"上报顺序"关联：
 * `oneye_dev_event_report()` 内部强制成帧（SDK `oneye_dev_event.c:1099`）⇒ 一次上报 = 一帧，
 * 一帧一个 ack ⇒ FIFO 弹出最旧一条即可精确对应（容量与 SDK 的 EV_PENDING_MAX 一致）。 */
#define KEY_PENDING_MAX 8
static char s_pending_ids[KEY_PENDING_MAX][24];
static int  s_pending_n;
/* 两端在不同任务：上报侧 = 按键/HTTP 任务，确认侧 = SDK 回调（网络任务）⇒ 加临界区保护 */
static portMUX_TYPE s_pending_mux = portMUX_INITIALIZER_UNLOCKED;

static void key_pending_push(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return;
    }
    portENTER_CRITICAL(&s_pending_mux);
    if (s_pending_n == KEY_PENDING_MAX) {          /* 满：丢最旧（与 SDK 覆盖最旧同策） */
        memmove(s_pending_ids[0], s_pending_ids[1], sizeof(s_pending_ids[0]) * (KEY_PENDING_MAX - 1));
        s_pending_n--;
    }
    snprintf(s_pending_ids[s_pending_n], sizeof(s_pending_ids[0]), "%s", id);
    s_pending_n++;
    portEXIT_CRITICAL(&s_pending_mux);
}

static const char *key_pending_pop(void)
{
    static char out[24];
    portENTER_CRITICAL(&s_pending_mux);
    if (s_pending_n == 0) {
        portEXIT_CRITICAL(&s_pending_mux);
        return NULL;
    }
    snprintf(out, sizeof(out), "%s", s_pending_ids[0]);
    memmove(s_pending_ids[0], s_pending_ids[1], sizeof(s_pending_ids[0]) * (KEY_PENDING_MAX - 1));
    s_pending_n--;
    portEXIT_CRITICAL(&s_pending_mux);
    return out;
}

/* ack 负载里取 code（`"code":"ok"` 子串判定足够：负载由服务端按契约生成） */
static bool ack_payload_is_ok(const char *payload, size_t len)
{
    char buf[256];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0';
    return strstr(buf, "\"code\":\"ok\"") != NULL;
}

/* 按键 → event/up 的**唯一**上报路径：物理按键与本地验证面注入共用同一函数
 * （同一幂等 id 生成、同一契约负载、同一 SDK 投递），保证"注入验证的就是真链路"。 */
static void emit_key_event(const char *key, const char *act, bool injected)
{
    /* 契约映射页 §2 行 18：按键事件走 `event/up`，data{key,action}；不为按键新开下行面 */
    char data[96];
    snprintf(data, sizeof(data), "{\"key\":\"%s\",\"action\":\"%s\"}", key, act);
    ESP_LOGI(TAG, "[key] %s/%s%s", key, act, injected ? " (注入/local-verification-only)" : "");

    /* 板载 LCD 状态屏同步（本地验证面：屏幕上看到的与面板显示同一份事实） */
    {
        char kl[32];
        snprintf(kl, sizeof(kl), "KEY %s %s", key, act);
        lcd_update(kl);
    }

    /* 本地验证面：先登记本地检测（pending），再上报；随后按 SDK 回执/云端 ack 推进状态 */
    static uint32_t s_key_id;
    char id[24];
    snprintf(id, sizeof(id), "key-%05u", (unsigned)++s_key_id);
    panel_api_key_event(key, act, id, fw_wall_ms());

    oneye_dev_event_item_t item;
    memset(&item, 0, sizeof(item));   /* 事件条目无 struct_size/api_version（非配置结构体） */
    item.id = id;                     /* 幂等 id：用于与云端 ack 的 data.ref 关联 */
    item.type = ONEYE_FW_EVENT_TYPE_DEVICE_EVENT;
    item.severity = ONEYE_DEV_SEVERITY_INFO;
    item.title = "key";
    item.data_json = data;

    oneye_dev_sdk_err_t rc = oneye_dev_event_report(&item, 1);
    if (rc == ONEYE_DEV_SDK_OK) {
        key_pending_push(id);         /* 交付成功：等待云端 ack 按 FIFO 关联 */
        panel_api_key_uplink(id, "sent");
    } else {
        panel_api_key_uplink(id, "failed");
    }
}

/* 本板按键名集合（与 panel_api.c 的 `k_panel_keys` 同源；来自 board_def.h 的
 * `INPUT_KEY_DEFAULT_INFO()`：REC/MUTE/SET/PLAY/VOLUP/VOLDOWN —— **本板无 MODE 键**）。 */
static bool key_name_valid(const char *key)
{
    static const char *names[] = { "rec", "mute", "set", "play", "volup", "voldown" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(key, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool key_action_valid(const char *act)
{
    static const char *acts[] = { "click", "click_release", "press", "press_release" };
    for (size_t i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) {
        if (strcmp(act, acts[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* 本地验证面注入回调（POST /api/simulate/key）：只做参数校验，随后走同一 emit_key_event。
 * 见 panel_api.h 的边界说明 —— 它是**验证/演示**手段，不是物理按键的替代验收证据。 */
static esp_err_t sim_key_handler(const char *key, const char *action)
{
    if (key == NULL || action == NULL || !key_name_valid(key) || !key_action_valid(action)) {
        return ESP_ERR_INVALID_ARG;
    }
    emit_key_event(key, action, true);
    return ESP_OK;
}

static esp_err_t input_key_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx)
{
    (void)handle;
    (void)ctx;
    if (evt == NULL) {
        return ESP_OK;
    }
    emit_key_event(key_user_str((int)evt->data), key_action_str((int)evt->type), false);
    return ESP_OK;
}

static void keys_start(void)
{
#if CONFIG_ONEYE_FW_ENABLE_KEYS
    if (s_periph_set == NULL) {
        return;
    }
    input_key_service_info_t key_info[] = INPUT_KEY_DEFAULT_INFO();
    input_key_service_cfg_t key_cfg = INPUT_KEY_SERVICE_DEFAULT_CONFIG();
    key_cfg.handle = s_periph_set;
    key_cfg.based_cfg.task_stack = 4 * 1024;
    periph_service_handle_t key_srv = input_key_service_create(&key_cfg);
    if (key_srv == NULL) {
        chk_ok("input_key_service_create", false, "服务创建失败");
        return;
    }
    (void)input_key_service_add_key(key_srv, key_info, INPUT_KEY_NUM);
    (void)periph_service_set_callback(key_srv, input_key_service_cb, NULL);
    ESP_LOGI(TAG, "按键服务已启动（%d 键 → event/up）", INPUT_KEY_NUM);
#endif
}

/* ---------------------------------------------------------------- Wi-Fi（配网实现见 wifi_prov.c） */

/* 定义在文件后部（上云入口 / 面板链路快照同步），此处前置声明供 wifi_prov_boot() 调用 */
static void oneye_start(void);
static void panel_sync_task(void *arg);
static void panel_start_if_enabled(void);
/* 常驻上行维护循环：**不是**独立任务 —— 就地跑在 cloud_start_task 里。
 * 它一个循环同时驱动三条产出方：track 埋点（`track/up`）、影子整帧补发（`shadow/up`）、
 * log 取证插桩（`log/up` 周期流量）。栈/内部 RAM 约束见「埋点 → track/up」段注释。 */
static void uplink_maintain_run(void);

/* ---------------------------------------------------------------- 资源读数（`[res]`）
 *
 * ★★ 2026-10-07（第三批）：这一读数**原来只有一处、且在入网之后才可达** ——
 *   它写在 `cloud_start_task()` 里，而该任务由 `on_wifi_ready()`（GOT_IP 之后）创建。
 *   后果（真机实证，不是推断）：BLE 机型**未入网**时这一行**永远打不出来** ——
 *   上一批 BLE 件 120 s 抓包里 `[res]` 命中 0 行（同段其余启动行都在场），
 *   于是"12 KB 的 NimBLE 主机栈起来之后内部 RAM 还剩多少"**没有读数**。
 *
 * 本函数把同一组量做成**带阶段标签**的一次性读数，在多个**不依赖入网**的必经点各打一行：
 *   · `phase=pre-prov…`   —— `app_main()` 里外设/LCD/摄像头/SD/AEC/回放都已就绪、配网与 NimBLE **之前**
 *     （两种机型都在同一位置打 ⇒ 跨机型同阶段可比）；
 *   · `phase=ble-ready…`  —— `wifi_prov_boot()` 里 BLE 通道**已起栈并开始广播、尚未入网**
 *     （BLE 机型的 NimBLE 后读数；与上面那行同机相减 = NimBLE 主机栈的净代价）；
 *   · `phase=cloud-start…` —— `cloud_start_task()` 里（= **原有的**那一处，位置与三个量逐字保留，
 *     只加了 phase 标签）⇒ 入网之后、SDK 起来之后的量，仍可读，⛔ 未被删掉。
 *
 * 关键量是 `largest_internal_block`（`MALLOC_CAP_INTERNAL` 的**最大连续块**）：这台板子历史实测
 *   只有 2304 B，而任何 ≥16 KB 的新 FreeRTOS 任务栈都要"连续"内部块（见 `cloud_start_task()` 注释）。
 *   它在**入网前**的量才决定"BLE 机型还能不能再起任务"。
 *
 * ⛔ 不许为此新增常驻任务/定时器：本函数就地调用（单次、只读 `heap_caps_*`、不阻塞、不分配）。
 * ⛔ 不许把 `phase` 去掉：多处读数没有阶段标签就分不清"哪一刻的量"，等于把上一批那个缺口换个形状。
 * ⛔ 不许把 `cloud-start（入网后）` 那一处删掉（门禁 A13d 断言三处都在）。 */
static void res_report(const char *phase)
{
    ESP_LOGI(TAG, "[res] phase=%s free_internal=%u B / largest_internal_block=%u B / free_psram=%u B",
             phase,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* 联网就绪 → 启动上云。放在独立任务里跑（oneye_start 需较大栈；事件任务只置位）。
 * 回调会随重连反复触发，故用一次性标志保证 SDK 只启动一次；面板启动本身幂等。 */
static void cloud_start_task(void *arg)
{
    (void)arg;
    oneye_start();

    /* ⚠️ 这一处原来是 `(void)xTaskCreate(...)` —— 创建失败时**零告警**，串口上"没跑"与"没写"完全同形。
     * 2026-09-30 真机实测就是被这一点掩盖的：`log_probe_task`（3072 words）根本没起来，
     * 而板上没有任何一行能证明它失败（`log-probe` 命中 0 行）。凡创建任务，返回值一律接出来告警。 */
    if (xTaskCreate(panel_sync_task, "panel_sync", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "面板同步任务创建失败（内存不足）⇒ 面板链路快照不会刷新");
    }

    /* ★ 2026-09-30（本批）：**其余三处一律不再新建任务**。
     * 真机实测（`_tmp-phase2/device-trackup-fix.md` §1、`_tmp-phase2/serial-trackup-proof2.txt:243-245`）：
     * `track`（4096 words）与 `shadow_reassert`（4096 words）都 `pdPASS` 失败，`log_probe`（3072 words）同样；
     * 同一刻板子自己量出来 `largest_internal_block=2304 B` ⇒ 内部 RAM **最大连续块**只有 2.2 KB，
     * 而一个 4096 words 的 FreeRTOS 栈要的是 **16 KB 连续内部块**，结构上不可能成功。
     * 所以三条产出方（track 埋点 / 影子补发 / log 取证）现在共用**本任务**这 6144 words 栈：
     * 见 uplink_maintain_run() 与 shadow_reassert_step() / log_probe_step()。 */

    /* 资源读数：把「16 KB 栈在这台板子上要不到」从**推断**变成**测量**（这正是上一版 track 任务失败的根因）。
     * ★ 2026-10-07（第三批）：**本处读数逐字保留**（同位置、同三个量），只把日志改成经 `res_report()`
     *   打出的带阶段标签版本 `phase=cloud-start（入网后）` ⇒ 与"入网前"那几个阶段可对照。
     *   ⛔ 不许把这一处删掉换取别处读数：入网后/未入网两个时刻要能同时看到才有对照。 */
    res_report("cloud-start（入网后）");

    /* 上行维护（track 埋点 + 影子补发 + log 取证）：**就地跑在本任务里**，一个新任务都不建 ——
     * 理由见上一条与「埋点 → track/up」段注释。本任务因此不再 `vTaskDelete(NULL)`：它转为这三条面的
     * 常驻维护者（1 s 一拍，多数拍只读计数/判链路）。栈是**已经分配**的 6144 words（本函数的创建点
     * `on_wifi_ready()`），常驻不抬高内存峰值；而"再建一个 4096 words 的任务"正是这台板子上实测失败的那一步。
     * uplink_maintain_run() 不返回。 */
    uplink_maintain_run();
}

/* ★★ 2026-10-07（本批）：入网之后的**启动期动作**全部收进本函数，顺序逐字固定为
 *   ① 入网日志 → ② net_probe 自检 → ③ SNTP 授时 → ④ 本地面板 → ⑤ cloud_start_task（→ oneye_start）
 *
 * 为什么 ③ 必须在 ⑤（cloud_start_task）之前：板子没有 RTC，未授时时系统时间是 1970，而服务端
 *   证书的 notBefore 落在未来 ⇒ 严格 TLS 下 mbedtls 必然报 `BADCERT_FUTURE`（`sntp_boot.h:2-11`
 *   逐字）。生产口径是**严格 TLS**（`CONFIG_ONEYE_FW_TLS_INSECURE=n`），所以"先有钟、再建链"
 *   是硬顺序；而运行期 `cloud_ts` 授时要等 `CLOUD_LINK_UP` 才来 ⇒ 不在这里补钟可能构成
 *   bootstrap 死锁（连不上 ⇒ 拿不到云端时间 ⇒ 更连不上）。
 *
 * ⚠️ 本函数在**系统事件任务**上下文执行（`wifi_prov.h` 的 ready 回调口径），因此 ②③ 是**有界的
 *   同步等待**：net_probe 最多 3×3 s（三个探针各自 3 s 超时，`net_probe.c:102`）、sntp 最多
 *   2×10 s（两台服务器各 10 s，`sntp_boot.c:15/34/41`）；真机正常路径实测分别 ≈71 ms 与 ≈1 s。
 *   之所以接受这段阻塞：它只发生在**首次入网**（下方一次性守卫），此刻 Wi-Fi 已拿到 IP、SDK 尚未
 *   建链；代价是这段时间内系统事件任务不处理新事件（Wi-Fi 断开事件会延后到返回后才被处理）。
 *   ⛔ 不要为此"再建一个任务"：这台板子上任何 ≥16 KB 的新任务都实测失败
 *   （`largest_internal_block=2304 B`，见 `cloud_start_task()` 注释）。
 *
 * ⛔ 也不要把 ②③ 降级成"只在某条通道里做"：那正是本批修掉的旧结构缺陷（见下方
 *   `prov_boot_after_connect()` 的旧结构留痕）。门禁 A11/A12 机械拒绝这两种回退。 */
static void on_wifi_ready(void)
{
    /* ★ 收敛点的下游（ADR-0017 D2）：**无论凭据来自哪条通道**（C1/C2 BLE、C3 凭据文件、
     *   C4 台面 API、C5 Kconfig），都在这一处进入上云链路。通道侧不需要、也不允许知道这里做什么。
     *   ⛔ 不要在这里按通道分叉；⛔ 也不要把本函数改造成"每通道一份"。 */
    wifi_prov_ble_stop_advertising();     /* 已入网 ⇒ 停 BLE 广播（幂等，重连时重复调无害） */

    static bool started;
    if (started) {
        return;
    }
    started = true;

    /* ① 入网日志（**每条通道**都在这里留一行）。此前只有 C3/C5 走 prov_boot_after_connect() 时才有
     *    这一行；wifi_prov.c 的 GOT_IP 处理器已打过一行带 IP 的，这一行补齐"来源通道"的应用侧视角。 */
    ESP_LOGI(TAG, "Wi-Fi 已连接：ip=%s（ssid=%s，来源 %s）",
             wifi_prov_ip(), wifi_prov_ssid(), wifi_prov_source());

    /* ② 板级网络自检（只读、带 errno，**不改变任何连接行为**）：把"没路由 / 网关不通 / 上行丢包"
     *    三种同形故障区分开。放在建链之前 ⇒ 日志顺序即"联网 → 自检 → 授时 → 上云"。 */
    net_probe_report(CONFIG_ONEYE_FW_CLOUD_HOST, (unsigned)CONFIG_ONEYE_FW_CLOUD_PORT);

    /* ③ 启动期授时：**严格 TLS 校验的前置条件**（理由见本函数头）。失败不阻塞上云 —— 此时若严格
     *    校验过不去，是**故意**的可见失败，而不是静默降级（`sntp_boot.h:9-11`）。 */
    (void)sntp_boot_sync();

    /* ④ 本地验证面与云端链路解耦：拿到 IP 就起（幂等；掉线重连后拿到 IP 也能补起） */
    panel_start_if_enabled();

    /* ⑤ 上云：**唯一**那个任务仍只在这里建（门禁 A2） */
    if (xTaskCreate(cloud_start_task, "cloud_start", 6144, NULL, 4, NULL) != pdPASS) {
        started = false;
        ESP_LOGE(TAG, "上云任务创建失败（内存不足）");
        return;
    }
    ESP_LOGI(TAG, "已联网 → 启动上云（oneye-dev-sdk）");
}

/** 影子状态的**整帧**：boot 发一次，云链路上线（down → up）时再补发一次。
 *  没有周期重报 —— 见 shadow_reassert_step() 的说明。 */
static char s_shadow_state[192];       /* 整帧：firmwareVersion + power + credSource */
static bool s_shadow_state_valid;

/* 上一轮读到的云链路在线状态，用于识别**重连**（down → up）。
 * 初值 true：boot 那一帧就当链路当时可用 —— 链路不可用时 SDK 会自己把待发帧的入队时刻
 * 刷新到 link up 那一刻（`internal/oneye_internal.c` 的 bint_on_link_up），开机帧不会因断链而过期。 */
static bool s_shadow_link_was_up = true;

/** 影子**补发**判定：事件驱动，**只在云链路 down → up 时补发整帧**；没有周期重报。
 *
 * 为什么去掉周期重报（2026-09-25，R80 选项 (iv)）：
 *   · 量级：整帧 60 s 一次 ⇒ ≈1440 帧/天/设备，而登记的容量假设是 `fallbackPeriodSec=300`
 *     ⇒ 288 帧/天/设备（云端 TSL `OY-SPK-01.json` 的 `defaults.reportStrategy`），相差 5×；
 *     云端**没有任何代码读 `reportStrategy`** ⇒ 这 5× 只能在设备侧收窄。
 *   · 只收窄**载荷**（改报身份子集）**解决不了**这 5×（2026-09-25 实测确认）：帧数一帧没少，
 *     且子集里仍然带 `credSource` **这一列**，帧照样进 Kafka/TDengine。要真消掉它只能去掉周期。
 *   · 为什么少掉这一遍重述也还能接受：链路恢复时 SDK 自己就把各面「已入队、还没发出去」的帧
 *     的入队时刻刷新了一遍，断链期间不计入 TTL，随后照常 drain 出去
 *     （`internal/oneye_internal.c:756-764`，2026-09-24 为更早那次影子丢失加的）。
 *     所以「开机帧因断链过期被静默清掉」这一类**断链**故障本就有兜底，周期重报在它之上是冗余的。
 *
 * ⚠️ **已接受的代价（不是疏漏）**：一次上报丢了、而**原因不是链路掉线**时 —— 例如帧已经
 * publish 出去、却在链路对端/broker 侧被丢掉，而链路自己从未 down 过 —— 现在**不再自愈**：
 * 没有周期重报就没有第二次机会，只能等下一次真正的 `down → up`，或设备重启。`credSource`
 * 这类**状态**因此可能在一段时间内对服务端不可见。这是 R80 (iv) 明确接受的权衡
 * （拿「帧量 5×」换「这一小类丢失」），不是遗漏。若日后要重新拿回自愈能力，应当另立一条
 * **低频**兜底，而不是退回 60 s 周期。
 *
 * 整帧仍在 **boot** 发（见 oneye_start 第 6 步），并在这里的**重连**时补发一次，把 `power`
 * 这类非身份键也带回；补发放在**本函数**里而不是 `sdk_event_cb`（回调由 SDK 事件任务调用，
 * 在回调内再回调 SDK 属重入）。
 *
 * ★★ 2026-09-30 真机修正：本函数**不再是一个任务** ★★
 *
 * 第一版把它写成 `xTaskCreate(shadow_reassert_task, "shadow_reassert", 4096, …)`（4096 words = 16 KB 内部 RAM 栈）。
 * 真机两轮串口取证（`_tmp-phase2/serial-korvo2-proof*.txt`、`_tmp-phase2/serial-trackup-proof2.txt:243`）实测
 * `pdPASS` **失败**，只留一行 `影子补发任务创建失败（内存不足）—— 链路上线后不会补发整帧`；本批收紧前
 * （`device-trackup-fix.md` §9-1）它一直是"仍然起不来"的欠账。
 * 同一刻板子自报 `largest_internal_block=2304 B`（`serial-trackup-proof2.txt:245`）⇒ 16 KB **连续**内部块要不到。
 *
 * 修法与 `track/up` 面**同一条**：不新建任务，**就地并入**已经在跑的 `cloud_start_task`（6144 words，
 * 创建点 `on_wifi_ready()`，真机确认成功）。这里拆成 `shadow_reassert_step()`——**每次只做一次判定、立即返回**，
 * 由 `uplink_maintain_run()` 每秒调用。为什么能绕开内存约束：它跑在**已经分配**的 6144 words 栈上，
 * 不申请任何新栈；判定逻辑（`down → up` + 状态有效 ⇒ 重发整帧）与原任务循环体**逐行相同**，只是换了宿主。
 * ⚠️ 因此**不许**为了"看起来更干净"再把它变回独立任务：这台上任何 ≥16 KB 的新任务都会重现同一次失败。 */
static void shadow_reassert_step(bool link_up)
{
    if (link_up && !s_shadow_link_was_up && s_shadow_state_valid) {
        oneye_dev_sdk_err_t frc = oneye_dev_base_report_state(s_shadow_state);
        ESP_LOGI(TAG, "影子整帧补发（链路上线）：%s", oneye_dev_strerror(frc));
    }
    s_shadow_link_was_up = link_up;
}

/** 启动本地验证面（需已联网获得 IP；与云端链路是否可用无关） */
static void panel_start_if_enabled(void)
{
#if CONFIG_ONEYE_FW_ENABLE_PANEL_API
    panel_api_set_wifi(wifi_prov_is_connected(), wifi_prov_ip(), wifi_prov_ssid(), wifi_prov_source());
    if (!wifi_prov_is_connected()) {
        ESP_LOGW(TAG, "未联网 → 本地验证面板未启动（/api/* 需设备 IP 才可访问）");
        return;
    }
    if (panel_api_start(0) == ESP_OK) {
        ESP_LOGI(TAG, "本地验证面板：http://%s/api/status（宿主 tools/panel/panel.py 亦可聚合）",
                 wifi_prov_ip());
    } else {
        ESP_LOGE(TAG, "本地验证面板启动失败（端口 %d 被占用？）", CONFIG_ONEYE_FW_PANEL_PORT);
    }
#else
    ESP_LOGI(TAG, "本地验证面板已按 Kconfig 关闭（ONEYE_FW_ENABLE_PANEL_API=n）");
#endif
}

/* ------------------------------------------------ 旧结构留痕：prov_boot_after_connect() 已删除
 *
 * 2026-10-07 之前，入网后的「启动自检 + 授时 + 面板」写在一个叫 `prov_boot_after_connect(src, ssid)`
 * 的辅助函数里（`static void prov_boot_after_connect(const char *src, const char *ssid)`），而它
 * **只被 C3（SD/SPIFFS 凭据文件）与 C5（Kconfig 兜底）的成功路径调用**（旧调用点 `:822` / `:859`）。
 * 后果：**BLE 通道（C1/C2）入网后根本不跑 net_probe 与 SNTP**。
 *
 * 为什么那是缺陷（不是风格问题）：
 *   · `sntp_boot.h:2` 逐字 —— "设备侧 TLS 严格校验必须先有时间"。BLE 配网的机器若跳过授时，
 *     在**严格 TLS**（生产口径）下可能直接连不上；而运行期 `cloud_ts` 授时要等 `CLOUD_LINK_UP`
 *     驱动 ⇒ **bootstrap 死锁**（连不上 ⇒ 拿不到云端时间 ⇒ 更连不上）。这是**真风险**；
 *   · `net_probe_report` 按 `net_probe.h:2` 只是取证（不改变连接行为），跳过它只丢诊断证据；
 *   · 更直接的是：它与本文件 `wifi_prov_boot()` 上方那段注释**自相矛盾** —— 那里逐字写着
 *     "入网成功之后（面板启动 / net_probe / 授时 / 上云 / 影子 / 埋点）**全部**收口在
 *     `on_wifi_ready()` …… **所有通道共用那一条路径**，这是本设计唯一的不变量（ADR-0017 D2）"。
 *
 * 本批按那一条不变量收口：三步都搬进 `on_wifi_ready()` 的收敛点下游（顺序与理由见该函数头），
 * 于是**所有通道**（含将来新增的声波 / 二维码）都必经它们。通道侧仍旧只负责交凭据。
 * ⛔ 因此**不要**在这里重新长出第二个"入网后动作"函数，也⛔ **不要**在通道模块里直接调
 *    `net_probe_report()` / `sntp_boot_sync()`：门禁 `tools/check-prov-convergence.py` 的
 *    A11/A12 会机械拒绝「通道模块里出现这两步」与「把授时挪到 cloud_start_task 之后」。
 * ⚠️ 诚实边界：这条收敛修复**尚未在 BLE 机型真机上验证过端到端配网**（无同场手机）。 */

/** 配网启动：按 ADR-0017 D1/D2 的**优先级**依次尝试；没有任何通道可就地（按机型）把下一步讲清楚。
 *
 * 尝试顺序 = 优先级顺序（用户 2026-10-07 口径：**SD 卡 WiFi 凭据 > 蓝牙配网 > 其他**）：
 *   C3 SD 卡 / SPIFFS 凭据文件（`ONEYE_FW_ENABLE_WIFI_FILE`，**机型能力位**）
 *     → C1/C2 BLE 交互式通道（`ONEYE_FW_ENABLE_BLE_PROV`）
 *       → C5 Kconfig 兜底 SSID（**不是通道**，属「其他」层，故排在 BLE 之后）
 *         → 都没有/都没成 ⇒ 报"本次启动不会上云"并按**本机型做得到的方式**给下一步
 *
 * ⚠️ 这个顺序不是装饰：真正决定"谁被采信"的是 `wifi_prov_submit_credentials*()` 里的
 *    `wifi_prov_policy_decide()`（SD rank 0 > BLE rank 1 > 其他 rank 2），本函数只是把
 *    高优先级的通道**先递上去**。C3 解析成功但**本次入网失败**时，这里**显式**
 *    `wifi_prov_release_held()` 释放采信槽再降级 —— 否则判据会把后续通道全挡住。
 *
 * ⛔ 本函数**不做任何「入网后」动作**。入网成功之后（面板启动 / net_probe / 授时 / 上云 / 影子 /
 *    埋点）全部收口在 `on_wifi_ready()` —— 由 `wifi_prov.c` 的 `IP_EVENT_STA_GOT_IP` 处理器
 *    经 ready 回调触发。**所有通道共用那一条路径**，这是本设计唯一的不变量（ADR-0017 D2）。
 * ⛔ 新增通道（含预留的 R1 声波 / R2 二维码）时：在 `wifi_prov_policy.h` 登记枚举与 rank →
 *    在通道模块里调 `wifi_prov_submit_credentials()` → **不要**在这里加"入网后分支"。
 */
static void wifi_prov_boot(void)
{
    char ssid[WIFI_PROV_SSID_MAX] = { 0 };
    char pass[WIFI_PROV_PASS_MAX] = { 0 };
    char src[64] = { 0 };

#if CONFIG_ONEYE_FW_ENABLE_WIFI_FILE
    /* C3（**rank 0，最高**）：SD 卡根目录 oneye-wifi.txt（用户投放）；SD 未挂载时自动试 SPIFFS 同名文件。
     * ★ `ONEYE_FW_ENABLE_WIFI_FILE` 是**机型能力位**：有卡槽机型 y / 无卡槽机型 n。
     *   ⛔ 它**不是**「量产 vs 台面」开关 —— 旧口径把它当量产开关，后果是纯生产件一个通道都不剩、
     *   `oneye_start()` 永不调用、设备上不了云（2026-10-07 真机实测；见 ADR-0017 D3）。 */
    if (wifi_prov_load_file(ssid, sizeof(ssid), pass, sizeof(pass), src, sizeof(src)) == ESP_OK &&
        ssid[0] != '\0') {
        if (wifi_prov_submit_credentials_src(ssid, pass, WIFI_PROV_CHAN_FILE, src) == ESP_OK) {
            /* 入网后的动作（日志 / 自检 / 授时 / 面板 / 上云）全部在收敛点下游
             * （on_wifi_ready → cloud_start_task）—— 这里只结束启动期通道分派。 */
            return;
        }
        /* ★ 卡上有凭据、但**本次入网失败** ⇒ 显式释放采信槽再降级。理由：优先级判据管的是
         *   "多通道都可得时谁先被采信"，不是"已经失败的通道继续霸位"；不释放的话 rank 0 会一直占着
         *   槽位，把 BLE 与兜底通道全部拒掉，设备只能反复重试那份连不上的旧凭据（配网体验死锁）。 */
        ESP_LOGW(TAG, "SD 卡凭据未能入网（来源 %s，ssid=%s）⇒ 释放采信槽，降级到其它通道", src, ssid);
        wifi_prov_release_held("file-connect-failed");
        ssid[0] = '\0';
        pass[0] = '\0';
        src[0] = '\0';
    }
#else
    ESP_LOGI(TAG, "本机型未启用 SD 卡/SPIFFS 凭据文件配网（ONEYE_FW_ENABLE_WIFI_FILE=n）"
                  "——该位是机型能力位（无卡槽机型），不是量产开关");
#endif

    /* ---------------- C1/C2 BLE（**rank 1**，排在「其他」层之前） ----------------
     * 起交互式通道，「还没有凭据」不等于「上不了云」：等通道把凭据交进来，交进来之后走的是
     * 同一条收敛点（wifi_prov.c 的 GOT_IP → s_ready_cb）。 */
#if CONFIG_ONEYE_FW_ENABLE_BLE_PROV
    ESP_LOGI(TAG, "启动 BLE 配网通道 C1/C2（手机 App 或另一台嵌入式设备经 BLE 下发 SSID/密码）");
    if (wifi_prov_ble_start() == ESP_OK) {
        ESP_LOGW(TAG, "下一步：手机 App 连接 ONEYE-<设备 id 后 4 位>，"
                      "输入串口打印的 6 位配对码（POP）后下发 Wi-Fi 凭据");
        /* ★ 2026-10-07（第三批）：**NimBLE 主机栈已起来（12 KB，
         *   `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE 12288`）、已开始广播、而还没有入网**的这一刻，
         *   把内部 RAM 的三个量打出来 —— 这正是 BLE 机型此前**没有读数**的那一个时刻
         *   （入网后那处 `phase=cloud-start…` 本机型走不到）。与本函数上方/`app_main()` 里的
         *   `phase=pre-prov…`（同一块板、外设已就绪、NimBLE 之前）相减即 NimBLE 的净代价。 */
        res_report("ble-ready（NimBLE 已起栈并广播、未入网）");
        panel_start_if_enabled();     /* 无 IP ⇒ 提示并直接返回 */
        return;                       /* 等通道提交凭据；入网后由收敛点统一触发上云 */
    }
    ESP_LOGE(TAG, "BLE 配网通道启动失败 ⇒ 继续尝试 Kconfig 兜底常量（若有）");
#endif

    /* ---------------- C5 Kconfig 兜底常量（**rank 2**，「其他」层） ----------------
     * ⚠️ 它**不是通道**（非交互式、编译期常量），口径上属「其他」，故排在 BLE **之后**：
     *    有 BLE 可用时不该由常量抢先入网。生产件里 `CONFIG_ONEYE_FW_WIFI_SSID` 通常为空。 */
    if (CONFIG_ONEYE_FW_WIFI_SSID[0] != '\0') {
        if (wifi_prov_submit_credentials_src(CONFIG_ONEYE_FW_WIFI_SSID, CONFIG_ONEYE_FW_WIFI_PASSWORD,
                                             WIFI_PROV_CHAN_KCONFIG, "kconfig") == ESP_OK) {
            /* 同 C3：入网后动作归收敛点下游，此处不重复做任何一步。 */
            return;
        }
        ESP_LOGE(TAG, "Kconfig 兜底 SSID 未能入网（ssid=%s）", CONFIG_ONEYE_FW_WIFI_SSID);
    }

    /* ---------------- 没有任何可用通道（运行期） ---------------- */
    ESP_LOGW(TAG, "未找到可用的 Wi-Fi 凭据 → 本次启动不会上云；板级自检/按键/录音/本地回放继续运行");
    /* ★ 2026-10-07 修正：这里**原来**无条件叫用户"把 oneye-wifi.txt 放 SD 卡根目录后复位"——
     *   而该分支在 `ONEYE_FW_ENABLE_WIFI_FILE=n` 时同样会走到，于是给出了一条**做不到**的指引
     *   （让人去插一张本机型根本不读的卡）。改为按**本机型实际具备的通道**讲下一步。 */
#if CONFIG_ONEYE_FW_ENABLE_WIFI_FILE
    ESP_LOGW(TAG, "下一步：本机型支持 SD 卡配网 —— 把 oneye-wifi.txt（内容 ssid=… 与 password=…）"
                  "放 SD 卡根目录后复位设备");
#elif CONFIG_ONEYE_FW_ENABLE_BLE_PROV
    ESP_LOGW(TAG, "下一步：本机型**不支持** SD 卡配网（ONEYE_FW_ENABLE_WIFI_FILE=n）"
                  "⇒ 请用 BLE 配网：手机 App 连接 ONEYE-<设备 id 后 4 位>，"
                  "输入串口打印的 6 位配对码（POP）后下发 Wi-Fi 凭据");
#else
    ESP_LOGE(TAG, "本机型既无 SD 卡配网能力（ONEYE_FW_ENABLE_WIFI_FILE=n）也无 BLE 通道"
                  "（ONEYE_FW_ENABLE_BLE_PROV=n）⇒ 属于配置错误，请修正机型 defaults（ADR-0017 D3）");
#endif
    panel_start_if_enabled();     /* 无 IP ⇒ 提示并直接返回 */
}

/* ---------------------------------------------------------------- oneye 上云 */

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

static void oneye_start(void)
{
    oneye_dev_base_config_t base_cfg;
    oneye_dev_log_config_t log_cfg;
    oneye_dev_event_config_t ev_cfg;
    oneye_dev_caps_t caps;
    oneye_dev_sdk_err_t rc;

    ESP_LOGI(TAG, "oneye-dev-sdk %s（base/mpp/event/log）", oneye_dev_version());

    /* 1) base：身份 + 端点（全部来自 Kconfig，不硬编码） */
    memset(&base_cfg, 0, sizeof(base_cfg));
    ONEYE_DEV_STRUCT_INIT(base_cfg);
    oneye_dev_base_config_defaults(&base_cfg);
    base_cfg.device_id = CONFIG_ONEYE_FW_DEVICE_ID;
    base_cfg.username = CONFIG_ONEYE_FW_DEVICE_ID;   /* EMQX ACL 用 ${username} 取它 */
    base_cfg.client_id = CONFIG_ONEYE_FW_DEVICE_ID;
    base_cfg.cloud_host = CONFIG_ONEYE_FW_CLOUD_HOST;
    base_cfg.cloud_port = (uint16_t)CONFIG_ONEYE_FW_CLOUD_PORT;
    base_cfg.cloud_ws_path = (CONFIG_ONEYE_FW_CLOUD_WS_PATH[0] != '\0') ? CONFIG_ONEYE_FW_CLOUD_WS_PATH : NULL;
    base_cfg.transport = fw_transport();
    base_cfg.token = (CONFIG_ONEYE_FW_CLOUD_TOKEN[0] != '\0') ? CONFIG_ONEYE_FW_CLOUD_TOKEN : NULL;

    /* 1.5) 凭据来源：**creds 分区优先**（量产口径：通用固件 + 产线写一次凭证）。
     *      2026-09-22 起这段逻辑**搬进了 SDK**（`oneye_dev_creds_*`）：伙伴只集成 SDK，不该每家
     *      自己实现一遍 —— 而各家实现最容易在"失败姿态"上走样（把"分区坏了"写成"读不到就回退"，
     *      就会让一台错机器拿公用身份上线，平台会当合法设备接受它）。
     *      契约：backend/contracts/domain/设备凭据分区与产测自证契约.md（§2 失败姿态冻结）。
     *      三种情形：
     *        · 有合法镜像 ⇒ 用分区里的 node_id/证书/私钥/CA（**不再用编译期常量**，否则"固件通用"不成立）；
     *        · **坏了**（CORRUPT/IO）⇒ 一律不联网，且**不回退**；
     *        · **没写过**（NOT_FOUND）⇒ 只有这一种情形允许回退（台面方便）；量产固件应让
     *          `ONEYE_DEV_CREDS_REQUIRED=y` 把它也变成"不联网"。
     */
    static oneye_dev_creds_t s_creds;
    /* 本机实际用的是**哪一份身份**（P0-A：必须让服务端看得见）。
     * 台面/漏写分区的机器允许回退到编译进固件的**公用**凭据，但"看不清有没有回退"不可以：
     * 一台漏写分区的机器拿公用身份上线，平台上若与正常机器看不出区别，它会被一直当好设备用下去。
     * 故这里记下来源，开机与其它状态一起报进影子 `reported` 的 `credSource`。 */
    oneye_dev_creds_source_t cred_src = ONEYE_DEV_CREDS_SOURCE_UNKNOWN;
    oneye_dev_sdk_err_t creds_rc = oneye_dev_creds_load(&s_creds);
    if (creds_rc == ONEYE_DEV_SDK_OK) {
        if (oneye_dev_creds_apply(&base_cfg, &s_creds) != ONEYE_DEV_SDK_OK) {
            ESP_LOGE(TAG, "凭据写入 base 配置失败（缺私钥/身份不合法）—— 不联网");
            return;
        }
        cred_src = ONEYE_DEV_CREDS_SOURCE_PARTITION;
        ESP_LOGI(TAG, "凭据来源：**creds 分区** node=%s（cert %u B / key %u B / CA %u B）crc32=%08x %s",
                 s_creds.node_id, (unsigned)s_creds.cert_len, (unsigned)s_creds.key_len,
                 (unsigned)s_creds.ca_len, (unsigned)s_creds.image_crc32,
                 (s_creds.mac && s_creds.mac[0]) ? s_creds.mac : "");
#if CONFIG_ONEYE_FW_PROV_ATTEST
        /*
         * 产测自证（契约 §6）：用**分区里那把私钥**签一段规范化文本，产线上位机用这台设备的证书验签。
         * 为什么要这一行：伙伴产线的凭证分区在哪个**偏移**我们不确定，"回读一段 flash 再比对"这条路
         * 走不通；而且回读只证明"flash 里有这份字节"，不证明"这台设备真的能用它上线"。
         *
         * ⚠️ 这一行**不带 ESP_LOG 前缀**（用 printf）：上位机要按 `ONEYE-PROV1 ` 开头的整行解析，
         *    带时间戳前缀会逼工具去做"猜前缀"的模糊匹配 —— 那种宽松解析迟早会放过错的证据。
         */
        {
            static char s_attest[ONEYE_DEV_CREDS_ATTEST_MAX];
            if (oneye_dev_creds_attest(&s_creds, s_attest, sizeof(s_attest)) == ONEYE_DEV_SDK_OK) {
                printf("%s\n", s_attest);
                fflush(stdout);
            } else {
                ESP_LOGE(TAG, "产测自证生成失败（分区里的证书/私钥不可用）—— 这台设备**不得**被判为 PASS");
            }
        }
#endif
    } else if (!oneye_dev_creds_is_absent(creds_rc)) {
        ESP_LOGE(TAG, "凭据不可用：%s", oneye_dev_strerror(creds_rc));
        ESP_LOGE(TAG, "**不联网**（不只不重试）：分区坏了却回退内嵌证书 = 拿公用凭据冒充这台设备");
        return;
    } else {
#if ONEYE_DEV_CREDS_REQUIRED_ACTIVE
        /* 量产口径：这台机器没有身份，就不上线。**不许**退回一份公用凭据。 */
        ESP_LOGE(TAG, "creds 分区没写过（%s），且本构建要求必须有分区凭据 —— 不联网，送产线重新写入",
                 oneye_dev_strerror(creds_rc));
        return;
#else
        ESP_LOGW(TAG, "creds 分区为空（%s）—— 回退到编译进固件的凭据（仅开发/产测可接受）",
                 oneye_dev_strerror(creds_rc));
#if defined(ONEYE_FW_EMBED_CERTS)
    /* 走到这里 = 用的是**公用**身份（固件里那一份），必须让云端看得见（见 cred_src 的说明）。 */
    cred_src = ONEYE_DEV_CREDS_SOURCE_EMBEDDED;
    /*
     * 一机一密 mTLS：设备证书/私钥/CA 由构建期嵌入（见 main/CMakeLists.txt 顶部说明；
     * 私钥不入库，目录由 -DONEYE_FW_CERT_DIR= 指定）。objcopy 生成的 blob 末尾带一个 NUL，
     * 这里裁掉，避免把 NUL 一并交给 mbedtls 解析。
     */
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

        if (ca_len  > 0 && oneye_cert_ca_start[ca_len - 1]   == '\0') { ca_len--; }
        if (crt_len > 0 && oneye_cert_crt_start[crt_len - 1] == '\0') { crt_len--; }
        if (key_len > 0 && oneye_cert_key_start[key_len - 1] == '\0') { key_len--; }

        base_cfg.credential         = oneye_cert_crt_start;
        base_cfg.credential_len     = crt_len;
        base_cfg.credential_key     = oneye_cert_key_start;
        base_cfg.credential_key_len = key_len;
        base_cfg.tls_ca_pem         = oneye_cert_ca_start;
        base_cfg.tls_ca_pem_len     = ca_len;

        ESP_LOGI(TAG, "一机一密：已嵌入设备证书（client %u B / key %u B / CA %u B）",
                 (unsigned)crt_len, (unsigned)key_len, (unsigned)ca_len);
    }
#else
        ESP_LOGE(TAG, "no creds partition and no embedded certificates — this device has no identity, not connecting");
        return;
#endif
#endif /* ONEYE_DEV_CREDS_REQUIRED_ACTIVE */
    }

#if CONFIG_ONEYE_FW_TLS_INSECURE
    base_cfg.tls_insecure_skip_verify = true;    /* dev/产测；生产必须 n 并投放自研 CA */
#else
    base_cfg.tls_insecure_skip_verify = false;
#endif
    base_cfg.enable_status_topic = true;   /* status/up（retained + 遗嘱） */
    base_cfg.event_cb = sdk_event_cb;

    rc = oneye_dev_base_init(&base_cfg);
    if (rc != ONEYE_DEV_SDK_OK) {
        ESP_LOGE(TAG, "base init 失败：%s", oneye_dev_strerror(rc));
        return;
    }

    /* 2) 能力集：严格口径（默认不声明任何能力位） */
    memset(&caps, 0, sizeof(caps));
    ONEYE_DEV_STRUCT_INIT(caps);
#if CONFIG_ONEYE_FW_DECLARE_VIDEO_LIVE
    caps.caps = (uint32_t)ONEYE_DEV_CAP_VIDEO_LIVE;
#else
    caps.caps = (uint32_t)ONEYE_DEV_CAP_NONE;
#endif
    caps.model_version = "1";
    caps.fw_version = ONEYE_FW_VERSION;
    (void)oneye_dev_base_set_caps(&caps);

    /* 3) log（自检结论走这里，不新造通道） */
    memset(&log_cfg, 0, sizeof(log_cfg));
    ONEYE_DEV_STRUCT_INIT(log_cfg);
    log_cfg.level = ONEYE_DEV_LOG_LEVEL_INFO;
    log_cfg.uplink_enabled = true;
    /* ★ 取证插桩（2026-09-21，Kconfig 默认关）：把批量间隔拉长、并关掉"条数早发"，让缓冲里的
     *   记录**停得住** —— 否则 1 s 就发走了，`log/down dump` 的窗口语义在线上无从观测。
     *   缺省值是 1000 ms / 8 条。取证结论见 backend/contracts/api/mqtt/传输规范.md §8.1。 */
#if CONFIG_ONEYE_FW_LOG_PROBE
    log_cfg.batch_interval_ms = 20000u;
    log_cfg.min_items_per_frame = 500u;
#endif
    log_cfg.event_cb = sdk_event_cb;
    (void)oneye_dev_log_init(&log_cfg);

    /* 4) event */
    memset(&ev_cfg, 0, sizeof(ev_cfg));
    ONEYE_DEV_STRUCT_INIT(ev_cfg);
    ev_cfg.enabled = true;
    ev_cfg.track_enabled = true;
    ev_cfg.offline_bytes = 64 * 1024;
    ev_cfg.event_cb = sdk_event_cb;
    (void)oneye_dev_event_init(&ev_cfg);

    /* 5) 链路 */
    rc = oneye_dev_base_start();
    if (rc != ONEYE_DEV_SDK_OK) {
        ESP_LOGE(TAG, "base start 失败：%s", oneye_dev_strerror(rc));
        return;
    }
    (void)oneye_dev_base_wait_event(3000);

    /* 5b) 授时（契约 §7）由 CLOUD_LINK_UP 事件驱动的 `request_time_sync()` 发起（见 sdk_event_cb）：
     *     `oneye_dev_base_sync_time()` 会阻塞等待云端应答，必须跑在独立任务里，不能在 SDK 回调内调用。 */

    /* 6) 自检结论 + 已登记影子键（firmwareVersion / power / credSource）
     *
     * `credSource`：本机身份**从哪来**（`partition` = 分区里的每台一份；`embedded` = 回退到
     * 固件里的公用凭据）。为什么必须报：回退本身可以接受，**"看不清有没有回退"不可以** ——
     * 一台漏写分区的机器拿公用身份上线，平台上若与正常机器看不出区别，它会被一直当好设备用下去。
     * 键名与取值由 SDK 固定（`ONEYE_DEV_CREDS_SHADOW_KEY`），所有伙伴报的完全一致，服务端才能统一告警。
     * `src_json` 是单个键的 JSON 片段（`{"credSource":"…"}`），这里去掉它开头的 `{` 后并入本行。 */
    ONEYE_LOGI(ONEYE_DEV_LOG_TAG_BASE,
               "korvo2_oneye 板级自检：%d 项 / 失败 %d 项；固件 %s",
               s_check_total, s_check_failed, ONEYE_FW_VERSION);
    {
        char shadow[192];
        char src_json[64];
        char fields[64];
        bool has_src;
        oneye_dev_sdk_err_t rrc;

        /* 取 SDK 给的 `{"credSource":"…"}` 的**内层**（去掉外层那一对花括号），
         * 再把这段并进本行的对象里。
         *
         * ⚠️ 这里以前写的是 `src_json + 1`：它只去掉了开头的 `{`，**结尾的 `}` 还留着**，
         * 于是拼出来的是 `…"partition"}}` —— **多一个花括号 ⇒ 不是合法 JSON**。
         * 后果（2026-09-24 真机实测，查了几轮才定位）：`oneye_dev_base_report_state()`
         * 返回 INVALID（参数非法），设备**从来没上报过影子** ⇒ 平台侧没有影子文档，
         * 控制台的节点页与身份视图都看不到这台设备；而当时调用处是 `(void)…` 加一句
         * 无条件的"凭据来源已上报"，所以串口上看起来一切正常。
         * 修法：成对地去括号，并且**校验形状**（不是 `{…}` 就当作没有这一段，不硬拼）。 */
        has_src = false;
        if (oneye_dev_creds_state_json(cred_src, src_json, sizeof(src_json)) == ONEYE_DEV_SDK_OK) {
            size_t n = strlen(src_json);
            if (n >= 2u && src_json[0] == '{' && src_json[n - 1u] == '}' && (n - 2u) < sizeof(fields)) {
                memcpy(fields, src_json + 1, n - 2u);
                fields[n - 2u] = '\0';
                has_src = true;
            }
        }
        if (!has_src) {
            /* 来源未知（或形状不对）就不报这一项：报一个含糊/拼坏的值比不报更坏 ——
             * 服务端要么没法据此告警，要么整帧解不开（那正是这次踩到的坑）。 */
            ESP_LOGW(TAG, "凭据来源未知，不上报 %s", ONEYE_DEV_CREDS_SHADOW_KEY);
        }
        snprintf(shadow, sizeof(shadow),
                 "{\"firmwareVersion\":\"%s\",\"power\":true%s%s}",
                 ONEYE_FW_VERSION, has_src ? "," : "", has_src ? fields : "");

        /* ⚠️ 这个返回值**必须看**（2026-09-24 修正）。
         *
         * 原来是 `(void)oneye_dev_base_report_state(shadow);` —— 紧接着无条件打印了一句
         * "凭据来源已上报"，于是**失败与成功在串口上长得一模一样**。真机实测的后果：
         * 设备在平台侧**没有影子文档**（身份视图/节点页都看不到它），而现场日志只在说"已上报"。
         * 上报失败常见于链路刚起（`bint_guard_link` 要求 initialized && started && connected），
         * 所以这里失败**重试一次**并两次都把错误码打出来 —— 让"没报上去"这件事不再需要靠猜。 */
        rrc = oneye_dev_base_report_state(shadow);
        if (rrc != ONEYE_DEV_SDK_OK) {
            ESP_LOGW(TAG, "影子上报失败（第 1 次）：%s —— 2 s 后重试", oneye_dev_strerror(rrc));
            vTaskDelay(pdMS_TO_TICKS(2000));
            rrc = oneye_dev_base_report_state(shadow);
        }
        if (rrc == ONEYE_DEV_SDK_OK) {
            ESP_LOGI(TAG, "影子上报成功（shadow/up，含 %s）：%s", ONEYE_DEV_CREDS_SHADOW_KEY, shadow);
            if (has_src) {
                ESP_LOGI(TAG, "凭据来源已上报：%s=%s", ONEYE_DEV_CREDS_SHADOW_KEY,
                         oneye_dev_creds_source_str(cred_src));
            }
        } else {
            ESP_LOGE(TAG, "影子上报**失败**（重试后仍为 %s）—— 平台侧将看不到 %s，"
                          "设备在控制台的节点页/身份视图里不会出现",
                     oneye_dev_strerror(rrc), ONEYE_DEV_CREDS_SHADOW_KEY);
        }

        /* 上报返回 OK 只代表"已入发送队列"，**不代表已经发到线上**。
         * 2026-09-24 真机就撞上了这个区别：串口说上报成功、broker 上却一帧都没有。
         * 所以这里把 SDK 自己的收发计数与链路状态一并打出来 —— 让"入队了但没发出去"这件事
         * 在串口上就能分辨（tx_frames 不涨 = 发送路径没走通；dropped_frames 涨 = 被丢弃）。 */
        {
            oneye_dev_base_stats_t st;
            oneye_dev_link_status_t ls;
            oneye_dev_sdk_err_t src, lrc;
            /* ⚠️ 这两个结构体带 struct_size/api_version ABI 守卫（所有 SDK 结构体都带）：
             * 不初始化就调用，getter 会直接返回 INVALID 而**什么都不写** —— 第一版诊断就这么
             * 静默失效了（串口一行都没多）。`ONEYE_DEV_STRUCT_INIT` 就是干这个的。 */
            memset(&st, 0, sizeof(st));
            ONEYE_DEV_STRUCT_INIT(st);
            memset(&ls, 0, sizeof(ls));
            ONEYE_DEV_STRUCT_INIT(ls);
            src = oneye_dev_base_get_stats(&st);
            lrc = oneye_dev_base_get_link_status(&ls);
            ESP_LOGI(TAG, "[cloud-stats] stats_rc=%d link_rc=%d tx_frames=%u rx_frames=%u dropped=%u retries=%u cloud_link_up=%d transport=%s",
                     (int)src, (int)lrc,
                     (unsigned)st.tx_frames, (unsigned)st.rx_frames,
                     (unsigned)st.dropped_frames, (unsigned)st.publish_retries,
                     (int)ls.cloud_link_up,
                     oneye_dev_transport_str(ls.transport));
        }

        /* 记住这次的内容，交给**已在跑的** `uplink_maintain_run()`（cloud_start_task 内）在
         * `down → up` 时补发（2026-09-24 起；2026-09-25 改为事件驱动）。
         *
         * 为什么曾经必须周期重述，以及为什么现在不必了：`credSource` 是**状态**，不是事件 ——
         * 开机那一帧丢了，这台机器在凭据来源视图里就永远看不见（正是要防的静默事故）。真机实测：
         * 开机那次上报的帧在队列里等到第一次 drain 时已经过了影子面 30 s 的 TTL，被静默清掉
         * （`tx expired: face=1`）—— 平台上因此**从来没有影子文档**：控制台节点页与身份视图都
         * 看不到这台设备，而 `report_state()` 返回的是成功。
         * 但这条**断链**故障现由 SDK 自己兜底（链路恢复时刷新待发帧的入队时刻，见
         * `internal/oneye_internal.c:756-764`），周期重报在它之上是冗余的；而周期重报本身要付
         * ≈1440 帧/天/设备（对照登记的 `fallbackPeriodSec=300` ⇒ 288 帧/天/设备，差 5×）。
         * 故 2026-09-25 按 R80 选项 (iv) 去掉周期，只保留**事件驱动**的 `down → up` 补发；
         * 代价（非掉线类丢失不再自愈）见 shadow_reassert_step() 的注释，是**已接受**的。
         *
         * ★ 2026-09-30：这里**不再 `xTaskCreate(shadow_reassert_task, …4096 words…)`** ——
         *   真机实测那个任务起不来（告警逐字见 shadow_reassert_step() 注释）。补发判定改为
         *   `shadow_reassert_step()`，由常驻的 `uplink_maintain_run()` 每秒调用一次；
         *   **不新建任务、不申请新栈**，因此不存在"起不来"这一态。 */
        snprintf(s_shadow_state, sizeof(s_shadow_state), "%s", shadow);
        s_shadow_state_valid = true;
    }

#if CONFIG_ONEYE_FW_ENABLE_MPP
    {
        oneye_dev_mpp_config_t mpp_cfg;
        memset(&mpp_cfg, 0, sizeof(mpp_cfg));
        ONEYE_DEV_STRUCT_INIT(mpp_cfg);
        mpp_cfg.event_cb = sdk_event_cb;
        (void)oneye_dev_mpp_init(&mpp_cfg);
        ESP_LOGW(TAG, "mpp 域已初始化（不发起会话：媒体数据面未在真机验证）");
    }
#endif
}

#if CONFIG_ONEYE_FW_LOG_PROBE
/* ★ 取证插桩（2026-09-21，**Kconfig 默认关**；取证后可整段删除）：
 *
 * 为什么需要它：本固件**只有开机自检那一行**走 SDK 的 log 面（`ONEYE_LOGI`，见 oneye_start 第 6 步），
 * 其余日志全是 ESP-IDF 的 `ESP_LOG*`（只落本地栈）；而 SDK **内部**日志按设计也只落平台输出、
 * **不经 log 库上行**（`oneye_internal.c:oneye_int_log` 的注释自陈"经 log 库上行需增加钩子"）。
 * ⇒ 板上 `rmng/dev/<node>/log/up` 面上行**几乎无流量**，于是 `set_uplink` / `dump` 的"效果"
 * 在真机上**无从观测**（2026-09-21 首次复验实测到这一点：19 条下行全部投递成功、串口有日志，
 * 但 broker 侧一帧 `log/up` 都没有）。
 *
 * 本函数通过**公开 API**（不是内部日志）按确定节奏写记录：每 3 s 一条；每第 5 条之后再补一簇 5 条，
 * 用来观察 `dump` 的窗口语义（配合 20 s 的批量间隔，缓冲里的记录停得住）。
 *
 * ★★ 2026-09-30 真机修正：本函数**不再是一个任务** ★★
 *
 * 第一版是 `xTaskCreate(log_probe_task, "log_probe", 3072, …)`（3072 words = 12 KB 内部 RAM 栈）。
 * 真机实测 `pdPASS` **失败**（`_tmp-phase2/serial-trackup-proof2.txt:244`），且它的返回值当时被 `(void)` 丢掉
 * ⇒ 板上**没有一行**能证明它失败，症状与"代码没写"完全同形（`device-serial-proof.md` §7.3 只能写"高度疑似"）。
 * 本批两件事一起做：① 返回值告警（上一批已接，本批连任务本身一起删）；② 拆成 `log_probe_step()`——
 * **写一轮立即返回**，由已在跑的 `uplink_maintain_run()` 每 3 s 调用一次。
 * 为什么能绕开内部 RAM 约束：它跑在 `cloud_start_task` **已经分配**的 6144 words 栈上，不再申请 12 KB 新栈；
 * 写的记录与节奏与原任务**逐条相同**（`tick=` 每 3 s 一条、每第 5 条补 `burst=` 一簇 5 条），
 * 只是把 `vTaskDelay` 换成了调用者的节拍。
 * ⚠️ 因此**不许**把它变回独立任务：3072 words 在这台板子上同样要不到（同刻 `largest_internal_block=2304 B`）。
 *
 * 频率核算（与 sdkconfig.defaults:100-102 的登记同源，本批未改）：每 15 s 共 10 条 × 约 60–100 B
 * ≈ 0.1 KB/s，且 20 s 才组一帧 ⇒ 帧率 0.05 帧/s、字节率约为契约基线（≤1 帧/s、≤16 KB/s）的 0.6%。 */
static void log_probe_step(uint32_t *seq)
{
    uint32_t i = ++(*seq);

    ONEYE_LOGI(ONEYE_DEV_LOG_TAG_BASE, "log-probe tick=%u", (unsigned)i);
    if ((i % 5u) == 0u) {
        uint32_t k;

        for (k = 0u; k < 5u; k++) {
            ONEYE_LOGI(ONEYE_DEV_LOG_TAG_BASE, "log-probe burst=%u/%u", (unsigned)i,
                       (unsigned)k);
        }
    }
}

#endif /* CONFIG_ONEYE_FW_LOG_PROBE */

/* ---------------------------------------------------------------- 埋点 → track/up
 *
 * 契约：`rmng/dev/{node}/track/up`（QoS **0**、信封 + `type=track`、≥5 s 聚合窗口、≤16 KB/帧；
 * 正本 = backend/contracts/api/mqtt/传输规范.md:89/:148、backend/contracts/domain/事件与埋点上报.md §2.4/§3.3）。
 * SDK 侧的**通道、白名单、组帧、API 全都早已存在**（组帧 `components/oneye-dev-sdk/src/oneye_dev_event.c:638`、
 * 白名单 `include/oneye_dev_event.h:47-53`、API 声明 `:135`），缺的只有**产出方**：
 * 本文件 `oneye_start()` 第 4 步把 `ev_cfg.track_enabled` 置了 true（`:981`），
 * 而 2026-09-30 实测**全 `examples/oneye` 对 `oneye_dev_event_track()` 零调用**
 * ⇒ 这条面长期"开着但从不发"。下面补两个**只报真事**的产出方（不新造名字、不新造字段）：
 *
 *  ① `boot`（`ONEYE_DEV_TRACK_BOOT`，`oneye_dev_event.h:53`）：**每次启动必发一条**。attrs 只带白名单
 *     允许的 `uptime_ms`（`事件与埋点上报.md:116`），取值 = `esp_timer_get_time()` 的**实测**运行毫秒。
 *     为什么必须**等链路上线**才发：SDK 对 track 走 QoS0 且**离线不缓存**
 *     （`src/oneye_dev_event.c:1188-1191` 逐字：`!link_up()` ⇒ 直接丢弃并计入 `track_dropped`）
 *     ⇒ 链路没起就调用等于没发。故本任务先等到 `cloud_link_up` 再发（最多等 120 s，超时如实告警）。
 *     为什么不带 `reason`：`boot` 的 `reason` **取值**在契约里没有登记（§3.3 只登记了键名），
 *     这里不自行发明一套"重启原因"取值；要加须先登记。
 *
 *  ② `uplink.throttled`（`ONEYE_DEV_TRACK_UPLINK_THROTTLE`，`oneye_dev_event.h:51`）：**只在计数真的
 *     涨了才发**。attrs 的 `face` 取自 SDK 自己的**面名**（`oneye_dev_face_str()`，`src/oneye_dev_base.c:103-118`，
 *     不新造取值），`dropped` = 本次采样相对上次的**真实增量**；计数不涨就一条都不发 ——
 *     契约要的是"上行限速/背压取证"（`事件与埋点上报.md:114`），**没有背压就不许报背压**。
 *     取这两面：`log` 面 `dropped`（缓冲满丢弃，`oneye_dev_log.h:67`）与 `event` 面 `items_dropped`
 *     （队列/离线缓存满丢弃，`oneye_dev_event.h:95`）；**不含** `track_dropped` —— 它把"白名单外被拒"
 *     与"QoS0 离线按设计丢弃"也计进来，报成 throttled 会失真。
 *
 * 频率：10 s 采样一次（track 面聚合窗口 ≥5 s；计数器单调累计，10 s 既不高频也不漏增量）
 * ⇒ `boot` 每次启动至多 1 条、`uplink.throttled` 每面每次采样至多 1 条，远低于契约限额。
 *
 * ★★ 2026-09-30 真机修正：为什么这里**不再是一个独立任务** ★★
 *
 * 第一版把上面这套写成独立任务 `xTaskCreate(track_task, "track", 4096, …)`（4096 words = 16 KB 内部 RAM 栈）。
 * 真机两轮各 120 s 串口取证（`_tmp-phase2/serial-korvo2-proof*.txt`）实测：**`pdPASS` 失败**，
 * 板上只留一行 `埋点任务创建失败（内存不足）⇒ 本轮不会发出任何 track/up`，`[track]` 命中 0 行、
 * broker 侧 `track/up` 0 帧。同一时刻、同样 4096 words 的 `shadow_reassert` 也失败；而 `log_probe` 的返回值
 * 当时被 `(void)` 丢掉 ⇒ 它八成也没起来，却**零告警**。
 * 把 `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 由 64K 提到 128K **实测无效**（两条失败告警逐字不变，已回滚）：
 * 缺的不是"预留额度"，而是这块板上（ADF + esp-sr AFE + 摄像头 + Wi-Fi + 面板同时在跑）**要不到 16 KB 连续内部 RAM**。
 *
 * ⇒ 修法不是"再挤一个任务出来"，而是**复用已经在跑的任务**：`oneye_start()` 本来就跑在 `cloud_start_task`
 * （6144 words，创建点 `on_wifi_ready()`，真机确认创建成功）里，而"链路上线"这一刻也正好落在它手里 ⇒ 埋点**就地**跑在它那儿。
 * 代价：`cloud_start_task` 不再 `vTaskDelete(NULL)`，那 6144 words 栈转为常驻。它是**已分配**的栈，不抬高内存峰值；
 * 反过来，任何"再建一个 ≥16 KB 任务"的写法都会重现同一个失败 —— 这就是不许走那条路的原因。
 *
 * ★★ 2026-09-30（本批）把另外两条产出方也并进同一个循环 ★★
 *
 * 上一批只解决了 track 面；真机同刻的另外两条**仍然失败**：`shadow_reassert_task`（4096 words，影子补发）
 * 与 `log_probe_task`（3072 words，`log/up` 周期流量）。本批按**同一条**修法把三者并进
 * `uplink_maintain_run()` 一个 1 s 节拍：影子补发每秒判一次（`shadow_reassert_step()`）、
 * log 取证每 3 s 写一轮（`log_probe_step()`）、track 采样每 10 s 一次。
 * 判据：本工程现在**只在 `on_wifi_ready()` 里创建 1 个上云任务 + 1 个面板同步任务**，
 * 两者都接返回值；不再有任何"可能起不来的产出方任务"。
 */
#define ONEYE_FW_TRACK_SAMPLE_MS  10000u   /* `uplink.throttled` 采样周期（contract 聚合窗口 ≥5 s） */
#define ONEYE_FW_MAINTAIN_TICK_MS  1000u   /* 常驻维护循环节拍：影子 down→up 判定需要这个量级 */
#if CONFIG_ONEYE_FW_LOG_PROBE
#define ONEYE_FW_LOG_PROBE_MS      3000u   /* log 取证插桩写入间隔（与原 log_probe_task 同值） */
#endif

/* 上报一条 `uplink.throttled`。**只由"计数真的涨了"的调用点触发**（见上），不做任何猜测性上报。 */
static void track_emit_throttled(oneye_dev_face_t face, uint32_t dropped)
{
    const char *face_name = oneye_dev_face_str(face);
    char attrs[64];
    oneye_dev_sdk_err_t trc;

    if (face_name == NULL) {
        return;                     /* 没有面名可报：宁可少报，也不报含糊值 */
    }
    (void)snprintf(attrs, sizeof(attrs), "{\"face\":\"%s\",\"dropped\":%u}",
                   face_name, (unsigned)dropped);
    trc = oneye_dev_event_track(ONEYE_DEV_TRACK_UPLINK_THROTTLE, attrs);
    if (trc == ONEYE_DEV_SDK_OK) {
        ESP_LOGW(TAG, "[track] uplink.throttled 已上报（%s）", attrs);  /* 背压本身要显眼 ⇒ WARN */
    } else {
        ESP_LOGW(TAG, "[track] uplink.throttled 上报失败：%s（%s）",
                 oneye_dev_strerror(trc), attrs);
    }
}

/* 常驻上行维护循环：**由 cloud_start_task 直接调用**（不新建任务，见上）。本函数不返回。
 *
 * 三条产出方共用一个节拍（`ONEYE_FW_MAINTAIN_TICK_MS` = 1 s），各自按自己的周期到点才动：
 *   ① 影子整帧补发 —— 每拍一次 `shadow_reassert_step(link_up)`（原 `shadow_reassert_task` 的判据逐行未改）
 *   ② log 取证插桩 —— 每 3 s 一轮 `log_probe_step()`（原 `log_probe_task` 的节奏逐条未改）
 *   ③ track 埋点     —— `boot` 只发一次（等链路上线 → 等 SDK 计数确认）；`uplink.throttled` 每 10 s 采样
 * 三者的**入参/取值/判据全部沿用原实现**，唯一变化是"宿主任务"与"节拍从各自 `vTaskDelay` 变成统一 tick"。 */
static void uplink_maintain_run(void)
{
    uint32_t prev_event_drops = 0u;
    uint32_t prev_log_drops = 0u;
    uint32_t ms_since_track = 0u;
    uint32_t boot_waited_ms = 0u;
#if CONFIG_ONEYE_FW_LOG_PROBE
    uint32_t probe_seq = 0u;
    uint32_t ms_since_probe = 0u;
#endif
    /* boot 埋点状态机：0 = 等链路上线；1 = 已入队、等 SDK 计数确认；2 = 已了结（成功/失败/超时，都不再动） */
    int boot_state = 0;
    char boot_attrs[48] = { 0 };
    bool link_up = false;

    for (;;) {
        oneye_dev_link_status_t ls;

        vTaskDelay(pdMS_TO_TICKS(ONEYE_FW_MAINTAIN_TICK_MS));
        ms_since_track += ONEYE_FW_MAINTAIN_TICK_MS;
        boot_waited_ms += ONEYE_FW_MAINTAIN_TICK_MS;
#if CONFIG_ONEYE_FW_LOG_PROBE
        ms_since_probe += ONEYE_FW_MAINTAIN_TICK_MS;
#endif

        /* 链路状态每拍只取一次，三处共用（`ONEYE_DEV_STRUCT_INIT` 不可省：不初始化 struct_size
         * ⇒ getter 返回 INVALID 且**什么都不写**，上一版诊断就这么静默失效过）。 */
        memset(&ls, 0, sizeof(ls));
        ONEYE_DEV_STRUCT_INIT(ls);
        link_up = (oneye_dev_base_get_link_status(&ls) == ONEYE_DEV_SDK_OK) && ls.cloud_link_up;

        /* ① 影子补发：仅 `down → up` 且状态有效时补发整帧 */
        shadow_reassert_step(link_up);

        /* ② log 取证插桩：每 3 s 写一轮**真实**记录（走 SDK 公开 log API，不伪造内容、不新造 tag） */
#if CONFIG_ONEYE_FW_LOG_PROBE
        if (ms_since_probe >= ONEYE_FW_LOG_PROBE_MS) {
            ms_since_probe = 0u;
            log_probe_step(&probe_seq);
        }
#endif

        /* ③a `boot`：等链路上线（QoS0 + 离线不缓存 ⇒ 链路没起就发等于没发），最多 120 s */
        if (boot_state == 0) {
            if (link_up) {
                oneye_dev_sdk_err_t trc;

                (void)snprintf(boot_attrs, sizeof(boot_attrs), "{\"uptime_ms\":%llu}",
                               (unsigned long long)(esp_timer_get_time() / 1000));
                trc = oneye_dev_event_track(ONEYE_DEV_TRACK_BOOT, boot_attrs);
                if (trc == ONEYE_DEV_SDK_OK) {
                    /* ⚠️ 返回值 OK **只代表"已入聚合缓冲"**，不代表已经发出去：track 面按 ≥5 s 的窗口组帧
                     * （`oneye_dev_event.c:651-656`），实际冲刷由 SDK 自己的周期 tick 驱动。
                     * 2026-09-30 真机第一版修好后就撞到了这个区别：串口打的是 `track_reported=0`（刚入队，
                     * 还没到窗口），而 broker 上其实收到了帧 —— 只看第一行会误判成"没发出去"。
                     * 所以这里**等计数真的动了**再打判据行（最多 30 s；实测约 20 s 与 log 面同批发出）。
                     * 这一步是"不许静默"的关键：判据行必须由 SDK 的实测计数背书，而不是由调用返回值背书。 */
                    ESP_LOGI(TAG, "[track] boot 已入队（%s）—— 等聚合窗口/周期冲刷后确认", boot_attrs);
                    boot_state = 1;
                    boot_waited_ms = 0u;
                } else {
                    ESP_LOGW(TAG, "[track] boot 上报失败：%s（%s）",
                             oneye_dev_strerror(trc), boot_attrs);
                    boot_state = 2;
                }
            } else if (boot_waited_ms >= 120000u) {
                ESP_LOGW(TAG, "[track] 等云链路上线超时（120 s）⇒ 不发 boot 埋点"
                              "（track 面 QoS0 且离线不缓存，链路没起时发了也会被丢）");
                boot_state = 2;
            }
        } else if (boot_state == 1) {
            oneye_dev_event_stats_t st;

            memset(&st, 0, sizeof(st));
            ONEYE_DEV_STRUCT_INIT(st);
            (void)oneye_dev_event_get_stats(&st);
            if (st.track_reported > 0u || st.track_dropped > 0u) {
                if (st.track_reported > 0u && st.track_dropped == 0u) {
                    ESP_LOGI(TAG, "[track] boot 已上报（%s）；track_reported=%u track_dropped=%u frames_tx=%u",
                             boot_attrs, (unsigned)st.track_reported, (unsigned)st.track_dropped,
                             (unsigned)st.frames_tx);
                } else {
                    ESP_LOGW(TAG, "[track] boot 已入队但 30 s 内未见上报确认（%s）："
                                  "track_reported=%u track_dropped=%u frames_tx=%u",
                             boot_attrs, (unsigned)st.track_reported, (unsigned)st.track_dropped,
                             (unsigned)st.frames_tx);
                }
                boot_state = 2;
            } else if (boot_waited_ms >= 30000u) {
                ESP_LOGW(TAG, "[track] boot 已入队但 30 s 内未见上报确认（%s）："
                              "track_reported=%u track_dropped=%u frames_tx=%u",
                         boot_attrs, (unsigned)st.track_reported, (unsigned)st.track_dropped,
                         (unsigned)st.frames_tx);
                boot_state = 2;
            }
        }

        /* ③b `uplink.throttled`：每 10 s 采样各面丢弃计数，**只报真实增量** */
        if (ms_since_track >= ONEYE_FW_TRACK_SAMPLE_MS) {
            oneye_dev_event_stats_t es;
            oneye_dev_log_stats_t lgs;

            ms_since_track = 0u;

            memset(&es, 0, sizeof(es));
            ONEYE_DEV_STRUCT_INIT(es);
            if (oneye_dev_event_get_stats(&es) == ONEYE_DEV_SDK_OK) {
                if (es.items_dropped > prev_event_drops) {
                    track_emit_throttled(ONEYE_DEV_FACE_EVENT, es.items_dropped - prev_event_drops);
                }
                prev_event_drops = es.items_dropped;    /* 取数失败时**不**改写基准，避免造出假增量 */
            }

            memset(&lgs, 0, sizeof(lgs));
            ONEYE_DEV_STRUCT_INIT(lgs);
            if (oneye_dev_log_get_stats(&lgs) == ONEYE_DEV_SDK_OK) {
                if (lgs.dropped > prev_log_drops) {
                    track_emit_throttled(ONEYE_DEV_FACE_LOG, lgs.dropped - prev_log_drops);
                }
                prev_log_drops = lgs.dropped;
            }
        }
    }
}

/* 面板用的链路快照同步（2 s 周期；面板只读，不改变设备行为） */
static void panel_sync_task(void *arg)
{
    (void)arg;
    while (1) {
        oneye_dev_link_status_t link;
        oneye_dev_base_stats_t stats;
        memset(&link, 0, sizeof(link));
        ONEYE_DEV_STRUCT_INIT(link);
        memset(&stats, 0, sizeof(stats));
        ONEYE_DEV_STRUCT_INIT(stats);
        if (oneye_dev_base_get_link_status(&link) == ONEYE_DEV_SDK_OK &&
            oneye_dev_base_get_stats(&stats) == ONEYE_DEV_SDK_OK) {
            panel_api_set_link(link.cloud_link_up, oneye_dev_transport_str(link.transport),
                               stats.tx_frames, stats.rx_frames);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ---------------------------------------------------------------- 入口 */

void app_main(void)
{
    esp_err_t nvs;

    ESP_LOGI(TAG, "korvo2_oneye 启动：ESP32-S3-Korvo-2 v3 板级固件（ESP-ADF v2.8 / IDF 5.5）");
    ESP_LOGI(TAG, "编译期板级断言：board_expect.h 已通过（rst V3.1 ↔ board_def.h）");

    panel_api_set_identity(ONEYE_FW_VERSION, "ESP32-S3-Korvo-2 v3");

    nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init：%s（无 NVS 仍可跑板级自检）", esp_err_to_name(nvs));
    }

    /* ① 参数自检 + ② 板级初始化 + ③ 按键服务 */
    board_selftest();
    board_init_peripherals();
    keys_start();

    /* ③a 本地验证面：注册按键事件注入（POST /api/simulate/key，走与物理按键同一条上报路径）。
     *     用途 = 远程/自动化验证「上行 → 云端 ack → 面板第三态」；触发源是 HTTP，不是 ADC 按键。 */
    panel_api_set_key_simulator(sim_key_handler);

    /* ③b AEC 采集（录音 → WAV 落 SD/SPIFFS；供验证面板在 web 播放）
     *     传板级 **ADC**（ES7210）句柄：`s_board->audio_hal` 是 ES8311（DAC），
     *     `s_board->adc_hal` 才是 ES7210；录音前用它 `AUDIO_HAL_CTRL_START` 重新 arm ADC
     *     （含 MIC 偏置/增益刷新），避免 STOP 之后录到近乎静音。 */
    if (aec_capture_init(s_board != NULL ? s_board->adc_hal : NULL) == ESP_OK) {
        ESP_LOGI(TAG, "AEC 采集就绪（面板可触发录音：POST /api/action {\"op\":\"aec_start\"}）");
    } else {
        ESP_LOGW(TAG, "AEC 采集初始化失败（面板将显示 aec.enabled=false 或不可用）");
    }

    /* ③c 板上回放（SD 卡 wav/mp3 → 扬声器；面板可触发：POST /api/action {\"op\":\"play\",\"path\":...}） */
    if (player_init(s_board != NULL ? s_board->audio_hal : NULL) == ESP_OK) {
        (void)player_set_volume(80);
        ESP_LOGI(TAG, "板上回放就绪（仅 SD 卡 /sdcard 下的 wav、mp3）");
    }

    /* ③d 摄像头（OV3660，board_def 的 CAM_PIN_*；面板可触发：POST /api/camera/capture）
     *
     * 初始化顺序（真机两次实测定的口径，与上游注释**不完全一致**，此处以实测为准）：
     *   ① 放在 `board_init_peripherals()` **之前**（照抄上游 lcd_camera 的"camera init in advance"）
     *      ⇒ SCCB 探测失败（`camera probe ... no sensor FAIL`，t≈1.7 s）：此时 ADF 的 I2C 总线尚未建立；
     *   ② 放在 `board_init_peripherals()` **之后**（本实现）
     *      ⇒ 探测成功（`Camera PID=0x3660 / Detected OV3660 / address=0x3c`），LCD 也能正常刷新。
     *   即：上游那句注释针对的是"扩展芯片操作可能异常"，在**本板本固件**上并不要求摄像头先于 LCD。
     *
     * 失败**不中止启动**，且按 chk_warn 计（提示级）：摄像头属配件，缺了不应阻断上云/按键/录音/回放/LCD，
     * 否则设备连 IP 都拿不到、反而看不到失败原因（真机教训见 chk_warn 注释）。 */
    if (camera_api_init() == ESP_OK) {
        camera_state_t cs;
        camera_api_get_state(&cs);
        ESP_LOGI(TAG, "摄像头就绪（%s PID=0x%04x，fmt=%s fb=%s×%d）：面板可抓拍 POST /api/camera/capture",
                 cs.sensor, (unsigned)cs.pid, cs.format, cs.fb_loc, cs.fb_count);
        chk_warn("camera probe (OV3660, SCCB 0x3c)", true, cs.sensor);
    } else {
        ESP_LOGW(TAG, "摄像头未就绪（按「未装配/接线问题」取证，不得声明 video.* 能力位）");
        chk_warn("camera probe (OV3660, SCCB 0x3c)", false, "no sensor");
    }

    ESP_LOGI(TAG, "[board-check] 自检汇总：%d 项，失败 %d 项，提示 %d 项",
             s_check_total, s_check_failed, s_check_warn);

#if CONFIG_ONEYE_FW_SELFTEST_STRICT
    if (s_check_failed > 0) {
        ESP_LOGE(TAG, "板级自检失败 %d 项 → 按 STRICT 口径中止上云（不静默继续）", s_check_failed);
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }
#endif

    /* ★ 2026-10-07（第三批）：配网/NimBLE **之前**的基线读数。位置选在"外设全部就绪之后、
     *   `wifi_prov_boot()` 之前"（LCD/摄像头/SD/AEC/回放都已初始化）⇒ 两种机型都在**同一位置**
     *   打这一行，跨机型同阶段可比；BLE 机型再与 `phase=ble-ready…` 相减即 NimBLE 主机栈的净代价。 */
    res_report("pre-prov（外设就绪、配网与 NimBLE 之前）");

    /* ④ 配网（SD 卡凭据文件优先 → Kconfig 兜底）+ ⑤ oneye 上云
     * 无凭据/连不上 ⇒ 不中止：板级自检、按键、AEC 录音、板上回放、串口日志仍可用；
     * 本地验证面板需设备 IP，故仅在联网成功时启动（见 panel_start_if_enabled）。 */
    wifi_prov_set_ready_cb(on_wifi_ready);
    wifi_prov_boot();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
