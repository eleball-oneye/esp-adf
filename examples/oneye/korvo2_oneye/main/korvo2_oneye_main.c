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
 *   - 不发明通道/字段：自检结论走 `log/up` 文本；影子只写**已登记键**（`esp.fw_version` / `esp.power`）；
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
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
#include "net_probe.h"
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
         * 本固件仅打印（命令面/PTZ 属 S16，未落地，不做假实现）。 */
        ESP_LOGI(TAG, "[sdk-event] command payload: %.*s", (int)len, (const char *)payload);
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
#if CONFIG_ONEYE_FW_LOG_PROBE
static void log_probe_task(void *arg);
#endif

/* 联网就绪 → 启动上云。放在独立任务里跑（oneye_start 需较大栈；事件任务只置位）。
 * 回调会随重连反复触发，故用一次性标志保证 SDK 只启动一次；面板启动本身幂等。 */
static void cloud_start_task(void *arg)
{
    (void)arg;
    oneye_start();
    (void)xTaskCreate(panel_sync_task, "panel_sync", 3072, NULL, 3, NULL);
#if CONFIG_ONEYE_FW_LOG_PROBE
    /* ★ 取证插桩（默认关）：见 log_probe_task 注释 */
    (void)xTaskCreate(log_probe_task, "log_probe", 3072, NULL, 2, NULL);
#endif
    vTaskDelete(NULL);
}

static void on_wifi_ready(void)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;
    panel_start_if_enabled();     /* 掉线重连后拿到 IP 也能补起本地验证面 */
    if (xTaskCreate(cloud_start_task, "cloud_start", 6144, NULL, 4, NULL) != pdPASS) {
        started = false;
        ESP_LOGE(TAG, "上云任务创建失败（内存不足）");
        return;
    }
    ESP_LOGI(TAG, "已联网 → 启动上云（oneye-dev-sdk）");
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

/** 配网：凭据来源 = SD/SPIFFS 凭据文件 → Kconfig（SSID 非空时） */
static void wifi_prov_boot(void)
{
    char ssid[WIFI_PROV_SSID_MAX] = { 0 };
    char pass[WIFI_PROV_PASS_MAX] = { 0 };
    char src[64] = { 0 };
    bool have = false;

#if CONFIG_ONEYE_FW_ENABLE_WIFI_FILE
    /* SD 卡根目录 oneye-wifi.txt（用户投放）；SD 未挂载时自动试 SPIFFS 同名文件 */
    if (wifi_prov_load_file(ssid, sizeof(ssid), pass, sizeof(pass), src, sizeof(src)) == ESP_OK &&
        ssid[0] != '\0') {
        have = true;
    }
#endif

    if (!have && CONFIG_ONEYE_FW_WIFI_SSID[0] != '\0') {
        snprintf(ssid, sizeof(ssid), "%s", CONFIG_ONEYE_FW_WIFI_SSID);
        snprintf(pass, sizeof(pass), "%s", CONFIG_ONEYE_FW_WIFI_PASSWORD);
        snprintf(src, sizeof(src), "%s", "kconfig");
        have = true;
    }

    if (!have) {
        ESP_LOGW(TAG, "未找到 Wi-Fi 凭据 → 跳过上云；板级自检/按键/录音/本地回放继续运行");
        ESP_LOGW(TAG, "配网方式：把 oneye-wifi.txt（内容 ssid=… 与 password=…）放 SD 卡根目录后复位设备");
        panel_start_if_enabled();     /* 无 IP ⇒ 提示并直接返回 */
        return;
    }

    (void)wifi_prov_set_source(src);
    if (wifi_prov_connect(ssid, pass, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 连接失败（来源 %s，ssid=%s）→ 跳过上云；"
                      "请核对凭据文件内容后复位（也可用面板 /api/action wifi_set 改配）", src, ssid);
        panel_start_if_enabled();
        return;
    }

    ESP_LOGI(TAG, "Wi-Fi 已连接：ip=%s（ssid=%s，来源 %s）", wifi_prov_ip(), ssid, src);

    /* 板级网络自检（只读、带 errno）：把"没路由 / 网关不通 / 上行丢包"三种同形故障区分开。
       放在 SDK 建链之前，日志顺序即为"联网 → 自检 → 上云"。 */
    net_probe_report(CONFIG_ONEYE_FW_CLOUD_HOST, (unsigned)CONFIG_ONEYE_FW_CLOUD_PORT);

    /* 启动期授时：**严格 TLS 校验的前置条件**（设备无 RTC，未授时时系统时间为 1970，
       服务端证书 notBefore 落在未来 ⇒ mbedtls 必然 BADCERT_FUTURE）。失败不阻塞上云。 */
    (void)sntp_boot_sync();

    panel_start_if_enabled();         /* 本地验证面与云端链路解耦：拿到 IP 就起 */
    /* 上云由 on_wifi_ready()（联网就绪回调）启动；此处无需重复调用 */
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
    base_cfg.username = CONFIG_ONEYE_FW_DEVICE_ID;   /* EMQX ACL 变量 %u 依赖 */
    base_cfg.client_id = CONFIG_ONEYE_FW_DEVICE_ID;
    base_cfg.cloud_host = CONFIG_ONEYE_FW_CLOUD_HOST;
    base_cfg.cloud_port = (uint16_t)CONFIG_ONEYE_FW_CLOUD_PORT;
    base_cfg.cloud_ws_path = (CONFIG_ONEYE_FW_CLOUD_WS_PATH[0] != '\0') ? CONFIG_ONEYE_FW_CLOUD_WS_PATH : NULL;
    base_cfg.transport = fw_transport();
    base_cfg.token = (CONFIG_ONEYE_FW_CLOUD_TOKEN[0] != '\0') ? CONFIG_ONEYE_FW_CLOUD_TOKEN : NULL;

#if defined(ONEYE_FW_EMBED_CERTS)
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
#endif

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

    /* 6) 自检结论 + 已登记影子键（esp.fw_version / esp.power） */
    ONEYE_LOGI(ONEYE_DEV_LOG_TAG_BASE,
               "korvo2_oneye 板级自检：%d 项 / 失败 %d 项；固件 %s",
               s_check_total, s_check_failed, ONEYE_FW_VERSION);
    {
        char shadow[128];
        snprintf(shadow, sizeof(shadow),
                 "{\"esp.fw_version\":\"%s\",\"esp.power\":true}", ONEYE_FW_VERSION);
        (void)oneye_dev_base_report_state(shadow);
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
 * 本任务通过**公开 API**（不是内部日志）按确定节奏写记录：每 3 s 一条；每第 5 条之后再补一簇 5 条，
 * 用来观察 `dump` 的窗口语义（配合 20 s 的批量间隔，缓冲里的记录停得住）。 */
static void log_probe_task(void *arg)
{
    uint32_t i = 0u;

    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    for (;;) {
        ONEYE_LOGI(ONEYE_DEV_LOG_TAG_BASE, "log-probe tick=%u", (unsigned)++i);
        if ((i % 5u) == 0u) {
            uint32_t k;

            for (k = 0u; k < 5u; k++) {
                ONEYE_LOGI(ONEYE_DEV_LOG_TAG_BASE, "log-probe burst=%u/%u", (unsigned)i,
                           (unsigned)k);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

#endif /* CONFIG_ONEYE_FW_LOG_PROBE */

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

    /* ④ 配网（SD 卡凭据文件优先 → Kconfig 兜底）+ ⑤ oneye 上云
     * 无凭据/连不上 ⇒ 不中止：板级自检、按键、AEC 录音、板上回放、串口日志仍可用；
     * 本地验证面板需设备 IP，故仅在联网成功时启动（见 panel_start_if_enabled）。 */
    wifi_prov_set_ready_cb(on_wifi_ready);
    wifi_prov_boot();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
