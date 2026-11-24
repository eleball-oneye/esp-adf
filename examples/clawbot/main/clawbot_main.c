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
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
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
#define CURSOR_GAIN         1.2f    /* pixels per degree, tune on the bench */
#define CURSOR_MAX_STEP     40.0f

/* Set to 1 to print the CSV stream, 0 to keep the console quiet. */
#define CSV_ENABLE          1

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
    .appearance = 0x03c0,  /* HID Generic */
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
        s_hid_connected = true;
        ESP_LOGI(TAG, "BLE connected (conn_id=%u)", s_hid_conn_id);
        break;
    case ESP_HIDD_EVENT_BLE_DISCONNECT:
        s_hid_secure = false;
        s_hid_connected = false;
        ESP_LOGI(TAG, "BLE disconnected, re-advertising");
        esp_ble_gap_start_advertising(&hidd_adv_params);
        break;
    default:
        break;
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&hidd_adv_params);
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
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_hidd_profile_init());
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

                /* Cursor: only when clutch is engaged and not scrolling. */
                if (s_clutch && s_state != G_SCROLL) {
                    s_cursor_x += s_roll_rate * dt * CURSOR_GAIN;
                    s_cursor_y += s_pitch_rate * dt * CURSOR_GAIN;
                } else {
                    s_cursor_x = s_cursor_y = 0;
                }

                if (s_hid_connected && s_hid_secure) {
                    int8_t dx = 0, dy = 0;
                    if (s_cursor_x >= 1.0f || s_cursor_x <= -1.0f) {
                        dx = (int8_t)fmaxf(-CURSOR_MAX_STEP, fminf(CURSOR_MAX_STEP, s_cursor_x));
                        s_cursor_x -= dx;
                    }
                    if (s_cursor_y >= 1.0f || s_cursor_y <= -1.0f) {
                        dy = (int8_t)fmaxf(-CURSOR_MAX_STEP, fminf(CURSOR_MAX_STEP, s_cursor_y));
                        s_cursor_y -= dy;
                    }
                    if (dx || dy || wheel || s_buttons) {
                        esp_hidd_send_mouse_value_ex(s_hid_conn_id, s_buttons, dx, dy, wheel);
                    }
                }
            }
        }

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

            if (s_hid_connected && s_hid_secure && s_buttons && !s_cursor_x && !s_cursor_y) {
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

#if CSV_ENABLE
            printf("CSV,%lld,%u,%u,%u,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%d,%d,%u,0,%.2f,%.2f,%.2f,%.2f\n",
                   (long long)t_ms,
                   s_hall_level[0], s_hall_level[1], s_hall_level[2],
                   imu_sample.ax, imu_sample.ay, imu_sample.az,
                   imu_sample.gx, imu_sample.gy, imu_sample.gz,
                   s_clutch ? 1 : 0, (int)s_state, s_buttons,
                   s_roll_rate, s_pitch_rate, s_yaw_rate, motion_bias_mag(&s_motion));
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(2));
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
        ESP_LOGI(TAG, "IMU calibrated: bias=%.2f dps, up=(%.2f,%.2f,%.2f)",
                 motion_bias_mag(&s_motion), s_motion.n[0], s_motion.n[1], s_motion.n[2]);
    } else {
        ESP_LOGW(TAG, "IMU not found, running hall-only");
    }

    ble_hid_init();
    csv_header();

    xTaskCreate(clawbot_task, "clawbot", 4096, NULL, 5, NULL);
}
