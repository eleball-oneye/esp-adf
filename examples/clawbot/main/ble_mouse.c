#include "ble_mouse.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_mouse";

static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_report_handle;
static uint16_t s_boot_handle;
static bool s_report_sub;
static bool s_boot_sub;
static ble_mouse_callbacks_t s_cb;

/* Report ID 1: buttons, dx, dy, wheel (relative). */
static const uint8_t s_report_map[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0
};

static uint8_t s_hid_info[4] = { 0x11, 0x01, 0x00, 0x03 };
static uint8_t s_protocol = 0x01; /* report protocol */
static const uint8_t s_report_ref[2] = { 0x01, 0x01 };
static const uint8_t s_manufacturer[] = "ELEBALL";
static const uint8_t s_model[] = "EB-CB01";
static const uint8_t s_pnp[] = { 0x02, 0xEB, 0x00, 0x01, 0xCB, 0x00, 0x07 };
static uint8_t s_battery = 100;

static uint8_t s_report[5] = { 1, 0, 0, 0, 0 };
static uint8_t s_boot[3] = { 0, 0, 0 };

static int hid_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);
static int dis_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);
static int cfg_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = hid_access, .arg = (void *)0x2A4A, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = hid_access, .arg = (void *)0x2A4B, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = hid_access, .arg = (void *)0x2A4C, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = hid_access, .arg = (void *)0x2A4E, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access, .arg = (void *)0x2A4D, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = BLE_UUID16_DECLARE(0x2A33), .access_cb = hid_access, .arg = (void *)0x2A33, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { 0 }
        }
    },
    {
        /* vendor config: write [0x01, gain_lo, gain_hi] or [0x02, action] */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFFF0),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0xFFF1), .access_cb = cfg_access, .arg = (void *)0xFFF1,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { 0 }
        }
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { 0 }
        }
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A24), .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = dis_access, .flags = BLE_GATT_CHR_F_READ },
            { 0 }
        }
    },
    { 0 }
};

static int hid_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch ((uintptr_t)arg) {
    case 0x2A4A:
        return os_mbuf_append(ctxt->om, s_hid_info, sizeof(s_hid_info)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    case 0x2A4B:
        return os_mbuf_append(ctxt->om, s_report_map, sizeof(s_report_map)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    case 0x2A4E:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            return os_mbuf_append(ctxt->om, &s_protocol, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && OS_MBUF_PKTLEN(ctxt->om) >= 1) {
            os_mbuf_copydata(ctxt->om, 0, 1, &s_protocol);
        }
        return 0;
    case 0x2A4C:
        return 0;
    case 0x2A4D:
        return os_mbuf_append(ctxt->om, s_report, sizeof(s_report)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    case 0x2A33:
        return os_mbuf_append(ctxt->om, s_boot, sizeof(s_boot)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int dis_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch ((uintptr_t)arg) {
    case 0x2A29: return os_mbuf_append(ctxt->om, s_manufacturer, sizeof(s_manufacturer) - 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    case 0x2A24: return os_mbuf_append(ctxt->om, s_model, sizeof(s_model) - 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    case 0x2A50: return os_mbuf_append(ctxt->om, s_pnp, sizeof(s_pnp)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    case 0x2A19: return os_mbuf_append(ctxt->om, &s_battery, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    default: return BLE_ATT_ERR_UNLIKELY;
    }
}

static void on_gatt_register(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_REGISTER_OP_CHR && ctxt->chr.chr_def->uuid->type == BLE_UUID_TYPE_16) {
        uint16_t u = ((ble_uuid16_t *)ctxt->chr.chr_def->uuid)->value;
        if (u == 0x2A4D) s_report_handle = ctxt->chr.val_handle;
        if (u == 0x2A33) s_boot_handle = ctxt->chr.val_handle;
    }
}

static int cfg_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || OS_MBUF_PKTLEN(ctxt->om) < 2) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint8_t buf[4] = {0};
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof(buf)) len = sizeof(buf);
    os_mbuf_copydata(ctxt->om, 0, len, buf);
    if (buf[0] == 0x01 && len >= 3) {
        uint16_t gain = (uint16_t)(buf[1] | (buf[2] << 8));
        ESP_LOGI(TAG, "cfg gain=%u", gain);
        if (s_cb.on_config) s_cb.on_config(gain, 0xFF);
    } else if (buf[0] == 0x02 && len >= 2) {
        ESP_LOGI(TAG, "cfg action=%u", buf[1]);
        if (s_cb.on_config) s_cb.on_config(0, buf[1]);
    }
    return 0;
}

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            if (s_cb.on_connect) s_cb.on_connect(true);
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_report_sub = s_boot_sub = false;
        if (s_cb.on_connect) s_cb.on_connect(false);
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe handle=%u notify=%d", event->subscribe.attr_handle, event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == s_report_handle) {
            s_report_sub = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_boot_handle) {
            s_boot_sub = event->subscribe.cur_notify;
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = 0x03C2;
    fields.appearance_is_present = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr rc=%d", rc);
        return;
    }
    advertise();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_mouse_init(ble_mouse_callbacks_t cb)
{
    s_cb = cb;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init err=%d", err);
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = on_gatt_register;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("clawbot-mouse");
    ble_svc_gap_device_appearance_set(0x03C2);

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt add rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "HID report handle=%u boot=%u", s_report_handle, s_boot_handle);

    nimble_port_freertos_init(host_task);
}

bool ble_mouse_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

void ble_mouse_send(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    if (dx > 127) dx = 127;
    if (dx < -127) dx = -127;
    if (dy > 127) dy = 127;
    if (dy < -127) dy = -127;

    s_report[0] = 1;
    s_report[1] = buttons;
    s_report[2] = (uint8_t)(int8_t)dx;
    s_report[3] = (uint8_t)(int8_t)dy;
    s_report[4] = (uint8_t)wheel;

    if (s_report_sub) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s_report, sizeof(s_report));
        if (om) ble_gatts_notify_custom(s_conn, s_report_handle, om);
    }
    if (s_boot_sub) {
        s_boot[0] = buttons;
        s_boot[1] = (uint8_t)(int8_t)dx;
        s_boot[2] = (uint8_t)(int8_t)dy;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s_boot, sizeof(s_boot));
        if (om) ble_gatts_notify_custom(s_conn, s_boot_handle, om);
    }
}

void ble_mouse_set_battery(uint8_t level)
{
    s_battery = level;
}

void ble_mouse_stop_advertising(void)
{
    ble_gap_adv_stop();
}

void ble_mouse_start_advertising(void)
{
    advertise();
}
