/*
 * t31_sim.c —— 温感子设备模拟器（CONFIG_ONEYE_T31_ROLE_SUBDEV）
 * =============================================================================
 * 定位：把一块 ESP32-S3 变成"看起来像健奥 T31 体温贴"的 BLE 外设，用来给网关
 *       （另一块板 = COM12，甲型 OY-GAW-01）当**被测子设备**。⛔ 本角色不连云。
 *
 * ★ 空口形态（与 oneye-link-sdk/profiles/t31.json 的 match/gatt 段逐字对齐）：
 *   · 广播名   `T31-<MAC 后 2 字节 hex>`（例：`T31-3F24`；link-sdk 按 `name_prefix:"T31"` 匹配）
 *   · 服务     `0000FFF0-0000-1000-8000-00805F9B34FB`
 *              广播包放 **16-bit** FFF0（31 B 内还要放名字）；扫描响应放**完整 128-bit** UUID
 *              （让按 128 位过滤的客户端也能命中）。可连接（CONN_MODE_UND）。
 *   · GATT     service FFF0
 *                ├─ FFF1 = **notify**（READ + NOTIFY）：温度/电量帧从这里推出去；
 *                │          READ 返回"最近一帧"的原始字节（便于用 nRF Connect 手工核对）
 *                └─ FFF2 = write（WRITE + WRITE_NO_RSP，可选）：只打印，**不据此改变行为**
 *
 * ★ 帧格式（模拟器自洽口径，⚠️ **真 T31 的编码尚未解出**）：
 *      `A6 <cmd> <payload...> <checksum> 6A`
 *        · 0xA6 = SOP、0x6A = EOP
 *        · checksum = **从 cmd 起逐字节求和取低 8 位**（自定义；真机校验口径未知 ⇒ 见下）
 *        · cmd 0x01（温度）：payload = i16 **LE** 温度(0.01 ℃) + u16 **LE** 帧序号
 *        · cmd 0xB1（电量）：payload = u8 电量% + u16 **LE** 电池 mV
 *        · 🔴 2026-10-25 新增 cmd 0x02（GNSS 定位，**本仓扩展**；真 T31 没有该命令）：
 *            payload = i32 **LE** 纬度(1e-7°)、i32 **LE** 经度(1e-7°)、i16 **LE** 海拔(m)、
 *                      u16 **LE** 地速(0.01 m/s)、u8 卫星数、u8 定位类型(0 未定位/1 2D/2 3D)
 *            ⇒ payload 14 B、整帧 **18 B**（≤ BLE 默认 MTU 的 20 B 载荷上限 ⇒ 不依赖 MTU 协商）。
 *            网关（gw_child.c）解这一帧后**并进温度帧的 TLV** 上报（TLV 0x50–0x55，见
 *            oneye-link-sdk/docs/TLV-REGISTRY.md §5），云端解成 latitude/longitude/… 属性。
 *   ⚠️ 两条必须写明的口径差异（⛔ 不要当成协议事实传播）：
 *     ① **真 T31 的帧体编码（含校验和算法、字段偏移）未解出**；本实现只保证
 *        「本模拟器 → 本仓网关（含 profiles/t31.json 的模拟器覆盖档案）」两端**自洽**。
 *        接真机时若收不到温度，先怀疑这条，而不是网关的 TLV 解析。
 *     ② TLV 侧（网关 → 云）用的是 oneye-link-sdk 的**既定**编码（u16/i16 **大端**、
 *        温度 0x10 为 i16/0.01 ℃）—— 那是契约口径，与本文件的帧体 LE 是两件事。
 *
 * ★ 温度是**模拟数据**（注释见 t31_sim_temp_centi）：60 s 周期的正弦 36.0–37.5 ℃
 *   ＋ ±0.05 ℃ 随机抖动；**不是**任何真实测量值。
 * =============================================================================
 */

#include "sdkconfig.h"

#if CONFIG_ONEYE_T31_ROLE_SUBDEV

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "t31_sim.h"

#if CONFIG_ONEYE_GNSS_ENABLE
#include "gnss_atgm336x.h"
#endif

static const char *TAG = "t31sub";

/* ---------------------------------------------------------------- 常量 */

#define T31_SIM_SVC_UUID16   0xFFF0u
#define T31_SIM_NOTIFY_UUID16 0xFFF1u
#define T31_SIM_WRITE_UUID16  0xFFF2u

#define T31_FRAME_SOP        0xA6u
#define T31_FRAME_EOP        0x6Au
#define T31_CMD_TEMP         0x01u
#define T31_CMD_BATT         0xB1u
#define T31_CMD_GPS          0x02u   /* 🔴 2026-10-25 本仓扩展：GNSS 定位（见文件头） */
#define T31_CMD_TLV          0x03u   /* 🔴 2026-10-28 本仓扩展：通用 TLV 透传（网关不解析标签） */

#define T31_SIM_PI           3.14159265f
/* 模拟温度区间（℃ ×100）：36.00 – 37.50（用户口径） */
#define T31_SIM_TEMP_MIN_CENTI 3600
#define T31_SIM_TEMP_MAX_CENTI 3750

/* ---------------------------------------------------------------- 状态 */

static char     s_name[16] = "T31-0000";      /* 广播名 T31-xxxx */
static uint8_t  s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_notify_handle;              /* FFF1 的 value handle（sync 之后查得） */
static bool     s_subscribed;                 /* 中心是否已订阅 FFF1 的 notify（CCCD） */
static uint8_t  s_last_frame[16];             /* 最近一帧（FFF1 的 READ 返回它） */
static size_t   s_last_frame_len;
static TaskHandle_t s_report_task;

/* GAP 事件回调（定义在文件后半；广播启动时要把它交出去 ⇒ 先声明） */
static int t31_gap_event(struct ble_gap_event *event, void *arg);

/* ------------------------------------------------------------ 帧编码 */

/*
 * 组一帧：`A6 <cmd> <payload...> <checksum> 6A`。
 * checksum = 从 cmd 起逐字节求和取低 8 位（⚠️ 模拟器自洽口径，真 T31 校验算法未解出）。
 * 返回整帧字节数；缓冲不足返回 0。
 */
static size_t t31_build_frame(uint8_t cmd, const uint8_t *payload, size_t plen,
                              uint8_t *out, size_t cap)
{
    size_t i = 0;
    uint8_t sum = cmd;

    if (out == NULL || cap < plen + 4u) {
        return 0;
    }
    out[i++] = (uint8_t)T31_FRAME_SOP;
    out[i++] = cmd;
    for (size_t k = 0; k < plen; k++) {
        out[i++] = payload[k];
        sum = (uint8_t)(sum + payload[k]);
    }
    out[i++] = sum;                       /* checksum（低 8 位） */
    out[i++] = (uint8_t)T31_FRAME_EOP;
    return i;
}

/* -------------------------------------------------------- 模拟温度 */

/*
 * ★ **模拟数据**（⛔ 不是测量值、⛔ 不是 T31 的真值）：
 *   60 s 一个周期的正弦，映射到 36.00 – 37.50 ℃，再叠加 ±0.05 ℃ 的随机抖动
 *   （抖动用 esp_random()，避免"看起来像真的采样"的死板曲线）。
 *   返回 0.01 ℃ 为单位的温度。
 */
static int16_t t31_sim_temp_centi(void)
{
    static uint32_t elapsed_ms;
    float phase;
    float wave;
    float noise;
    float c;

    elapsed_ms += (uint32_t)CONFIG_ONEYE_T31_SUB_TEMP_REPORT_MS;
    phase = (float)(elapsed_ms % 60000u) / 60000.0f;
    wave = 0.5f + 0.5f * sinf(2.0f * T31_SIM_PI * phase);       /* 0 .. 1 */
    noise = ((float)((int)(esp_random() % 11u) - 5)) / 100.0f;  /* ±0.05 ℃ */
    c = 36.0f + 0.75f * wave + noise;
    if (c < 36.0f) {
        c = 36.0f;
    }
    if (c > 37.5f) {
        c = 37.5f;
    }
    return (int16_t)(c * 100.0f + 0.5f);
}

/* ------------------------------------------------------------ 推送 */

static void t31_notify(const uint8_t *buf, size_t len)
{
    struct os_mbuf *om;
    int rc;

    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_notify_handle == 0) {
        return;
    }
    om = ble_hs_mbuf_from_flat(buf, (uint16_t)len);
    if (om == NULL) {
        ESP_LOGW(TAG, "notify 组包失败（mbuf 不足）⇒ 本帧丢弃");
        return;
    }
    rc = ble_gatts_notify_custom(s_conn, s_notify_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify 失败 rc=%d（中心可能已断开）", rc);
    }
}

/*
 * 🔴 2026-10-25：GNSS 定位帧（cmd 0x02）—— 本仓对 T31 模拟器的**扩展命令**。
 * 🔴 2026-10-28：新增 **cmd 0x03 通用 TLV 透传帧**；`gnssState` 由子设备直接编成 TLV
 *    （`0x56 01 state`），网关**不解析标签**，只把这段 TLV 原样并进 `sub.telemetry`
 *    ⇒ 新物模型属性只要子设备会编 TLV，网关固件不用改。
 *
 * 为什么单独一帧、而不是并进温度帧：
 *   · 温度帧 8 B / GPS 帧 18 B / TLV 帧 7 B，都 ≤ BLE 默认 MTU 的 20 B 载荷上限 ⇒ **不依赖 MTU 协商**；
 *   · 定位 1 Hz、温度 2 s 一帧，两者节奏不同（且室内可能整场无有效定位）；
 *   · 网关侧把最近一次定位/TLV **并进温度帧的 TLV** 上报（见 gw_child.c），云端无温度缺帧风险。
 *
 * ⚠️ 0x02 payload 全部 **LE**；0x03 payload 是**与云端 TLV 同字节序（大端）**的原始标签流。
 */
#if CONFIG_ONEYE_GNSS_ENABLE
static void t31_emit_gps(void)
{
    static unsigned gps_no;
    gnss_fix_t g;
    uint8_t pl[14];
    uint8_t frame[24];
    int32_t lat = 0;
    int32_t lon = 0;
    int16_t alt = 0;
    uint16_t spd = 0;
    uint8_t sats;
    uint8_t fix;
    bool has;
    size_t len;

    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
        return;   /* 与温度同口径：无连接/未订阅不推送（只由温度帧留日志） */
    }

    has = gnss_get(&g);
    sats = has ? g.sats : 0u;
    fix = (has && g.fix) ? g.fix_type : 0u;
    if (has && g.fix) {
        lat = g.lat_e7;
        lon = g.lon_e7;
        alt = g.alt_m;
        spd = g.speed_cms;
    }

    pl[0] = (uint8_t)((uint32_t)lat & 0xFFu);
    pl[1] = (uint8_t)(((uint32_t)lat >> 8) & 0xFFu);
    pl[2] = (uint8_t)(((uint32_t)lat >> 16) & 0xFFu);
    pl[3] = (uint8_t)(((uint32_t)lat >> 24) & 0xFFu);
    pl[4] = (uint8_t)((uint32_t)lon & 0xFFu);
    pl[5] = (uint8_t)(((uint32_t)lon >> 8) & 0xFFu);
    pl[6] = (uint8_t)(((uint32_t)lon >> 16) & 0xFFu);
    pl[7] = (uint8_t)(((uint32_t)lon >> 24) & 0xFFu);
    pl[8] = (uint8_t)((uint16_t)alt & 0xFFu);
    pl[9] = (uint8_t)(((uint16_t)alt >> 8) & 0xFFu);
    pl[10] = (uint8_t)(spd & 0xFFu);
    pl[11] = (uint8_t)((spd >> 8) & 0xFFu);
    pl[12] = sats;
    pl[13] = fix;

    len = t31_build_frame(T31_CMD_GPS, pl, sizeof(pl), frame, sizeof(frame));
    if (len == 0) {
        return;
    }
    t31_notify(frame, len);
    gps_no++;
    if ((gps_no % 5u) == 1u) {
        ESP_LOGI(TAG, "→ notify GPS 帧 #%u：fix=%u sats=%u lat=%.7f lon=%.7f alt=%d m"
                      "（%u B；室内 fix=0 属正常，见 gnss_atgm336x.h）",
                 gps_no, (unsigned)fix, (unsigned)sats,
                 (double)lat / 1e7, (double)lon / 1e7, (int)alt, (unsigned)len);
    }
}

/*
 * 通用 TLV 透传帧（cmd 0x03）：payload 就是云端 `sub.telemetry` 的 TLV 字节流（大端）。
 * 当前只放 `0x56 01 <state>`；以后新增属性只要在这里 append 标签，**网关固件不用改**。
 * 帧上限 20 B ⇒ TLV ≤ 16 B（本帧 7 B）。
 */
static void t31_emit_tlv(void)
{
    static unsigned tlv_no;
    gnss_fix_t g;
    uint8_t tlv[16];
    uint8_t frame[24];
    size_t n = 0;
    size_t len;
    uint8_t state;

    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
        return;
    }
    state = gnss_get(&g) ? g.state : (uint8_t)GNSS_STATE_NO_DATA;

    tlv[n++] = 0x56u;   /* GNSS_STATE */
    tlv[n++] = 1u;
    tlv[n++] = state;

    len = t31_build_frame(T31_CMD_TLV, tlv, n, frame, sizeof(frame));
    if (len == 0) {
        return;
    }
    t31_notify(frame, len);
    tlv_no++;
    if ((tlv_no % 10u) == 1u) {
        ESP_LOGI(TAG, "→ notify TLV 帧 #%u：0x56 gnssState=%u（%u B；网关原样转发）",
                 tlv_no, (unsigned)state, (unsigned)len);
    }
}
#endif /* CONFIG_ONEYE_GNSS_ENABLE */


static void t31_emit_once(void)
{
    static uint16_t seq;
    static unsigned frame_no;
    static uint8_t battery_pct = 92;      /* 模拟电量：缓慢下降，仅用于展示链路 */

    uint8_t payload[4];
    uint8_t frame[16];
    int16_t centi;
    size_t len;
    bool online = (s_conn != BLE_HS_CONN_HANDLE_NONE);

#if CONFIG_ONEYE_GNSS_ENABLE
    /* 定位帧先发（独立 cmd/独立缓冲）：温度帧仍是 FFF1 READ 的"最近一帧" */
    t31_emit_gps();
    /* 通用 TLV 再发：gnssState 等新属性由子设备编 TLV，网关不解析标签 */
    t31_emit_tlv();
#endif

    centi = t31_sim_temp_centi();
    payload[0] = (uint8_t)((uint16_t)centi & 0xFFu);          /* i16 LE 低字节 */
    payload[1] = (uint8_t)(((uint16_t)centi >> 8) & 0xFFu);   /* i16 LE 高字节 */
    payload[2] = (uint8_t)(seq & 0xFFu);                      /* u16 LE 序号低 */
    payload[3] = (uint8_t)((seq >> 8) & 0xFFu);               /* u16 LE 序号高 */
    seq++;

    len = t31_build_frame(T31_CMD_TEMP, payload, sizeof(payload), frame, sizeof(frame));
    if (len == 0) {
        return;
    }
    memcpy(s_last_frame, frame, len);
    s_last_frame_len = len;
    frame_no++;

    if (!online || !s_subscribed) {
        /* 无连接 / 中心还没订阅 notify ⇒ 不推送（只留日志，便于串口自证"我在产生数据"） */
        ESP_LOGI(TAG, "无连接或未订阅 ⇒ **不推送**（本次温度 %.2f ℃，帧 %u B 已生成）",
                 (double)centi / 100.0, (unsigned)len);
        return;
    }

    t31_notify(s_last_frame, s_last_frame_len);
    ESP_LOGI(TAG, "→ notify 温度帧 #%u：%.2f ℃（cmd=0x01，%u B；%.2f ℃ 为**模拟数据**）",
             frame_no, (double)centi / 100.0, (unsigned)len, (double)centi / 100.0);

    if ((frame_no % 10u) == 0u) {
        uint8_t bpl[3];
        uint8_t bf[16];
        size_t blen;
        uint16_t mv = (uint16_t)(3000u + (unsigned)battery_pct * 8u);

        bpl[0] = battery_pct;
        bpl[1] = (uint8_t)(mv & 0xFFu);
        bpl[2] = (uint8_t)((mv >> 8) & 0xFFu);
        blen = t31_build_frame(T31_CMD_BATT, bpl, sizeof(bpl), bf, sizeof(bf));
        if (blen > 0) {
            t31_notify(bf, blen);
            ESP_LOGI(TAG, "→ notify 电量帧：%u %% / %u mV（cmd=0xB1，%u B）",
                     (unsigned)battery_pct, (unsigned)mv, (unsigned)blen);
        }
        if (battery_pct > 5u) {
            battery_pct--;                /* 每 10 帧（默认 20 s）掉 1%，纯展示用 */
        }
    }
}

/* 周期任务：等"周期到"或"连接/订阅事件"的即时通知；两种情况都推一帧。
 * 用通知而不用队列：连接后立即推一帧是**事件**，周期推是**定时**，二者语义都是"该推了"。 */
static void t31_report_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_ONEYE_T31_SUB_TEMP_REPORT_MS));
        t31_emit_once();
    }
}

/* ------------------------------------------------------------ GATT */

static int t31_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        /* FFF1 的 READ：回"最近一帧"的原始字节（不发新帧、不改状态） */
        int rc = os_mbuf_append(ctxt->om, s_last_frame, (uint16_t)s_last_frame_len);
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        /* FFF2 下行：只打印长度。真 T31 的下行命令编码未解出 ⇒ 模拟器**不据此改变行为**
         * （⛔ 不假装支持"改周期/校准"，否则网关侧会以为下发成功）。 */
        ESP_LOGI(TAG, "收到下行写（fff2，%u B）：模拟器不解析（真 T31 下行未解出）",
                 (unsigned)OS_MBUF_PKTLEN(ctxt->om));
        return 0;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* service FFF0 / chr FFF1(notify) / chr FFF2(write) */
static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(T31_SIM_SVC_UUID16),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(T31_SIM_NOTIFY_UUID16),
                .access_cb = t31_gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(T31_SIM_WRITE_UUID16),
                .access_cb = t31_gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ------------------------------------------------------- 广播 / 事件 */

static void t31_advertise(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp;
    struct ble_gap_adv_params params;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(T31_SIM_SVC_UUID16) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.name = (uint8_t *)s_name;
    fields.name_len = (uint8_t)strlen(s_name);
    fields.name_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields 失败 rc=%d", rc);
        return;
    }

    /* 扫描响应：完整 128-bit 服务 UUID（0000FFF0-0000-1000-8000-00805F9B34FB 的**小端**字节序） */
    memset(&rsp, 0, sizeof(rsp));
    rsp.uuids128 = (ble_uuid128_t[]) { BLE_UUID128_INIT(
        0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x10, 0x00, 0x00, 0xF0, 0xFF, 0x00, 0x00) };
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    (void)ble_gap_adv_rsp_set_fields(&rsp);

    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;    /* 可连接 */
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;    /* 通用可发现 */
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, t31_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start 失败 rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "广播中：name=%s / 服务 0000FFF0… / FFF1 notify / FFF2 write（可连接）", s_name);
    }
}

static int t31_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_subscribed = false;
            ESP_LOGI(TAG, "中心已连接（conn=%u）⇒ 立即推一帧温度",
                     (unsigned)s_conn);
            if (s_report_task != NULL) {
                (void)xTaskNotifyGive(s_report_task);   /* 连接后立即推一帧 */
            }
        } else {
            ESP_LOGW(TAG, "连接失败 status=%d ⇒ 重新广播", event->connect.status);
            t31_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "中心已断开 reason=%d ⇒ 重新广播", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        t31_advertise();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        t31_advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        s_subscribed = (event->subscribe.cur_notify != 0);
        ESP_LOGI(TAG, "notify 订阅=%d（attr=%u）⇒ %s",
                 s_subscribed ? 1 : 0, (unsigned)event->subscribe.attr_handle,
                 s_subscribed ? "开始按周期推送" : "停止推送");
        if (s_subscribed && s_report_task != NULL) {
            (void)xTaskNotifyGive(s_report_task);
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 协商为 %u", (unsigned)event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

/* ---------------------------------------------------------- NimBLE */

static void t31_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE 复位 reason=%d（host 将重新同步）", reason);
}

static void t31_on_sync(void)
{
    int rc;
    uint16_t val_handle = 0;

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr 失败 rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto 失败 rc=%d", rc);
        return;
    }

    {
        ble_uuid16_t svc = BLE_UUID16_INIT(T31_SIM_SVC_UUID16);
        ble_uuid16_t chr = BLE_UUID16_INIT(T31_SIM_NOTIFY_UUID16);
        if (ble_gatts_find_chr(&svc.u, &chr.u, NULL, &val_handle) == 0) {
            s_notify_handle = val_handle;
        } else {
            ESP_LOGE(TAG, "找不到 FFF1 的 value handle ⇒ notify 将不可用");
        }
    }

    ble_svc_gap_device_name_set(s_name);

    if (s_report_task == NULL) {
        if (xTaskCreate(t31_report_task, "t31_report", 3072, NULL, 4, &s_report_task) != pdPASS) {
            s_report_task = NULL;
            ESP_LOGE(TAG, "温度推送任务创建失败（内存不足）⇒ 只广播、不推数据");
        }
    }

    t31_advertise();
    ESP_LOGI(TAG, "★ 子设备就绪：%s（模拟健奥 T31；周期 %d ms；⛔ 不连云）",
             s_name, (int)CONFIG_ONEYE_T31_SUB_TEMP_REPORT_MS);
}

static void t31_host_task(void *param)
{
    (void)param;
    nimble_port_run();                 /* 返回 = host 已停止 */
    nimble_port_freertos_deinit();
}

/* 广播名 = `T31-<MAC 后 2 字节 hex>`（例 T31-3F24） */
static void t31_build_name(void)
{
    uint8_t mac[6] = { 0 };

    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        ESP_LOGW(TAG, "读 BT MAC 失败 ⇒ 广播名用占位 %s", s_name);
        return;
    }
    (void)snprintf(s_name, sizeof(s_name), "T31-%02X%02X", mac[4], mac[5]);
}

esp_err_t t31_sim_start(void)
{
    esp_err_t err;
    int rc;

    t31_build_name();
    ESP_LOGI(TAG, "==== 温感子设备模拟器：%s（BLE 外设；⛔ 不连云）====", s_name);
    ESP_LOGI(TAG, "服务 FFF0 / notify FFF1 / write FFF2；帧 A6 cmd payload checksum 6A；"
                  "温度帧 cmd=0x01（i16 LE 0.01℃ + u16 LE 序号）");

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败：%s（NimBLE 是否已在别处起栈？）", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = t31_on_reset;
    ble_hs_cfg.sync_cb = t31_on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg 失败 rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs 失败 rc=%d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_device_name_set(s_name);
    nimble_port_freertos_init(t31_host_task);
    return ESP_OK;
}

#endif /* CONFIG_ONEYE_T31_ROLE_SUBDEV */
