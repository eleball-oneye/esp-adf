/*
 * clawbot POC firmware — ESP32-S3-Touch-LCD-1.69
 *
 * 3x DRV5032AJ hall inputs (index/middle/ring) + on-board QMI8658C IMU,
 * BLE HID mouse, and a CSV data stream on the USB Serial/JTAG console.
 *
 * Wiring (see README.md):
 *   HALL_INDEX  -> GPIO2    HALL_MIDDLE -> GPIO3    HALL_RING -> GPIO17
 *   3V3 / GND   -> header 3V3 / GND
 *   IMU         -> on-board QMI8658C (I2C SDA=GPIO11, SCL=GPIO10)
 */

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "esp_hidd_prf_api.h"
#include "hid_dev.h"
#include "lcd.h"
#include "motion.h"
#include "qmi8658.h"

static const char *TAG = "clawbot";

/* ---------------- board wiring ---------------- */
#define PIN_HALL_INDEX   2
#define PIN_HALL_MIDDLE  18  /* GPIO3 is a strapping pin, avoid it */
#define PIN_HALL_RING    17
#define PIN_IMU_SDA      11
#define PIN_IMU_SCL      10

/* ---------------- tuning ---------------- */
#define HALL_PERIOD_MS      50      /* 20 Hz, matches the product spec */
#define IMU_PERIOD_MS       10      /* 100 Hz */
#define REPORT_PERIOD_MS    20      /* 50 Hz */
#define DEBOUNCE_FRAMES     2

#define STABLE_MS           50.0f   /* skip the press transient */
#define SCROLL_DEG          6.0f    /* displacement that turns a press into scroll */
#define STANDBY_MS          300.0f  /* hold without motion -> standby (no toggle) */
#define WHEEL_DEG_PER_TICK  6.0f
#define CURSOR_GAIN_DEFAULT 20.0f   /* pixels per degree, tunable at runtime */
#define CURSOR_MAX_STEP     40.0f

/* Set to 1 to print the CSV stream, 0 to keep the console quiet. */

/* ---------------- state ---------------- */
typedef enum {
    G_IDLE = 0,
    G_PENDING,
    G_SCROLL,
    G_STANDBY,
} gesture_state_t;

static qmi8658_t s_imu;
static motion_t s_motion;
static bool s_imu_ok;
static float s_roll_rate, s_pitch_rate, s_yaw_rate;

static uint8_t s_hall_level[3];        /* debounced level: 1 = magnet present (OUT low) */
static uint8_t s_hall_raw_prev[3];
static uint8_t s_hall_count[3];

static gesture_state_t s_state = G_IDLE;
static float s_press_ms;
static float s_disp_deg;
static float s_wheel_accum;
static bool s_clutch = true;           /* default engaged */

static uint8_t s_buttons;              /* HID button mask */
static float s_cursor_x, s_cursor_y;

static uint16_t s_hid_conn_id;
static bool s_hid_secure;
static bool s_hid_connected;

static uint8_t s_hall_map[3] = {PIN_HALL_INDEX, PIN_HALL_MIDDLE, PIN_HALL_RING};
static int s_report_count;
static float s_gain = CURSOR_GAIN_DEFAULT;
static bool s_recenter_on = true;           /* pull cursor to screen center on engage */
static bool s_recenter_pending = false;
static bool s_hid_rel = false;
static int s_last_x = 16383;
static int s_last_y = 16383;

static void hid_send(uint8_t buttons, uint16_t x, uint16_t y, int8_t wheel)
{
    int dx = (int)x - s_last_x;
    int dy = (int)y - s_last_y;
    s_last_x = x;
    s_last_y = y;
    if (dx || dy || wheel || buttons) {
        esp_hidd_send_mouse_rel(s_hid_conn_id, buttons, (int16_t)dx, (int16_t)dy, wheel);
    }
}
static int s_screen_w = 1920;
static int s_screen_h = 1080;
static float s_abs_x = 16383.5f;   /* HID absolute position, 0..32767 */
static float s_abs_y = 16383.5f;
#define ADV_WAIT_MS (120 * 1000)   /* advertise for 2 min, then sleep */
static int64_t s_adv_since;
static bool s_auto_sleep = false;  /* dev default: no auto sleep; enable with 'sleep on' */

/* Runtime switches (serial commands). */
static bool s_ble_enabled = true;
static bool s_csv_enabled = true;
static esp_bd_addr_t s_peer_bda;
static bool s_peer_valid;
static int s_wheel_pending;

/* ---------------- BLE HID plumbing ---------------- */
#define HIDD_DEVICE_NAME "clawbot-mouse"

static uint8_t hidd_service_uuid128[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t hidd_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = ESP_BLE_GAP_CONN_ITVL_MS(7.5),
    .max_interval = ESP_BLE_GAP_CONN_ITVL_MS(20),
    .appearance = 0x03c2,  /* HID Mouse */
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(hidd_service_uuid128),
    .p_service_uuid = hidd_service_uuid128,
    .flag = 0x6,
};

static esp_ble_adv_params_t hidd_adv_params = {
    .adv_int_min = ESP_BLE_GAP_ADV_ITVL_MS(20),
    .adv_int_max = ESP_BLE_GAP_ADV_ITVL_MS(30),
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void hid_center_cursor(void)
{
    if (!(s_ble_enabled && s_hid_connected && s_hid_secure)) {
        printf("EVT,recenter,not-connected\n");
        return;
    }
    s_abs_x = 16383.5f;
    s_abs_y = 16383.5f;
    s_last_x = 16383;
    s_last_y = 16383;
    if (!s_hid_rel) {
        hid_send(s_buttons, 16383, 16383, 0);
    }
    printf("EVT,recenter,center\n");
}

static void ble_start_adv(void)
{
    if (!s_ble_enabled) {
        return;
    }
    esp_err_t err = esp_ble_gap_start_advertising(&hidd_adv_params);
    if (err == ESP_OK) {
        s_adv_since = esp_timer_get_time();
        printf("EVT,adv,1\n");
    } else {
        ESP_LOGW(TAG, "start advertising failed: %s", esp_err_to_name(err));
        printf("EVT,adv,err=%d\n", (int)err);
    }
}

static void enter_sleep(const char *why)
{
    printf("EVT,sleep,%s\n", why);
    lcd_sleep();
    esp_ble_gap_stop_advertising();
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();
}

static void hidd_event_callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch (event) {
    case ESP_HIDD_EVENT_REG_FINISH:
        if (param->init_finish.state == ESP_HIDD_INIT_OK) {
            esp_ble_gap_set_device_name(HIDD_DEVICE_NAME);
            esp_ble_gap_config_adv_data(&hidd_adv_data);
            ESP_LOGI(TAG, "HID profile registered, advertising as %s", HIDD_DEVICE_NAME);
        }
        break;
    case ESP_HIDD_EVENT_BLE_CONNECT:
        s_hid_conn_id = param->connect.conn_id;
        memcpy(s_peer_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        s_peer_valid = true;
        s_hid_connected = true;
        ESP_LOGI(TAG, "BLE connected (conn_id=%u)", s_hid_conn_id);
        printf("EVT,hid,connect\n");
        s_adv_since = 0;
        esp_ble_gap_set_prefer_conn_params(s_peer_bda, 6, 12, 0, 400);
        break;
    case ESP_HIDD_EVENT_BLE_DISCONNECT:
        s_hid_secure = false;
        s_hid_connected = false;
        ESP_LOGI(TAG, "BLE disconnected, re-advertising");
        printf("EVT,hid,disconnect\n");
        ble_start_adv();
        break;
    default:
        break;
    }
}


static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ble_start_adv();
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        s_hid_secure = param->ble_security.auth_cmpl.success;
        ESP_LOGI(TAG, "pair %s", s_hid_secure ? "ok" : "failed");
        break;
    default:
        break;
    }
}

static void ble_hid_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    esp_bt_sleep_disable();   /* keep the radio awake: fewer dropped HID reports */
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_err_t hidd_err = esp_hidd_profile_init();
    if (hidd_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_profile_init failed: %s", esp_err_to_name(hidd_err));
        printf("EVT,hidd,err=%d\n", (int)hidd_err);
    }
    esp_ble_gap_register_callback(gap_event_handler);
    esp_hidd_register_callbacks(hidd_event_callback);

    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
}

/* ---------------- hall ---------------- */
static void hall_init(void)
{
    for (int i = 0; i < 3; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_hall_map[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        s_hall_level[i] = 0;
        s_hall_raw_prev[i] = 0;
        s_hall_count[i] = 0;
    }
}

static void gesture_press(void)
{
    s_state = G_PENDING;
    s_press_ms = 0;
    s_disp_deg = 0;
    s_wheel_accum = 0;
}

static void gesture_release(void)
{
    switch (s_state) {
    case G_PENDING:
        /* short press without motion -> toggle clutch */
        if (s_press_ms < STANDBY_MS && s_disp_deg <= SCROLL_DEG) {
            s_clutch = !s_clutch;
            if (s_clutch && s_recenter_on) {
                s_recenter_pending = true;
            }
            ESP_LOGI(TAG, "clutch %s", s_clutch ? "ENGAGED" : "DISENGAGED");
        }
        break;
    case G_SCROLL:
    case G_STANDBY:
        /* release never toggles the clutch */
        break;
    default:
        break;
    }
    s_state = G_IDLE;
    s_press_ms = 0;
    s_disp_deg = 0;
}

/* returns the wheel delta to emit this tick (0 if none) */
static int8_t gesture_update(float dt_s, float roll_rate, float pitch_rate)
{
    int8_t wheel = 0;

    if (s_state == G_IDLE) {
        return 0;
    }

    float dt_ms = dt_s * 1000.0f;
    s_press_ms += dt_ms;

    float omega = sqrtf(roll_rate * roll_rate + pitch_rate * pitch_rate);
    if (s_press_ms > STABLE_MS) {
        s_disp_deg += omega * dt_s;
    }

    switch (s_state) {
    case G_PENDING:
        if (s_disp_deg > SCROLL_DEG) {
            s_state = G_SCROLL;
            s_wheel_accum = 0;
        } else if (s_press_ms > STANDBY_MS) {
            s_state = G_STANDBY;
        }
        break;
    case G_STANDBY:
        if (s_disp_deg > SCROLL_DEG) {
            s_state = G_SCROLL;
            s_wheel_accum = 0;
        }
        break;
    case G_SCROLL:
        s_wheel_accum += pitch_rate * dt_s; /* pitch -> vertical wheel */
        while (s_wheel_accum >= WHEEL_DEG_PER_TICK) {
            wheel++;
            s_wheel_accum -= WHEEL_DEG_PER_TICK;
        }
        while (s_wheel_accum <= -WHEEL_DEG_PER_TICK) {
            wheel--;
            s_wheel_accum += WHEEL_DEG_PER_TICK;
        }
        break;
    default:
        break;
    }
    return wheel;
}

static bool hall_update(uint8_t idx)
{
    uint8_t raw = gpio_get_level(s_hall_map[idx]) ? 0 : 1; /* open-drain: low = magnet */

    if (raw != s_hall_raw_prev[idx]) {
        s_hall_raw_prev[idx] = raw;
        s_hall_count[idx] = 0;
        return false;
    }
    if (s_hall_count[idx] < DEBOUNCE_FRAMES) {
        s_hall_count[idx]++;
        return false;
    }
    if (raw != s_hall_level[idx]) {
        s_hall_level[idx] = raw;
        return true;
    }
    return false;
}

/* ---------------- console CSV ---------------- */
static void csv_header(void)
{
    printf("# clawbot POC firmware, CSV fields:\n");
    printf("# t_ms,h_all,h_mid,h_ring,ax,ay,az,gx,gy,gz,clutch,state,btn,wheel,roll,pitch,yaw,bias\n");
}

/* ---------------- main task ---------------- */
static void clawbot_task(void *arg)
{
    (void)arg;
    int64_t t_ms = 0;
    int64_t last_imu = esp_timer_get_time();
    int64_t last_hall = last_imu;
    int64_t last_report = last_imu;
    bool imu_first = true;
    qmi8658_sample_t imu_sample = {0};

    /* BOOT button (GPIO0): short press = screen on/off, long press = deep sleep. */
    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot_cfg);
    bool boot_prev = false;
    int64_t boot_since = 0;

    printf("clawbot: hall GPIO %d/%d/%d, IMU %s\n",
           PIN_HALL_INDEX, PIN_HALL_MIDDLE, PIN_HALL_RING,
           s_imu_ok ? "ready" : "MISSING");

    while (true) {
        int64_t now = esp_timer_get_time();
        t_ms = now / 1000;

        /* IMU at 100 Hz */
        if ((now - last_imu) >= IMU_PERIOD_MS * 1000) {
            float dt = (now - last_imu) / 1e6f;
            last_imu = now;
            if (dt > 0.1f) {
                dt = 0.1f;
            }
            if (s_imu_ok) {
                qmi8658_raw_t raw;
                if (qmi8658_read_raw(&s_imu, &raw) == ESP_OK) {
                    qmi8658_to_sample(&raw, &imu_sample);
                    motion_update(&s_motion, &imu_sample, dt);
                    motion_rates(&s_motion, &imu_sample, &s_roll_rate, &s_pitch_rate, &s_yaw_rate);
                }
            }
            if (imu_first) {
                imu_first = false;
            } else {
                int8_t wheel = gesture_update(dt, s_roll_rate, s_pitch_rate);
                s_wheel_pending += wheel;

                /* Cursor: absolute HID position, updated only while engaged. */
                bool moved = false;
                if (s_clutch && s_state != G_SCROLL) {
                    s_abs_x += (-s_roll_rate * dt * s_gain) * 32767.0f / (float)s_screen_w;
                    s_abs_y += (s_pitch_rate * dt * s_gain) * 32767.0f / (float)s_screen_h;
                    if (s_abs_x < 0) s_abs_x = 0;
                    if (s_abs_x > 32767) s_abs_x = 32767;
                    if (s_abs_y < 0) s_abs_y = 0;
                    if (s_abs_y > 32767) s_abs_y = 32767;
                    moved = (s_roll_rate != 0.0f || s_pitch_rate != 0.0f);
                }

                if (s_ble_enabled && s_hid_connected && s_hid_secure) {
                    if (moved || wheel || s_buttons) {
                        hid_send(s_buttons,
                                                (uint16_t)s_abs_x, (uint16_t)s_abs_y, wheel);
                    }
                }
            }
        }

        /* Pairing window: no host within 2 min -> deep sleep, wake on BOOT (GPIO0 low). */
        if (s_auto_sleep && s_ble_enabled && !s_hid_connected && s_adv_since != 0 &&
            (now - s_adv_since) > (int64_t)ADV_WAIT_MS * 1000) {
            enter_sleep("timeout");
        }

        /* BOOT button: short press toggles the LCD, long press sleeps. */
        bool boot_now = (gpio_get_level(GPIO_NUM_0) == 0);
        if (boot_now && !boot_prev) {
            boot_since = now;
        } else if (!boot_now && boot_prev) {
            int64_t held_ms = (now - boot_since) / 1000;
            if (held_ms >= 1500) {
                enter_sleep("button");
            } else if (held_ms >= 40) {
                lcd_set_power(!lcd_is_on());
                printf("EVT,lcd,%d\n", lcd_is_on() ? 1 : 0);
            }
        }
        boot_prev = boot_now;

        /* Hall at 20 Hz */
        if ((now - last_hall) >= HALL_PERIOD_MS * 1000) {
            last_hall = now;
            for (int i = 0; i < 3; i++) {
                if (!hall_update(i)) {
                    continue;
                }
                switch (i) {
                case 0: /* index -> left button */
                    if (s_hall_level[0]) {
                        s_buttons |= 0x01;
                    } else {
                        s_buttons &= ~0x01;
                    }
                    break;
                case 1: /* middle -> right button */
                    if (s_hall_level[1]) {
                        s_buttons |= 0x02;
                    } else {
                        s_buttons &= ~0x02;
                    }
                    break;
                case 2: /* ring -> clutch/scroll */
                    if (s_hall_level[2]) {
                        gesture_press();
                    } else {
                        gesture_release();
                    }
                    break;
                default:
                    break;
                }
            }
        }

        /* Report + CSV at 50 Hz */
        if ((now - last_report) >= REPORT_PERIOD_MS * 1000) {
            last_report = now;

            if (s_recenter_pending) {
                s_recenter_pending = false;
                hid_center_cursor();
            }

            if (s_ble_enabled && s_hid_connected && s_hid_secure && s_buttons && !s_cursor_x && !s_cursor_y) {
                esp_hidd_send_mouse_value_ex(s_hid_conn_id, s_buttons, 0, 0, 0);
            }

            /* LCD debug page at 10 Hz. */
            if ((s_report_count++ % 5) == 0) {
                bool hall[3] = {
                    s_hall_level[0] != 0,
                    s_hall_level[1] != 0,
                    s_hall_level[2] != 0,
                };
                lcd_status_page(hall, s_clutch, (int)s_state,
                                motion_bias_mag(&s_motion), s_roll_rate, s_pitch_rate);
            }

            if (s_csv_enabled) {
            printf("CSV,%lld,%u,%u,%u,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%d,%d,%u,%d,%.2f,%.2f,%.2f,%.2f\n",
                   (long long)t_ms,
                   s_hall_level[0], s_hall_level[1], s_hall_level[2],
                   imu_sample.ax, imu_sample.ay, imu_sample.az,
                   imu_sample.gx, imu_sample.gy, imu_sample.gz,
                   s_clutch ? 1 : 0, (int)s_state, s_buttons,
                   (int)s_wheel_pending,
                   s_roll_rate, s_pitch_rate, s_yaw_rate, motion_bias_mag(&s_motion));
            s_wheel_pending = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}


static void imu_recalibrate(void)
{
    if (!s_imu_ok) {
        return;
    }
    static qmi8658_sample_t cal[100];
    int n = 0;
    for (int i = 0; i < 100; i++) {
        qmi8658_raw_t raw;
        if (qmi8658_read_raw(&s_imu, &raw) == ESP_OK) {
            qmi8658_to_sample(&raw, &cal[n++]);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    motion_init(&s_motion, cal, n);
    printf("EVT,cal,bias=%.2f\n", motion_bias_mag(&s_motion));
}

static void handle_command(const char *cmd)
{
    if (!strcmp(cmd, "help")) {
        printf("EVT,help,ble on|off|status;csv on/off;cal;gain N;screen W H;recenter on/off/now;hid test;sleep on/off/now\n");
    } else if (!strcmp(cmd, "ble on")) {
        s_ble_enabled = true;
        ble_start_adv();
        printf("EVT,ble,1\n");
    } else if (!strcmp(cmd, "ble off")) {
        s_ble_enabled = false;
        esp_ble_gap_stop_advertising();
        if (s_peer_valid) {
            esp_ble_gap_disconnect(s_peer_bda);
        }
        printf("EVT,ble,0\n");
    } else if (!strcmp(cmd, "ble status")) {
        printf("EVT,ble,%d,%d\n", s_ble_enabled ? 1 : 0, s_hid_connected ? 1 : 0);
    } else if (!strcmp(cmd, "csv on")) {
        s_csv_enabled = true;
        printf("EVT,csv,1\n");
    } else if (!strcmp(cmd, "csv off")) {
        s_csv_enabled = false;
        printf("EVT,csv,0\n");
    } else if (!strcmp(cmd, "gain")) {
        printf("EVT,gain,%.1f\n", s_gain);
    } else if (!strncmp(cmd, "gain ", 5)) {
        float v = (float)atof(cmd + 5);
        if (v < 1.0f) v = 1.0f;
        if (v > 100.0f) v = 100.0f;
        s_gain = v;
        printf("EVT,gain,%.1f\n", s_gain);
    } else if (!strcmp(cmd, "lcd on")) {
        lcd_set_power(true);
    } else if (!strcmp(cmd, "lcd off")) {
        lcd_set_power(false);
    } else if (!strcmp(cmd, "sleep on")) {
        s_auto_sleep = true;
        printf("EVT,sleep,auto-on\n");
    } else if (!strcmp(cmd, "sleep off")) {
        s_auto_sleep = false;
        printf("EVT,sleep,auto-off\n");
    } else if (!strcmp(cmd, "sleep")) {
        enter_sleep("manual");
    } else if (!strcmp(cmd, "recenter on")) {
        s_recenter_on = true;
        printf("EVT,recenter,on\n");
    } else if (!strcmp(cmd, "recenter off")) {
        s_recenter_on = false;
        printf("EVT,recenter,off\n");
    } else if (!strcmp(cmd, "recenter now")) {
        hid_center_cursor();
    } else if (!strncmp(cmd, "screen ", 7)) {
        int w = 0, h = 0;
        if (sscanf(cmd + 7, "%d %d", &w, &h) == 2 && w > 0 && h > 0) {
            s_screen_w = w;
            s_screen_h = h;
            printf("EVT,screen,%d,%d\n", w, h);
        } else {
            printf("EVT,screen,invalid\n");
        }
    } else if (!strcmp(cmd, "hid test")) {
        if (s_ble_enabled && s_hid_connected && s_hid_secure) {
            s_abs_x += 1500.0f;
            if (s_abs_x > 32767) s_abs_x = 32767;
            esp_hidd_send_mouse_abs(s_hid_conn_id, 0, (uint16_t)s_abs_x, (uint16_t)s_abs_y, 0);
            printf("EVT,hid,test\n");
        } else {
            printf("EVT,hid,not-connected\n");
        }
    } else if (!strcmp(cmd, "cal")) {
        imu_recalibrate();
    } else if (cmd[0]) {
        printf("EVT,unknown,%s\n", cmd);
    }
}

static void command_task(void *arg)
{
    (void)arg;
    char line[64];
    int len = 0;
    uint8_t ch;

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
            ESP_LOGW(TAG, "usb_serial_jtag driver install failed");
        }
    }

    printf("EVT,ready,ble on|off|status;csv on|off;cal\n");
    while (true) {
        int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(20));
        if (n == 1) {
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                line[len] = '\0';
                handle_command(line);
                len = 0;
            } else if (len < (int)sizeof(line) - 1) {
                line[len++] = (char)ch;
            }
        }
    }
}


static void battery_monitor_task(void *arg)
{
    (void)arg;
    adc_oneshot_unit_handle_t adc = NULL;
    adc_cali_handle_t cali = NULL;
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    bool ok = false;
    if (adc_oneshot_new_unit(&unit_cfg, &adc) == ESP_OK) {
        adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
        if (adc_oneshot_config_channel(adc, ADC_CHANNEL_0, &ch) == ESP_OK) {
            adc_cali_curve_fitting_config_t cal = {
                .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
            };
            adc_cali_create_scheme_curve_fitting(&cal, &cali);
            ok = true;
        }
    }
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (!ok) continue;
        int raw = 0;
        if (adc_oneshot_read(adc, ADC_CHANNEL_0, &raw) != ESP_OK) continue;
        int mv = 0;
        if (cali) adc_cali_raw_to_voltage(cali, raw, &mv);
        else mv = raw * 3300 / 4095;
        int bat_mv = mv * 3;              /* 200k / 100k divider */
        int pct = (bat_mv - 3300) * 100 / (4200 - 3300);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        esp_hidd_set_battery_level((uint8_t)pct);
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    lcd_init();
    hall_init();

    if (qmi8658_init(&s_imu, PIN_IMU_SDA, PIN_IMU_SCL) == ESP_OK) {
        s_imu_ok = true;
        /* Boot calibration: the board must be still for ~1 s. */
        imu_recalibrate();
    } else {
        ESP_LOGW(TAG, "IMU not found, running hall-only");
    }

    ble_hid_init();
    csv_header();

    xTaskCreate(clawbot_task, "clawbot", 4096, NULL, 5, NULL);
    xTaskCreate(command_task, "cmd", 3072, NULL, 4, NULL);
    xTaskCreate(battery_monitor_task, "batt", 3072, NULL, 2, NULL);
}
