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
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "board.h"
#include "es7210.h"
#include "esp_peripherals.h"
#include "input_key_service.h"

#include "oneye_dev_sdk.h"

#include "board_expect.h"
#include "panel_api.h"

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
    chk_int("blue_led_gpio (TCA9554 P7)", (1 << 7), get_blue_led_gpio());
    chk_int("es8311_mclk_src (ESP MCLK)", 0, get_es8311_mclk_src());

    /* ADC 输入通道格式：rst「AEC 电路」——4 通道，R = 硬件 AEC 回采；board_def.h:126 */
    chk_str("adc_input_ch_format", "RMNM", AUDIO_ADC_INPUT_CH_FORMAT);

    ESP_LOGI(TAG, "[board-check] 结果：%d 项，失败 %d 项", s_check_total, s_check_failed);
}

/* ---------------------------------------------------------------- 板级初始化 */

static esp_periph_set_handle_t s_periph_set;

static void board_init_peripherals(void)
{
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    s_periph_set = esp_periph_set_init(&periph_cfg);
    if (s_periph_set == NULL) {
        chk_ok("esp_periph_set_init", false, "外设集合创建失败");
        return;
    }

    audio_board_handle_t board = audio_board_init();
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
    } else {
        ESP_LOGW(TAG, "[board-check] %-34s fail（无卡检测引脚 ⇒ 无卡时属预期，需人工核对）",
                 "sdcard mount (1-line)");
    }
#endif

#if CONFIG_ONEYE_FW_ENABLE_LCD
    /* rst：ILI9341 320x240 + TCA9554 CS/RST/BL（board.c:89-153） */
    chk_ok("lcd init (ILI9341 320x240)", audio_board_lcd_init(s_periph_set, NULL) != NULL, "panel");
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

static void sdk_event_cb(oneye_dev_event_t evt, const void *payload, size_t len, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "[sdk-event] %s len=%u", evt_name(evt), (unsigned)len);
    if (evt == ONEYE_DEV_EVENT_EVT_ACK) {
        /* 云端 ack：契约信封 data.ref == 上行事件 id ⇒ 关联到本地按键历史（"历史响应"第三态）
         * 注意：a.c. 的 "ack" 指云端对 event/up 的确认帧；SDK 以原始负载投递。 */
        panel_api_key_ack((const char *)payload, len);
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

static esp_err_t input_key_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx)
{
    (void)handle;
    (void)ctx;
    if (evt == NULL) {
        return ESP_OK;
    }
    const char *key = key_user_str((int)evt->data);
    const char *act = key_action_str((int)evt->type);

    /* 契约映射页 §2 行 18：按键事件走 `event/up`，data{key,action}；不为按键新开下行面 */
    char data[96];
    snprintf(data, sizeof(data), "{\"key\":\"%s\",\"action\":\"%s\"}", key, act);
    ESP_LOGI(TAG, "[key] %s/%s", key, act);

    /* 本地验证面：先登记本地检测（pending），再上报；随后按 SDK 回执/云端 ack 推进状态 */
    static uint32_t s_key_id;
    char id[24];
    snprintf(id, sizeof(id), "key-%05u", (unsigned)++s_key_id);
    panel_api_key_event(key, act, id, (uint64_t)(esp_timer_get_time() / 1000));

    oneye_dev_event_item_t item;
    memset(&item, 0, sizeof(item));   /* 事件条目无 struct_size/api_version（非配置结构体） */
    item.id = id;                     /* 幂等 id：用于与云端 ack 的 data.ref 关联 */
    item.type = ONEYE_FW_EVENT_TYPE_DEVICE_EVENT;
    item.severity = ONEYE_DEV_SEVERITY_INFO;
    item.title = "key";
    item.data_json = data;

    oneye_dev_sdk_err_t rc = oneye_dev_event_report(&item, 1);
    panel_api_key_uplink(id, rc == ONEYE_DEV_SDK_OK ? "sent" : "failed");
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

/* ---------------------------------------------------------------- Wi-Fi（板级无关，仅为上云前置） */

static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi 断开，重连");
        panel_api_set_wifi(false, NULL);
        (void)esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        char ip[20] = "";
        esp_ip4addr_ntoa(&evt->ip_info.ip, ip, sizeof(ip));
        ESP_LOGI(TAG, "Wi-Fi 已获取 IP：%s", ip);
        panel_api_set_wifi(true, ip);
#if CONFIG_ONEYE_FW_ENABLE_PANEL_API
        /* 本地验证面（面板）需要 IP；此处启动，与云端链路是否可用无关 */
        if (panel_api_start(0) == ESP_OK) {
            ESP_LOGI(TAG, "验证面板：浏览器打开宿主面板服务，或直接访问 http://%s/api/status", ip);
        }
#endif
        if (s_wifi_eg != NULL) {
            xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        }
    }
}

static bool wifi_connect(const char *ssid, const char *password)
{
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t sta = { 0 };

    s_wifi_eg = xEventGroupCreate();
    if (s_wifi_eg == NULL) {
        return false;
    }
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, password, sizeof(sta.sta.password) - 1);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    (void)esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "等待 Wi-Fi 连接（SSID=%s）…", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(20000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
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

    ESP_LOGI(TAG, "[board-check] 自检汇总：%d 项，失败 %d 项", s_check_total, s_check_failed);

#if CONFIG_ONEYE_FW_SELFTEST_STRICT
    if (s_check_failed > 0) {
        ESP_LOGE(TAG, "板级自检失败 %d 项 → 按 STRICT 口径中止上云（不静默继续）", s_check_failed);
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }
#endif

    /* ④ 联网 + ⑤ oneye 上云（SSID 为空 → 只跑板级自检与按键） */
    if (CONFIG_ONEYE_FW_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "未配置 Wi-Fi SSID → 跳过上云；板级自检与按键服务继续运行");
    } else if (!wifi_connect(CONFIG_ONEYE_FW_WIFI_SSID, CONFIG_ONEYE_FW_WIFI_PASSWORD)) {
        ESP_LOGE(TAG, "Wi-Fi 连接超时 → 跳过上云");
    } else {
        ESP_LOGI(TAG, "Wi-Fi 已连接");
        oneye_start();
        /* 面板链路快照同步（本地验证面；与云端链路状态解耦） */
        (void)xTaskCreate(panel_sync_task, "panel_sync", 3072, NULL, 3, NULL);
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
