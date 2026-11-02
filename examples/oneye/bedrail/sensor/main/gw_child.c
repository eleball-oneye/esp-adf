/*
 * gw_child.c —— 子设备绑定（child.pair / child.unpair）+ sub.* 事件上报
 * =============================================================================
 * 事实源（⛔ 不要在本文件另造一套）：
 *   · 事件面定义：components/oneye-link-sdk/docs/PROTOCOL.md §3（`sub.*` 的 data 字段）
 *   · 命令面：`command/down` → `ControlCommand`（required `[id, params]`，**RAW 不套信封**）
 *             `command/up`   → `ControlAck`（`status ∈ {ok,error}`），由
 *             `oneye_dev_base_ack_command()` 发送（oneye_dev_base.h:250-253）
 *   · 档案运行时注册：`oneye_link_profile_register_json()`（oneye_link_profile.h:27）
 *                     / `oneye_link_profile_validate()`（:37）/ `oneye_link_profile_unregister()`（:30）
 *
 * ★ 为什么绑定要落 NVS 而不是只留内存：`child.pair` 是**云端**发起的动作，网关重启后
 *   必须仍然知道「哪个 node_id 对应哪个 model / BLE 地址 / 档案」，否则重启即失联；
 *   档案同理（link-sdk 的档案是**运行时**表，重启即空）。
 *
 * ★ 载荷口径（用户 2026-10-23 逐字给定）：
 *   sub.online    {"node","model","rssi"}
 *   sub.offline   {"node","model"}
 *   sub.telemetry {"node","model","rssi","cmd","tlv"}   （本模块另加可选的 raw / trunc）
 *   sub.error     {"node","model","err"}
 *   ⚠️ PROTOCOL.md §3 把子设备标识那段叫 `sub`（值是 BLE 地址）——本模块按用户裁定发 `node`
 *      （= 配对时的 childNode，设备的**云端 node_id**），两者语义不同，**不要混**。
 * =============================================================================
 */

#include "sdkconfig.h"

#if CONFIG_ONEYE_T31_ROLE_GATEWAY

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#include "oneye_dev_sdk.h"   /* base（ack_command / strerror）+ event（event/up）+ types */
#include "oneye_link.h"      /* link-sdk：档案注册/注销、主动断开、地址工具 */
#include "oneye_link_profile.h"
#include "oneye_link_tlv.h"  /* ONEYE_TLV_RAW_FRAME（子设备 GPS 帧的透传 TLV 兜底路径） */

#include "gw_child.h"

static const char *TAG = "gw_child";

/* ---------------------------------------------------------------- 常量 */

/* NVS 命名空间（**本模块独占**；与 wifi_prov.c 的 onEye_wifi 命名空间无关） */
#define GW_CHILD_NVS_NS        "oneye_child"
#define GW_CHILD_NVS_KEY_CNT   "n"

/* 绑定表上限：与 oneye_link_config_t.max_links（5，见 main.c）同量级 —— 不要超 */
#define GW_CHILD_MAX           5

#define GW_CHILD_NODE_MAX      40
#define GW_CHILD_PK_MAX        33
#define GW_CHILD_MODEL_MAX     33
#define GW_CHILD_ADDR_MAX      18
#define GW_CHILD_PROFILE_MAX   1024    /* linkProfile JSON 上限；NVS 单串上限 4000 B，留足余量 */
#define GW_CHILD_ID_MAX        40      /* 命令 id（契约由服务端生成，uuid 36 B） */

/* 事件里带的原始字节上限（T31 的 notify 帧实测 ≤ 20 B；TLV 上限 512 B 只是保护值） */
#define GW_CHILD_TLV_CAP       64
#define GW_CHILD_RAW_CAP       32
/* 🔴 2026-10-23：sub.telemetry 上报的 tlv 十六进制上限（字节数）—— 超过就截断并带 trunc 标记。
 *   事件库/云端对单条 data 有长度约束（真机 `invalid argument`）⇒ 只发可解码的最小集，
 *   `raw` 不再随 telemetry 上报（TLV 自带的 RAW_FRAME(0x80) 已覆盖"云端换解析器"的需求）。
 * 🔴 2026-10-25：32 → **64**（旧值逐字留痕，⛔ 不抹：`#define TELE_TLV_MAX           32u`）。
 *   原因：GNSS 定位并进温度帧 TLV 后总长 = 温度帧 TLV 18 B + GPS 26 B = **44 B**；
 *   32 B 会把 GPS 标签整段截掉（截断标记不补发 ⇒ 云端永远收不到纬度）。 */
#define TELE_TLV_MAX           64u

/* GNSS 定位帧（子设备 cmd=0x02）与它并进 TLV 的标签/长度 —— 与
 * oneye-link-sdk/docs/TLV-REGISTRY.md §5 的 0x50–0x55 逐字同源。 */
#define GW_GPS_FRAME_LEN       18u   /* A6 02 <14 B payload> checksum 6A */
#define GW_GPS_PAYLOAD_LEN     14u
#define GW_GPS_FRESH_MS        30000 /* 30 s 内的定位才并进 telemetry（模组 1 Hz） */
#define GW_TLV_GNSS_LAT        0x50u /* i32 BE 1e-7° */
#define GW_TLV_GNSS_LON        0x51u /* i32 BE 1e-7° */
#define GW_TLV_GNSS_ALT        0x52u /* i16 BE m */
#define GW_TLV_GNSS_SPEED      0x53u /* u16 BE 0.01 m/s */
#define GW_TLV_GNSS_SATS       0x54u /* u8 颗 */
#define GW_TLV_GNSS_FIX        0x55u /* u8 0=未定位 1=2D 2=3D */
#define GW_TLV_GNSS_BYTES      26u   /* 有定位时的全部 GPS 标签字节数（6+6+4+4+3+3） */

/* 通用 TLV 透传（子设备 cmd=0x03）：网关**不解析标签**，只缓存并原样并进 telemetry。
 * 这样新增物模型属性（如 gnssState 0x56）只改子设备 + 云端 TSL，⛔ 不用改网关固件。 */
#define GW_CMD_TLV             0x03u
#define GW_EXTRA_TLV_CAP       64u
#define GW_EXTRA_TLV_FRESH_MS  30000

/* 队列深度：命令很罕见（配对/解绑），子设备事件每 2 s 一帧 */
#define GW_CHILD_CMD_Q_DEPTH   2
/* 🔴 2026-10-23 真机：队列 8 起来后出现 `子设备事件队列满`（上报任务没出队时积压 16 s）。
 * 旧值逐字留痕（⛔ 不抹）：`#define GW_CHILD_SUB_Q_DEPTH   8`
 * 现 = 4：**宁丢不堵**（帧是 2 s 一帧的遥测，丢几帧不影响云端趋势；堵住会连命令回执一起拖死）。 */
#define GW_CHILD_SUB_Q_DEPTH   4

/* 工作任务：栈 4096（要跑 NVS 写 + 档案 JSON 解析）；优先级与 gw_state 同级 */
/* 🔴 2026-10-23 真机：4096 会 `A stack overflow in task gw_child`（组帧/JSON/hex 全在本任务）。
 * 旧值逐字留痕（⛔ 不抹）：`#define GW_CHILD_TASK_STACK    4096`
 * 现 = 8192（与 link/event 回调侧解耦；本任务只做解析/上报，8192 足够且仍在内部 RAM 富余区）。 */
#define GW_CHILD_TASK_STACK    8192
#define GW_CHILD_TASK_PRIO     4
#define GW_CHILD_POLL_MS       50

/* 事件类型（PROTOCOL.md §3 的**逐字**取值；oneye_dev_event.h 只登记了 alarm 那一组） */
#define GW_EVT_SUB_ONLINE      "sub.online"
#define GW_EVT_SUB_OFFLINE     "sub.offline"
#define GW_EVT_SUB_TELEMETRY   "sub.telemetry"
#define GW_EVT_SUB_ERROR       "sub.error"

/* 服务名（用户 2026-10-23 给定的 params.service 取值） */
#define GW_SVC_PAIR            "child.pair"
#define GW_SVC_UNPAIR          "child.unpair"

/* 子设备事件的种类（队列里传递，⛔ 不直接传 link-sdk 的指针） */
#define GW_SUB_KIND_ONLINE     0u
#define GW_SUB_KIND_OFFLINE    1u
#define GW_SUB_KIND_TELEMETRY  2u
#define GW_SUB_KIND_ERROR      3u

/* ---------------------------------------------------------------- 类型 */

/* 一条绑定（**故意不含档案正文**：档案通常 ≥400 B，只在注册时临时读一次，
 * 否则 4 条绑定要常驻 4 KB 内部 RAM） */
typedef struct {
    bool used;
    char node[GW_CHILD_NODE_MAX];
    char product_key[GW_CHILD_PK_MAX];
    char model[GW_CHILD_MODEL_MAX];
    char addr[GW_CHILD_ADDR_MAX];   /* 学到的 BLE 地址（配对时云端不知道，首次连上才记） */
    /*
     * 2026-10-26〔子设备发现〕App 从"发现候选"里**选中**的那台子设备的 MAC（= `child.pair` 的
     * `addr` 参数）。非空 ⇒ 本绑定只认这个地址（`bind_resolve` 里锁定），同型号多台不会连错；
     * 空 ⇒ 老行为（按档案 model 匹配 + 首次连上学习地址）。NVS 键 = "want"。
     */
    char want_addr[GW_CHILD_ADDR_MAX];
    /* 2026-10-28：子设备 cmd=0x03 通用 TLV 的最近一帧（网关不解析标签，原样并进 telemetry） */
    uint8_t extra_tlv[GW_EXTRA_TLV_CAP];
    uint8_t extra_tlv_len;
    int64_t extra_tlv_ms;
} gw_child_bind_t;

/* 队列里的一条子设备事件（回调内定长拷贝，⛔ 不含任何外部指针） */
typedef struct {
    uint8_t kind;
    int8_t  rssi;
    uint8_t cmd;
    bool    has_cmd;
    bool    tlv_trunc;
    bool    raw_trunc;
    int16_t err;
    char    model[GW_CHILD_MODEL_MAX];
    char    addr[GW_CHILD_ADDR_MAX];
    uint8_t tlv_len;
    uint8_t raw_len;
    uint8_t tlv[GW_CHILD_TLV_CAP];
    uint8_t raw[GW_CHILD_RAW_CAP];
} gw_sub_evt_t;

/* 队列里的一条命令（回调内解析后投递；正文可能是 linkProfile 的序列化结果） */
typedef struct {
    char   id[GW_CHILD_ID_MAX];
    char   service[16];
    char   node[GW_CHILD_NODE_MAX];
    char   product_key[GW_CHILD_PK_MAX];
    char   model[GW_CHILD_MODEL_MAX];
    /* 2026-10-26〔子设备发现〕`child.pair` 的可选 `addr`（App 选中的 MAC；空 = 未指定）。 */
    char   addr[GW_CHILD_ADDR_MAX];
    bool   has_profile;
    bool   profile_too_large;
    size_t profile_len;
    char   profile[GW_CHILD_PROFILE_MAX];
} gw_cmd_t;

/* ---------------------------------------------------------------- 状态 */

static gw_child_bind_t s_binds[GW_CHILD_MAX];
static QueueHandle_t   s_cmd_q;
static QueueHandle_t   s_sub_q;
static TaskHandle_t    s_task;

/* ---------------------------------------------------------------- 子设备发现（2026-10-26） */

#define GW_DISC_MAX         8       /* 附近最多记 8 台（同型号多台也放得下） */
#define GW_DISC_TTL_MS      120000  /* 超时未再看到即从发现表剔除 */
/* 2026-10-26〔发现扫描节流 · 真机教训〕：不能"一直扫"也不能"连上就永久停扫"。
 *   实测现象：子设备连上后 link-sdk 会停扫（共存降载），发现表 120 s 后清空 ⇒
 *   App 再打开"添加子设备"时云端 candidates 永远是空。
 *   方案：**每 GW_DISC_REPORT_MS 开一个 GW_DISC_SCAN_WINDOW_MS 的发现窗口**
 *   （8 s / 30 s ⇒ 平均扫描占空比 ≈0.5%），窗口结束立刻关扫描并汇总上报。 */
#define GW_DISC_REPORT_MS        30000
#define GW_DISC_SCAN_WINDOW_MS    8000

typedef struct {
    bool    used;
    char    addr[GW_CHILD_ADDR_MAX];
    char    model[GW_CHILD_MODEL_MAX];
    char    product_key[GW_CHILD_PK_MAX];
    int8_t  rssi;
    int64_t last_ms;
} gw_disc_t;

static gw_disc_t s_disc[GW_DISC_MAX];
static int64_t   s_disc_last_report_ms;
/* 发现窗口：非 0 = 窗口开着，到点关扫描并上报（见 GW_DISC_REPORT_MS 注释）。 */
static int64_t   s_disc_window_end_ms;

/* 档案里的 productKey 尚未全量随链路下发 ⇒ 先按内置基线映射（T31 档 ↔ OY-THE-01）。
 * ⚠️ 绑定表里已带 product_key 的机型优先用绑定表的（见 gw_disc_pk_of）。 */
static const struct {
    const char *model;
    const char *pk;
} s_disc_pk_baseline[] = {
    { "T31", "OY-THE-01" },
};

/* sub.telemetry 节流：只在工作任务里读写 ⇒ 不需要临界区 */
static int64_t  s_last_tele_ms;
static uint32_t s_tele_dropped;
static uint32_t s_unbound_dropped;   /* 未配对子设备的帧（丢弃并计数，见 handle_sub） */
static uint32_t s_sub_q_dropped;     /* 事件队列满而丢弃的帧（2026-10-23 观测性） */

/* 2026-10-26〔允许丢帧 + 断线清队列〕云状态门控：
 *   现场（mg3mode2 实测）：云断期间 sub.telemetry 继续入 SDK 事件队列 ⇒ txq/slots 塞满、
 *   堆碎片化到 largest 只剩 4 KB ⇒ TLS 重连需要 13–16 KB 连续块，**必然重连失败**（C 灭后回不来）。
 *   策略：云断 ⇒ ① 本任务暂停上报（直接丢弃并计数，用户已批准"允许丢帧/补报"）；
 *               ② 在**普通任务上下文**里清空 SDK 事件队列（不在 SDK 回调里调 SDK，避免重入）。 */
static volatile bool s_cloud_up = true;
static volatile bool s_purge_req;
static uint32_t s_cloud_dropped;

/* ---------------------------------------------------------------- 小工具 */

static void str_copy(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    (void)snprintf(dst, cap, "%s", src);
}

static int64_t now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000);
}

/* 大写到十六进制、**无分隔符**（PROTOCOL.md §3 示例：10020E5A080158） */
static void hex_upper(const uint8_t *buf, size_t n, char *out, size_t cap)
{
    static const char k_hex[] = "0123456789ABCDEF";
    size_t i;
    size_t o = 0;

    if (cap == 0) {
        return;
    }
    for (i = 0; i < n && (o + 2u) < cap; i++) {
        out[o++] = k_hex[(buf[i] >> 4) & 0x0Fu];
        out[o++] = k_hex[buf[i] & 0x0Fu];
    }
    out[o] = '\0';
}

/* 回调里拷原始字节：返回拷走的字节数，*trunc 标出是否被截断（⛔ 不静默截断） */
static size_t copy_bytes(uint8_t *dst, size_t cap, const uint8_t *src, size_t n, bool *trunc)
{
    size_t take = (n < cap) ? n : cap;

    *trunc = (n > cap);
    if (src != NULL && take > 0) {
        memcpy(dst, src, take);
    }
    return take;
}

static void bind_key(char *out, size_t cap, size_t idx, const char *what)
{
    (void)snprintf(out, cap, "b%u_%s", (unsigned)idx, what);
}

/* ---------------------------------------------------------------- NVS */

static esp_err_t nvs_store_count(void)
{
    nvs_handle_t h;
    uint8_t used = 0;
    esp_err_t err;

    for (size_t i = 0; i < GW_CHILD_MAX; i++) {
        if (s_binds[i].used) {
            used++;
        }
    }
    err = nvs_open(GW_CHILD_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, GW_CHILD_NVS_KEY_CNT, used);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_store_bind(size_t idx, const gw_child_bind_t *b)
{
    nvs_handle_t h;
    char k[12];
    esp_err_t err;

    err = nvs_open(GW_CHILD_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    bind_key(k, sizeof(k), idx, "node");
    err = nvs_set_str(h, k, b->node);
    bind_key(k, sizeof(k), idx, "pk");
    if (err == ESP_OK) {
        err = nvs_set_str(h, k, b->product_key);
    }
    bind_key(k, sizeof(k), idx, "model");
    if (err == ESP_OK) {
        err = nvs_set_str(h, k, b->model);
    }
    bind_key(k, sizeof(k), idx, "addr");
    if (err == ESP_OK) {
        err = nvs_set_str(h, k, b->addr);
    }
    /* 2026-10-26〔子设备发现〕选中的目标 MAC（"want"）—— 空串也要写，否则解绑/重配后旧值残留。 */
    bind_key(k, sizeof(k), idx, "want");
    if (err == ESP_OK) {
        err = nvs_set_str(h, k, b->want_addr);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_store_profile(size_t idx, const char *json)
{
    nvs_handle_t h;
    char k[12];
    esp_err_t err;

    err = nvs_open(GW_CHILD_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    bind_key(k, sizeof(k), idx, "prof");
    err = nvs_set_str(h, k, json);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* 读某槽位的档案正文；返回长度，0 = 没有/读失败 */
static size_t nvs_load_profile(size_t idx, char *buf, size_t cap)
{
    nvs_handle_t h;
    char k[12];
    size_t len = cap;
    esp_err_t err;

    if (buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    err = nvs_open(GW_CHILD_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return 0;
    }
    bind_key(k, sizeof(k), idx, "prof");
    err = nvs_get_str(h, k, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        buf[0] = '\0';
        return 0;
    }
    return strlen(buf);
}

/* 抹掉一个槽位（绑定 + 档案） */
static esp_err_t nvs_erase_bind(size_t idx)
{
    nvs_handle_t h;
    char k[12];
    esp_err_t err;

    err = nvs_open(GW_CHILD_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    bind_key(k, sizeof(k), idx, "node");
    (void)nvs_erase_key(h, k);
    bind_key(k, sizeof(k), idx, "pk");
    (void)nvs_erase_key(h, k);
    bind_key(k, sizeof(k), idx, "model");
    (void)nvs_erase_key(h, k);
    bind_key(k, sizeof(k), idx, "addr");
    (void)nvs_erase_key(h, k);
    bind_key(k, sizeof(k), idx, "prof");
    (void)nvs_erase_key(h, k);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static size_t nvs_load_binds(void)
{
    nvs_handle_t h;
    uint8_t cnt = 0;
    size_t loaded = 0;
    esp_err_t err;

    for (size_t i = 0; i < GW_CHILD_MAX; i++) {
        memset(&s_binds[i], 0, sizeof(s_binds[i]));
    }

    err = nvs_open(GW_CHILD_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return 0;                       /* 从没配对过 —— 正常首启，不是错误 */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取绑定表失败（nvs_open %s）⇒ 本次以空表运行", esp_err_to_name(err));
        return 0;
    }
    if (nvs_get_u8(h, GW_CHILD_NVS_KEY_CNT, &cnt) != ESP_OK) {
        cnt = 0;
    }
    if (cnt > GW_CHILD_MAX) {
        ESP_LOGW(TAG, "NVS 里的绑定条数 %u 超过上限 %d ⇒ 只读前 %d 条",
                 (unsigned)cnt, (int)GW_CHILD_MAX, (int)GW_CHILD_MAX);
        cnt = GW_CHILD_MAX;
    }
    for (uint8_t i = 0; i < cnt; i++) {
        char k[12];
        size_t len;

        bind_key(k, sizeof(k), i, "node");
        len = sizeof(s_binds[i].node);
        if (nvs_get_str(h, k, s_binds[i].node, &len) != ESP_OK || s_binds[i].node[0] == '\0') {
            continue;                   /* 槽位空着 = 未使用 */
        }
        bind_key(k, sizeof(k), i, "pk");
        len = sizeof(s_binds[i].product_key);
        if (nvs_get_str(h, k, s_binds[i].product_key, &len) != ESP_OK) {
            s_binds[i].product_key[0] = '\0';
        }
        bind_key(k, sizeof(k), i, "model");
        len = sizeof(s_binds[i].model);
        if (nvs_get_str(h, k, s_binds[i].model, &len) != ESP_OK) {
            s_binds[i].model[0] = '\0';
        }
        bind_key(k, sizeof(k), i, "addr");
        len = sizeof(s_binds[i].addr);
        if (nvs_get_str(h, k, s_binds[i].addr, &len) != ESP_OK) {
            s_binds[i].addr[0] = '\0';
        }
        /* 2026-10-26〔子设备发现〕选中的目标 MAC；老 NVS 没有该键 ⇒ 空串 = 未指定。 */
        bind_key(k, sizeof(k), i, "want");
        len = sizeof(s_binds[i].want_addr);
        if (nvs_get_str(h, k, s_binds[i].want_addr, &len) != ESP_OK) {
            s_binds[i].want_addr[0] = '\0';
        }
        s_binds[i].used = true;
        loaded++;
    }
    nvs_close(h);
    return loaded;
}

/* ---------------------------------------------------------------- 绑定表查询 */

static int bind_find_by_node(const char *node)
{
    if (node == NULL || node[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < GW_CHILD_MAX; i++) {
        if (s_binds[i].used && strcmp(s_binds[i].node, node) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int bind_find_free(void)
{
    for (size_t i = 0; i < GW_CHILD_MAX; i++) {
        if (!s_binds[i].used) {
            return (int)i;
        }
    }
    return -1;
}

static bool model_in_use(const char *model, int except_idx)
{
    for (size_t i = 0; i < GW_CHILD_MAX; i++) {
        if ((int)i == except_idx || !s_binds[i].used) {
            continue;
        }
        if (model != NULL && strcmp(s_binds[i].model, model) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * 把「一条链路上的子设备」解析成绑定槽位。
 * 匹配优先级：① 学到的 BLE 地址逐字相等 → ② 档案 model 相等。
 * ② 命中且该槽位还没有地址时**顺手学会**地址（多台同型号时后续就按地址区分）。
 * 只在工作任务里调用（会写 NVS）。
 */
/**
 * MAC 地址比较（**大小写不敏感**）—— 2026-10-26〔子设备发现〕。
 *
 * <p>为什么必须不敏感：云端下发的 `addr` 是**小写**（`device-service` 归一化成小写），
 * 而 link-sdk 事件里的地址是**大写**（`%02X`）。同一台设备两种写法 ⇒ 用 `strcmp` 会永远不相等，
 * 锁定就失效（甚至把"只认锁定地址"的槽位判成不匹配而丢帧）。
 */
static bool addr_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 'a' + 'A') : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 'a' + 'A') : *b;
        if (ca != cb) {
            return false;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static int bind_resolve(const char *addr, const char *model)
{
    int by_model = -1;

    /* ② 2026-10-26〔子设备发现〕`want_addr` 锁定优先于一切：
     *    这是 App 从"发现候选"里**选中**的那一台。锁定槽位只接受该地址，
     *    ⛔ 不再走 model 回落（否则同型号多台时会把别的设备当成它）。 */
    if (addr != NULL && addr[0] != '\0') {
        for (size_t i = 0; i < GW_CHILD_MAX; i++) {
            if (s_binds[i].used && s_binds[i].want_addr[0] != '\0' &&
                addr_ieq(s_binds[i].want_addr, addr)) {
                return (int)i;
            }
        }
    }
    if (addr != NULL && addr[0] != '\0') {
        for (size_t i = 0; i < GW_CHILD_MAX; i++) {
            if (s_binds[i].used && s_binds[i].addr[0] != '\0' &&
                addr_ieq(s_binds[i].addr, addr)) {
                return (int)i;
            }
        }
    }
    if (model != NULL && model[0] != '\0') {
        for (size_t i = 0; i < GW_CHILD_MAX; i++) {
            /* 锁定到别的地址的槽位不参与 model 匹配（否则会顶替成那台设备）。 */
            if (s_binds[i].used && strcmp(s_binds[i].model, model) == 0 &&
                s_binds[i].want_addr[0] == '\0') {
                by_model = (int)i;
                break;
            }
        }
    }
    if (by_model >= 0 && addr != NULL && addr[0] != '\0' && s_binds[by_model].addr[0] == '\0') {
        str_copy(s_binds[by_model].addr, sizeof(s_binds[by_model].addr), addr);
        if (nvs_store_bind((size_t)by_model, &s_binds[by_model]) != ESP_OK) {
            ESP_LOGW(TAG, "子设备 %s 的 BLE 地址未能落 NVS（RAM 内已生效，重启后靠 model 再匹配）",
                     s_binds[by_model].node);
        } else {
            ESP_LOGI(TAG, "已记住子设备 %s 的 BLE 地址 %s",
                     s_binds[by_model].node, s_binds[by_model].addr);
        }
    }
    return by_model;
}

bool gw_child_any_bound(void)
{
    for (size_t i = 0; i < GW_CHILD_MAX; i++) {
        if (s_binds[i].used) {
            return true;
        }
    }
    return false;
}

/* 2026-10-25〔共存降载 · 方案 1〕绑定数量 = BLE 的"目标子设备数"。
 *   link-sdk 据此决定"还要不要扫描"：已在线数 ≥ 目标数 ⇒ 停扫，把射频让给 Wi-Fi。
 *   ⚠️ 任何改变绑定表的动作（启动恢复 / child.pair / child.unpair）之后都必须重新同步一次。 */
uint8_t gw_child_bound_count(void)
{
    uint8_t n = 0;
    for (size_t i = 0; i < GW_CHILD_MAX; i++) {
        if (s_binds[i].used) {
            n++;
        }
    }
    return n;
}

/* 把绑定数量同步给 link-sdk（放在所有"绑定表变更"路径的收口处调用）。 */
static void sync_target_links(void)
{
    oneye_link_set_target_links(gw_child_bound_count());
}

/* 2026-10-26〔允许丢帧 + 断线清队列〕由 main.c 的云事件回调调用（只置位，⛔ 不调 SDK）：
 *   云断 ⇒ 暂停 sub.telemetry 入队 + 申请清空 SDK 事件队列；
 *   云恢复 ⇒ 复位节流位（下一帧立刻补报）。 */
void gw_child_on_cloud_state(bool up)
{
    s_cloud_up = up;
    if (up) {
        s_last_tele_ms = 0;   /* 恢复后立刻补报一帧，不用等满节流窗口 */
        /* 2026-10-26〔子设备发现〕云恢复 ⇒ 尽快开一个发现窗口并补报一次，
         * 否则链路闪断期间的窗口被丢弃，candidates 会在 App 里"过期"。 */
        s_disc_last_report_ms = 0;
    } else {
        s_purge_req = true;   /* 实际清理在 gw_child_task（普通上下文）里做 */
    }
    /* 2026-10-26〔子设备发现〕扫描由**任务里的发现窗口**节制（8 s/30 s）；云断立即关窗+关扫描，
     * 云恢复不立刻开扫（等下一个窗口，避免刚重连就抢射频）。 */
    if (!up) {
        s_disc_window_end_ms = 0;
        oneye_link_set_discovery(false);
    }
    ESP_LOGI(TAG, "云状态 → %s（%s）", up ? "UP" : "DOWN",
             up ? "恢复 sub.telemetry（节流位已复位）；发现窗口由任务节流"
                : "暂停 sub.telemetry 入队，并申请清空 SDK 事件队列 ＋ 发现扫描关");
}

/* ---------------------------------------------------------------- 上报 */

static bool emit_event(const char *type, const char *data_json)
{
    oneye_dev_event_item_t it;
    int64_t t0_ms;
    int64_t used_ms;
    oneye_dev_sdk_err_t rc;

    memset(&it, 0, sizeof(it));            /* 事件条目无 struct_size/api_version */
    it.type = type;
    it.severity = ONEYE_DEV_SEVERITY_INFO;
    it.data_json = data_json;

    /* 🔴 2026-10-23：SDK `oneye_dev_event_report()` 的 ERR_INVALID 只来自**三个入参分支**
     *   （oneye_dev_event.c:1051-1063，逐字复刻到这里先自检一遍，失败时能直接指出是哪一条）：
     *     ① type == NULL || type[0]=='\0'；
     *     ② severity 不在 [INFO(0), CRITICAL(2)]；
     *     ③ data_json 非空且不是"整串消费的 JSON 对象"。
     *   注：SDK 里第 ③ 条实际以 `oneye_json_validate_object(data, 0u)` 调用，len=0 时该函数
     *   直接返回 true（见 oneye_json.c:456），所以真正会踩的只有 ①② 或**我们传了悬空/非对象串**。 */
    {
        cJSON *chk = NULL;
        bool json_ok = true;

        if (data_json != NULL && data_json[0] != '\0') {
            chk = cJSON_ParseWithLength(data_json, strlen(data_json));
            json_ok = (chk != NULL) && cJSON_IsObject(chk);
            if (chk != NULL) {
                cJSON_Delete(chk);
            }
        }
        if (type == NULL || type[0] == '\0' ||
            (int)it.severity < (int)ONEYE_DEV_SEVERITY_INFO ||
            (int)it.severity > (int)ONEYE_DEV_SEVERITY_CRITICAL || !json_ok) {
            /* 🔴 2026-10-23：把非法原文**转义**打印（≤120 B；不可见字符打成 \xNN，
             *   引号/反斜杠加反斜杠）—— 真机就是这样定位"哪里多/少了一个引号"。 */
            char esc[160];
            size_t o = 0;

            if (data_json != NULL) {
                for (size_t i = 0; data_json[i] != '\0' && o + 5u < sizeof(esc); i++) {
                    unsigned char c = (unsigned char)data_json[i];

                    if (c == '"' || c == '\\') {
                        esc[o++] = '\\';
                        esc[o++] = (char)c;
                    } else if (c >= 0x20u && c < 0x7Fu) {
                        esc[o++] = (char)c;
                    } else {
                        int w = snprintf(esc + o, sizeof(esc) - o, "\\x%02X", (unsigned)c);

                        if (w > 0) {
                            o += ((size_t)w < (sizeof(esc) - o)) ? (size_t)w : (sizeof(esc) - o - 1u);
                        }
                    }
                    if (o >= sizeof(esc) - 1u) {
                        break;
                    }
                }
            }
            esc[o] = '\0';
            ESP_LOGW(TAG, "[自检] event 入参非法 ⇒ SDK 必返 ERR_INVALID：type=%s sev=%d "
                          "data_len=%u json_obj=%d",
                     (type != NULL) ? type : "(null)", (int)it.severity,
                     (unsigned)((data_json != NULL) ? strlen(data_json) : 0u), (int)json_ok);
            if (!json_ok) {
                ESP_LOGW(TAG, "[自检] %s data 非法原文=\"%s\"（≤120 B）", type, esc);
            }
        }
    }

    /* 🔴 2026-10-23：每次出队上报都打 **rc + 耗时**（真机曾出现"任务不出队"但无读数可判）。
     *   判据：正常应 ≤ 数十 ms；>300 ms 说明上报路径（base 锁 / MQTT 队列）变慢。 */
    t0_ms = esp_timer_get_time() / 1000;
    rc = oneye_dev_event_report(&it, 1);
    used_ms = (esp_timer_get_time() / 1000) - t0_ms;

    if (rc == ONEYE_DEV_SDK_OK) {
        ESP_LOGI(TAG, "event/up %s 已提交（rc=0，耗时 %lld ms，data %u B）%s",
                 type, (long long)used_ms, (unsigned)strlen(data_json), data_json);
        return true;
    }
    /* 失败即真的没发出去（未在线且缓存关/队列满/参数非法）—— ⛔ 不假装已上报、⛔ 不重试
     * （条目已在任务里出队；重试会形成"失败→回队→再失败"死循环）。 */
    if (used_ms > 300) {
        ESP_LOGW(TAG, "event/up %s 失败：%s（rc=%d，耗时 %lld ms ⇒ 上报路径慢，检查 base/MQTT）",
                 type, oneye_dev_sdk_strerror(rc), (int)rc, (long long)used_ms);
    } else {
        ESP_LOGW(TAG, "event/up %s 失败：%s（rc=%d，耗时 %lld ms）⇒ 本帧丢弃（不重试）",
                 type, oneye_dev_sdk_strerror(rc), (int)rc, (long long)used_ms);
    }
    return false;
}

/* ---------------------------------------------------------------- 子设备发现（2026-10-26） */

/** model → productKey：绑定表里已有该机型的 product_key 就用它，否则回落内置基线映射。 */
static const char *gw_disc_pk_of(const char *model)
{
    if (model == NULL || model[0] == '\0') {
        return "";
    }
    for (size_t i = 0; i < GW_CHILD_MAX; i++) {
        if (s_binds[i].used && strcmp(s_binds[i].model, model) == 0 &&
            s_binds[i].product_key[0] != '\0') {
            return s_binds[i].product_key;
        }
    }
    for (size_t i = 0; i < sizeof(s_disc_pk_baseline) / sizeof(s_disc_pk_baseline[0]); i++) {
        if (strcmp(s_disc_pk_baseline[i].model, model) == 0) {
            return s_disc_pk_baseline[i].pk;
        }
    }
    return "";
}

/**
 * link-sdk 扫描发现回调（BLE host 任务上下文）—— 只写小表，⛔ 不做阻塞/上报。
 * ⚠️ 同一 addr 只保留一条（刷新 RSSI 与 last_ms）；表满时替换最旧的一条。
 */
static void gw_disc_on_adv(const char *addr, const char *model, int8_t rssi, void *ctx)
{
    (void)ctx;
    int64_t now = now_ms();
    int same = -1;
    int free_i = -1;
    int oldest = -1;

    for (int i = 0; i < GW_DISC_MAX; i++) {
        if (s_disc[i].used && strcmp(s_disc[i].addr, addr) == 0) {
            same = i;
            break;
        }
        if (!s_disc[i].used && free_i < 0) {
            free_i = i;
        }
        if (s_disc[i].used && (oldest < 0 || s_disc[i].last_ms < s_disc[oldest].last_ms)) {
            oldest = i;
        }
    }
    int idx = (same >= 0) ? same : (free_i >= 0 ? free_i : oldest);
    if (idx < 0) {
        return;
    }
    memset(&s_disc[idx], 0, sizeof(s_disc[idx]));
    s_disc[idx].used = true;
    str_copy(s_disc[idx].addr, sizeof(s_disc[idx].addr), addr);
    str_copy(s_disc[idx].model, sizeof(s_disc[idx].model), model);
    str_copy(s_disc[idx].product_key, sizeof(s_disc[idx].product_key), gw_disc_pk_of(model));
    s_disc[idx].rssi = rssi;
    s_disc[idx].last_ms = now;
}

/**
 * 汇总上报 sub.discovered —— 由 gw_child_task 每 GW_DISC_REPORT_MS 调一次。
 *
 * <p>payload：{@code {"items":[{"model","productKey","mac","macLen","rssi"}]}}；
 * 只含发现元数据，⛔ 不含温度/电量（未配对子设备数据不上报，用户 2026-10-23 口径）。
 * 空表/云断 ⇒ 直接返回（⛔ 不发空帧浪费配额）。
 */
static void gw_disc_report(void)
{
    char body[512];
    size_t used = 0;
    int n = 0;
    int64_t now = now_ms();

    if (!s_cloud_up) {
        return;
    }
    used += (size_t)snprintf(body + used, sizeof(body) - used, "{\"items\":[");
    for (int i = 0; i < GW_DISC_MAX; i++) {
        if (!s_disc[i].used) {
            continue;
        }
        if (now - s_disc[i].last_ms > GW_DISC_TTL_MS) {
            s_disc[i].used = false;   /* 超时剔除：网关已经看不到它了 */
            continue;
        }
        if (s_disc[i].product_key[0] == '\0') {
            continue;                 /* 机型没有 productKey 映射 ⇒ 上报了云端也匹配不了 */
        }
        if (used + 96u >= sizeof(body)) {
            break;
        }
        used += (size_t)snprintf(body + used, sizeof(body) - used,
                                 "%s{\"model\":\"%s\",\"productKey\":\"%s\",\"mac\":\"%s\","
                                 "\"macLen\":6,\"rssi\":%d}",
                                 (n > 0) ? "," : "",
                                 s_disc[i].model, s_disc[i].product_key, s_disc[i].addr,
                                 (int)s_disc[i].rssi);
        n++;
    }
    (void)snprintf(body + used, sizeof(body) - used, "]}");
    if (n <= 0) {
        return;
    }
    s_disc_last_report_ms = now;
    if (emit_event("sub.discovered", body)) {
        ESP_LOGI(TAG, "子设备发现已上报：%d 台（%s）", n, body);
    }
}

/* ---------------------------------------------------------------- 子设备事件处理（工作任务） */

/* ========================================================================
 * 🔴 2026-10-25：子设备 GNSS 定位（cmd 0x02）→ 并进温度帧的 TLV 上报
 *
 * 线协议（子设备 t31_sim.c 文件头）：`A6 02 <14 B LE payload> checksum 6A`，
 *   payload = lat i32le(1e-7°) / lon i32le(1e-7°) / alt i16le(m) / speed u16le(0.01 m/s)
 *             / sats u8 / fix u8(0 未定位 1 2D 2 3D)。
 *
 * ★ 为什么"先缓存、再并进温度帧"而不是直接把它当成一条 telemetry 上报：
 *   · 云端 `decodeT31` 的硬判据是"这一帧必须有温度（0x10）"——单独一帧 GPS 会被整帧丢弃；
 *   · 温度帧本来就按 CONFIG_ONEYE_T31_GW_SUB_TELE_MIN_MS 节流上报，把最近 30 s 内的定位
 *     并进去，既不额外增加 event/up 频率，又保证"有温度就有定位（若有）"。
 *   · 未定位（fix=0）时只并 sats/fix 两个标签：云端影子能看出"模组在、没信号"，
 *     ⛔ 不会用 0/0 的假经纬度污染地图。
 * ★ 云端标签口径（0x50–0x55；0x56 走 cmd=0x03 通用 TLV 透传）见 oneye-link-sdk/docs/TLV-REGISTRY.md §5；单位/大端与
 *   TLV 编码一致（i32/i16/u16 均大端，与温度 0x10 同族）。
 * ======================================================================== */

typedef struct {
    bool     valid;      /* 收到过合法定位帧 */
    int32_t  lat_e7;
    int32_t  lon_e7;
    int16_t  alt_m;
    uint16_t speed_cms;
    uint8_t  sats;
    uint8_t  fix;        /* 0 未定位 / 1 2D / 2 3D */
    int64_t  ms;         /* 最近一次收到定位帧的本地毫秒（now_ms 口径） */
} gw_gps_t;

static gw_gps_t s_gps;
static uint32_t s_gps_frames;      /* 累计收到的定位帧（诊断） */
static uint32_t s_gps_bad;         /* 累计结构/校验失败的定位帧（诊断） */
static uint32_t s_gps_merged;      /* 累计把 GPS 标签并进 telemetry 的帧数（诊断） */

static int32_t gps_rd_i32le(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static int16_t gps_rd_i16le(const uint8_t *p)
{
    return (int16_t)(uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint16_t gps_rd_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/** 解一帧 `A6 02 …`；失败返回 false 且 ⛔ 不修改 out。 */
static bool gw_gps_decode_frame(const uint8_t *frame, size_t len, gw_gps_t *out)
{
    gw_gps_t g = { 0 };
    uint8_t sum = 0;

    if (frame == NULL || out == NULL || len != GW_GPS_FRAME_LEN) {
        return false;
    }
    if (frame[0] != 0xA6u || frame[1] != 0x02u || frame[GW_GPS_FRAME_LEN - 1u] != 0x6Au) {
        return false;
    }
    for (size_t i = 1; i < GW_GPS_FRAME_LEN - 2u; i++) {   /* cmd + payload（不含 checksum/EOP） */
        sum = (uint8_t)(sum + frame[i]);
    }
    if (sum != frame[GW_GPS_FRAME_LEN - 2u]) {
        return false;
    }
    g.lat_e7 = gps_rd_i32le(frame + 2);
    g.lon_e7 = gps_rd_i32le(frame + 6);
    g.alt_m = gps_rd_i16le(frame + 10);
    g.speed_cms = gps_rd_u16le(frame + 12);
    g.sats = frame[14];
    g.fix = (frame[15] <= 2u) ? frame[15] : 0u;
    /* 合理性闸门：fix 有效才校经纬度范围（未定位时字段是 0/0，不是错误） */
    if (g.fix != 0u &&
        (g.lat_e7 < -900000000 || g.lat_e7 > 900000000 ||
         g.lon_e7 < -1800000000 || g.lon_e7 > 1800000000)) {
        return false;
    }
    if (g.fix == 0u) {
        g.lat_e7 = 0;
        g.lon_e7 = 0;
        g.alt_m = 0;
        g.speed_cms = 0;
    }
    g.valid = true;
    *out = g;
    return true;
}

/* TLV 写入小工具（大端）；缓冲不足返回原 used（调用方先算 needs） */
static size_t tlv_put_u8(uint8_t *b, size_t used, uint8_t tag, uint8_t v)
{
    b[used++] = tag;
    b[used++] = 1u;
    b[used++] = v;
    return used;
}

static size_t tlv_put_u16(uint8_t *b, size_t used, uint8_t tag, uint16_t v)
{
    b[used++] = tag;
    b[used++] = 2u;
    b[used++] = (uint8_t)(v >> 8);
    b[used++] = (uint8_t)(v & 0xFFu);
    return used;
}

static size_t tlv_put_i16(uint8_t *b, size_t used, uint8_t tag, int16_t v)
{
    return tlv_put_u16(b, used, tag, (uint16_t)v);
}

static size_t tlv_put_i32(uint8_t *b, size_t used, uint8_t tag, int32_t v)
{
    uint32_t u = (uint32_t)v;

    b[used++] = tag;
    b[used++] = 4u;
    b[used++] = (uint8_t)(u >> 24);
    b[used++] = (uint8_t)(u >> 16);
    b[used++] = (uint8_t)(u >> 8);
    b[used++] = (uint8_t)(u & 0xFFu);
    return used;
}

/** 把 GPS 标签追加到 TLV（fix=0 时只写 0x54/0x55）。返回新的 used。 */
static size_t gw_tlv_append_gps(uint8_t *buf, size_t used, const gw_gps_t *g)
{
    if (g->fix != 0u) {
        used = tlv_put_i32(buf, used, GW_TLV_GNSS_LAT, g->lat_e7);
        used = tlv_put_i32(buf, used, GW_TLV_GNSS_LON, g->lon_e7);
        used = tlv_put_i16(buf, used, GW_TLV_GNSS_ALT, g->alt_m);
        used = tlv_put_u16(buf, used, GW_TLV_GNSS_SPEED, g->speed_cms);
    }
    used = tlv_put_u8(buf, used, GW_TLV_GNSS_SATS, g->sats);
    used = tlv_put_u8(buf, used, GW_TLV_GNSS_FIX, g->fix);
    return used;
}

/** 处理一帧定位帧（cmd 0x02）：优先用原始 notify 字节（e->raw），退回 TLV 0x80 里的原始帧。 */
static void gw_gps_on_frame(const gw_sub_evt_t *e)
{
    gw_gps_t g = { 0 };
    bool ok = false;
    const uint8_t *frame = NULL;
    size_t len = 0;

    if (e->raw_len == GW_GPS_FRAME_LEN) {
        frame = e->raw;
        len = e->raw_len;
    } else if (e->tlv_len >= (2u + GW_GPS_FRAME_LEN) && e->tlv[0] == ONEYE_TLV_RAW_FRAME &&
               e->tlv[1] == GW_GPS_FRAME_LEN) {
        frame = e->tlv + 2;
        len = GW_GPS_FRAME_LEN;
    }
    if (frame != NULL) {
        ok = gw_gps_decode_frame(frame, len, &g);
    }
    if (!ok) {
        s_gps_bad++;
        if ((s_gps_bad % 20u) == 1u) {
            ESP_LOGW(TAG, "[gnss] 定位帧解不出（累计 %u 帧坏）raw_len=%u tlv_len=%u cmd=0x%02X",
                     (unsigned)s_gps_bad, (unsigned)e->raw_len, (unsigned)e->tlv_len,
                     (unsigned)e->cmd);
        }
        return;
    }
    g.ms = now_ms();
    s_gps = g;
    s_gps_frames++;
    if ((s_gps_frames % 5u) == 1u) {
        if (g.fix != 0u) {
            ESP_LOGI(TAG, "[gnss] 定位帧 #%u：fix=%u sats=%u lat=%.7f lon=%.7f alt=%d m "
                          "speed=%.2f m/s ⇒ 已缓存，随下一帧温度并入 TLV",
                     (unsigned)s_gps_frames, (unsigned)g.fix, (unsigned)g.sats,
                     (double)g.lat_e7 / 1e7, (double)g.lon_e7 / 1e7, (int)g.alt_m,
                     (double)g.speed_cms / 100.0);
        } else {
            ESP_LOGI(TAG, "[gnss] 定位帧 #%u：**未定位** fix=0 sats=%u（室内属正常；"
                          "已缓存 sats/fix，经纬度等高等着）",
                     (unsigned)s_gps_frames, (unsigned)g.sats);
        }
    }
}

/** 简化 JSON 字符串转义（真机事故：手写拼串遇到引号/反斜杠就产出非法 JSON）。
 *  本工程 node/model 来自 NVS/档案，理论上很短且受控，但仍按 JSON 规则转义。 */
static void json_escape_copy(char *dst, size_t cap, const char *src)
{
    size_t o = 0;

    if (dst == NULL || cap == 0) {
        return;
    }
    for (const char *p = src; p != NULL && *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;

        if (o + 8u >= cap) {
            break;
        }
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c >= 0x20u && c < 0x7Fu) {
            dst[o++] = (char)c;
        } else {
            int w = snprintf(dst + o, cap - o, "\\u%04X", (unsigned)c);
            if (w <= 0) {
                break;
            }
            o += (size_t)w;
        }
    }
    dst[o] = '\0';
}

/** 组装 sub.telemetry 的 data（手写，⛔ 不用 cJSON：C6 无 PSRAM，Wi-Fi 重连时堆只剩 ~10 KB，
 *  真机就在这段出现过 `cJSON OOM ⇒ 本帧丢弃`）。返回 false = 超缓冲。 */
static bool build_telemetry_json(char *out, size_t cap, const char *node, const char *model,
                                 int rssi, uint8_t cmd, const char *tlv_hex, bool cut)
{
    char esc_node[(GW_CHILD_NODE_MAX * 6u) + 1u];
    char esc_model[(GW_CHILD_MODEL_MAX * 6u) + 1u];
    int n;

    json_escape_copy(esc_node, sizeof(esc_node), node);
    json_escape_copy(esc_model, sizeof(esc_model), model);
    n = snprintf(out, cap,
                 "{\"node\":\"%s\",\"model\":\"%s\",\"rssi\":%d,\"cmd\":\"0x%02X\",\"tlv\":\"%s\"%s",
                 esc_node, esc_model, rssi, (unsigned)cmd, tlv_hex,
                 cut ? ",\"trunc\":true" : "");
    if (n < 0 || (size_t)n + 2u > cap) {
        return false;
    }
    out[n++] = '}';
    out[n] = '\0';
    return true;
}

/* 通用 TLV 结构校验（**不认识任何标签**，只看 tag/len 边界是否自洽）。
 * 这样网关不依赖具体物模型；新标签只要子设备按 TLV 编码就能原样转发。 */
static bool gw_tlv_walk_ok(const uint8_t *p, size_t n)
{
    size_t off = 0;

    while (off + 2u <= n) {
        uint8_t len = p[off + 1u];
        if (off + 2u + len > n) {
            return false;
        }
        off += 2u + len;
    }
    return off == n;
}

/* 子设备 cmd=0x03：缓存最近一帧通用 TLV（按 bind 分开，⛔ 不串台）。 */
static void gw_extra_tlv_on_frame(const gw_sub_evt_t *e, int idx)
{
    size_t n;
    const uint8_t *p;

    if (idx < 0 || e->raw_len < 6u || e->raw[0] != 0xA6u || e->raw[1] != GW_CMD_TLV ||
        e->raw[e->raw_len - 1u] != 0x6Au) {
        return;
    }
    p = e->raw + 2;
    n = (size_t)e->raw_len - 4u;   /* 去掉 A6 / cmd / checksum / 6A */
    if (n == 0u || n > sizeof(s_binds[idx].extra_tlv) || !gw_tlv_walk_ok(p, n)) {
        ESP_LOGW(TAG, "子设备通用 TLV 非法/超长（%u B）⇒ 丢弃本帧", (unsigned)n);
        return;
    }
    memcpy(s_binds[idx].extra_tlv, p, n);
    s_binds[idx].extra_tlv_len = (uint8_t)n;
    s_binds[idx].extra_tlv_ms = now_ms();
    ESP_LOGD(TAG, "子设备通用 TLV 已缓存：%u B（网关不解析标签，随下一帧温度原样转发）",
             (unsigned)n);
}

static void handle_sub(const gw_sub_evt_t *e)
{
    /* 🔴 2026-10-23（真机 `event/up sub.telemetry 失败：invalid argument`）：载荷**瘦身**——
     *   · data 只发最小可解码集：`{"node","model","rssi","cmd","tlv"}`；
     *   · ⛔ 不再带 `raw`（原始 notify 帧）：TLV 里的 RAW_FRAME(0x80) 已能承担"云端换解析器"，
     *     双份 hex 会把单条 data 撑长（真机事件库校验直接 ERR_INVALID）；
     *   · `tlv` 最多 TELE_TLV_MAX(64) 字节 → 128 个十六进制字符，超出截断并带 `"trunc":true`。
     *   🔴 2026-10-25：data[256] → **[320]**（旧值逐字留痕，⛔ 不抹：`char data[256];`）。
     *      64 B TLV = 128 hex 字符，加上 node/model/rssi/cmd 后约 200–230 B，256 只剩一点余量；
     *      而超长是**整帧丢弃**（不是截断）⇒ 给足余量。 */
    char data[320];
    char tlv_hex[(TELE_TLV_MAX * 2u) + 1u];
    int idx;

    idx = bind_resolve(e->addr, e->model);
    if (idx < 0) {
        /* 未配对的子设备**不上报**：报出去就等于在云端凭空造一台没有 node_id 的设备。
         * 这是"如实"的方向（用户裁定：子设备必须有独立 node_id + 独立卡片）。 */
        s_unbound_dropped++;
        if ((s_unbound_dropped % 10u) == 1u) {
            /* 2026-10-23：附近可能有别的 BLE 设备（同名型号/别的 T31）⇒ 这是**正常**现象，
             *   降为 DEBUG，⛔ 不刷 WARN（保留计数与首条/每 10 条一条的可诊断性）。 */
            ESP_LOGD(TAG, "未配对子设备 %s model=%s ⇒ 丢弃其事件（累计 %u 帧；"
                          "先由云端下发 child.pair 才会代传）",
                     (e->addr[0] != '\0') ? e->addr : "(未知)",
                     (e->model[0] != '\0') ? e->model : "(未命中档案)",
                     (unsigned)s_unbound_dropped);
        }
        return;
    }

    switch (e->kind) {
    case GW_SUB_KIND_ONLINE:
        (void)snprintf(data, sizeof(data), "{\"node\":\"%s\",\"model\":\"%s\",\"rssi\":%d}",
                       s_binds[idx].node, s_binds[idx].model, (int)e->rssi);
        (void)emit_event(GW_EVT_SUB_ONLINE, data);
        break;

    case GW_SUB_KIND_OFFLINE:
        (void)snprintf(data, sizeof(data), "{\"node\":\"%s\",\"model\":\"%s\"}",
                       s_binds[idx].node, s_binds[idx].model);
        (void)emit_event(GW_EVT_SUB_OFFLINE, data);
        break;

    case GW_SUB_KIND_ERROR:
        (void)snprintf(data, sizeof(data), "{\"node\":\"%s\",\"model\":\"%s\",\"err\":%d}",
                       s_binds[idx].node, s_binds[idx].model, (int)e->err);
        (void)emit_event(GW_EVT_SUB_ERROR, data);
        break;

    case GW_SUB_KIND_TELEMETRY: {
        const int64_t now = now_ms();

        /* 2026-10-28：cmd=0x03 = 子设备通用 TLV 透传（gnssState 等新属性）。
         *   ⛔ 不当成一条 telemetry 上报（云端要求本帧必须有温度）；只缓存，
         *   随下一帧温度原样并入 TLV。网关**不解析标签** ⇒ 新物模型不用改网关。 */
        if (e->has_cmd && e->cmd == GW_CMD_TLV) {
            gw_extra_tlv_on_frame(e, idx);
            return;
        }

        /* 🔴 2026-10-25：cmd=0x02 = 子设备的 GNSS 定位帧（本仓对 T31 模拟器的扩展命令）。
         *   ⛔ 不当成一条 telemetry 上报 —— 云端 decodeT31 要求"这一帧必须有温度"，
         *   单独一帧 GPS 会被整帧丢弃；⇒ 只解出来缓存，随下一帧温度并入 TLV。
         *   定位帧**不参与**温度节流（否则 2 s 的温度窗口会把它一起挡掉）。 */
        if (e->has_cmd && e->cmd == 0x02u) {
            gw_gps_on_frame(e);
            return;
        }

        /* 2026-10-26〔允许丢帧 + 断线清队列〕云断期间**不再入队**：
         *   丢弃并计数（云侧影子 TTL + 重连补报兜底），保证 SDK 事件队列不被填满。 */
        if (!s_cloud_up) {
            s_cloud_dropped++;
            if ((s_cloud_dropped % 20u) == 1u) {
                ESP_LOGD(TAG, "云断 ⇒ 丢弃 sub.telemetry（累计 %u 帧；恢复后自动补报）",
                         (unsigned)s_cloud_dropped);
            }
            return;
        }

        /* 节流：同频上报没有意义（子设备 2 s 一帧 × 多子设备会把 event/up 打爆）。
         * 失败**不推进**节流位 ⇒ 下一帧立刻重试，不用等满一个窗口。 */
        if (s_last_tele_ms != 0 &&
            (now - s_last_tele_ms) < (int64_t)CONFIG_ONEYE_T31_GW_SUB_TELE_MIN_MS) {
            s_tele_dropped++;
            if ((s_tele_dropped % 20u) == 1u) {
                ESP_LOGD(TAG, "sub.telemetry 节流丢弃（累计 %u 帧，窗口 %d ms）",
                         (unsigned)s_tele_dropped, (int)CONFIG_ONEYE_T31_GW_SUB_TELE_MIN_MS);
            }
            return;
        }

        /* 2026-10-25：TLV = 档案解出的温度/电池 TLV +（有则）GPS 标签 0x50–0x55
         * +（有则）子设备 cmd=0x03 的通用 TLV（原样转发，网关不解析标签）。
         * 🔴 2026-10-23 真机收尾修复保留：手写拼串遇到引号/反斜杠会产出非法 JSON ⇒
         *   这里用 build_telemetry_json()（内置转义），⛔ 不再走 cJSON（C6 无 PSRAM，
         *   Wi-Fi 重连时堆只剩 ~10 KB，真机在这段出现过 `cJSON OOM ⇒ 本帧丢弃`）。 */
        {
            uint8_t tlvbuf[TELE_TLV_MAX];
            size_t tlv_used = (e->tlv_len < sizeof(tlvbuf)) ? e->tlv_len : sizeof(tlvbuf);
            size_t gps_need = 0;
            bool have_gps = s_gps.valid && (now - s_gps.ms) <= GW_GPS_FRESH_MS;
            bool have_extra = s_binds[idx].extra_tlv_len > 0u &&
                              (now - s_binds[idx].extra_tlv_ms) <= GW_EXTRA_TLV_FRESH_MS;
            size_t n;
            bool cut;

            memcpy(tlvbuf, e->tlv, tlv_used);
            if (have_gps) {
                gps_need = (s_gps.fix != 0u) ? GW_TLV_GNSS_BYTES : 6u;   /* 26 / (sats+fix) 6 */
                if (tlv_used + gps_need <= sizeof(tlvbuf)) {
                    tlv_used = gw_tlv_append_gps(tlvbuf, tlv_used, &s_gps);
                } else {
                    ESP_LOGW(TAG, "TLV 放不下 GPS（%u + %u > %u）⇒ 本帧只报温度",
                             (unsigned)tlv_used, (unsigned)gps_need, (unsigned)sizeof(tlvbuf));
                }
            }
            if (have_extra) {
                size_t extra_need = s_binds[idx].extra_tlv_len;
                if (tlv_used + extra_need <= sizeof(tlvbuf)) {
                    memcpy(tlvbuf + tlv_used, s_binds[idx].extra_tlv, extra_need);
                    tlv_used += extra_need;
                } else {
                    ESP_LOGW(TAG, "TLV 放不下子设备通用 TLV（%u + %u > %u）⇒ 本帧丢弃该段",
                             (unsigned)tlv_used, (unsigned)extra_need, (unsigned)sizeof(tlvbuf));
                }
            }
            n = (tlv_used < TELE_TLV_MAX) ? tlv_used : (size_t)TELE_TLV_MAX;
            cut = (tlv_used > n) || e->tlv_trunc;

            hex_upper(tlvbuf, n, tlv_hex, sizeof(tlv_hex));
            if (!build_telemetry_json(data, sizeof(data), s_binds[idx].node, s_binds[idx].model,
                                      (int)e->rssi, e->cmd, tlv_hex, cut)) {
                ESP_LOGW(TAG, "sub.telemetry data 超过缓冲（上限 %u B）⇒ 本帧丢弃",
                         (unsigned)sizeof(data));
                return;
            }
            if (have_gps) {
                s_gps_merged++;
                if ((s_gps_merged % 10u) == 1u) {
                    ESP_LOGI(TAG, "[gnss] 已并入 GPS TLV：fix=%u sats=%u（tlv=%u B）",
                             (unsigned)s_gps.fix, (unsigned)s_gps.sats,
                             (unsigned)tlv_used);
                }
            }
        }

        if (emit_event(GW_EVT_SUB_TELEMETRY, data)) {
            s_last_tele_ms = now;
        }
        break;
    }

    default:
        break;
    }
}

/* ---------------------------------------------------------------- 命令处理（工作任务） */

static void ack_command(const char *id, const char *err_msg)
{
    oneye_dev_sdk_err_t rc;

    if (id == NULL || id[0] == '\0') {
        return;   /* 没有 id 就没有回执关联依据（契约按 id 关联） */
    }
    rc = oneye_dev_base_ack_command(id, err_msg);
    if (rc == ONEYE_DEV_SDK_OK) {
        ESP_LOGI(TAG, "command/up id=%s status=%s", id, (err_msg == NULL) ? "ok" : "error");
    } else {
        ESP_LOGW(TAG, "command/up 回执发送失败（id=%s, %s）：%s",
                 id, (err_msg == NULL) ? "ok" : err_msg, oneye_dev_sdk_strerror(rc));
    }
}

/* ========================================================================
 * 2026-10-23（真机定位）：外部档案（child.pair 下发 / NVS 恢复）的**完整性校验**＋内置回退
 *
 * 事故逐字：云端第一次注入的是"扁平最小档"（串口 `已注册档案 model=T31（114 B）`），
 *   它把链接层启动时注册的**内置完整 T31 档案**（profiles/t31.json，517 B，含
 *   match / gatt / frame / commands[] / passthrough）按「同 model 覆盖」规则冲掉
 *   ⇒ 之后扫描匹配、收帧、TLV 归一化全部不成立（EMQX 已投递 child.pair，但子设备链没起来）。
 *
 * ⇒ 规则（本文件唯一出口，两处调用：do_pair / gw_child_restore_profiles）：
 *   · 注册任何外部来源的档案前先跑 {@link profile_missing_key}；
 *   · 缺任一必需键 ⇒ ⛔ 不注册该 JSON；改注册内置 T31 档案（并把内置档写回 NVS 自愈）。
 *
 * 校验清单（缺一即失败；返回值 = 缺失键名，供日志逐字打印）：
 *   match.name_prefix / match.service_uuid / gatt.notify_uuid /
 *   frame.sop / frame.eop / frame.body_len（必须 ≥ 0）
 * 说明：只校验"扫描 + 收帧"必需项；commands[] / passthrough 允许缺省
 *   （透传模式下仍能上报原始帧，见 link-sdk PROTOCOL § passthrough）。
 * ======================================================================== */

/** `obj[key]` 是非空字符串？（obj/key 任一缺失恒 false，⛔ 不失配崩） */
static bool json_has_text(const cJSON *obj, const char *key)
{
    const cJSON *it;

    if (obj == NULL || key == NULL) {
        return false;
    }
    it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(it) && it->valuestring != NULL && it->valuestring[0] != '\0';
}

/** `obj[key]` 是 ≥0 的数字？（缺省/负数/非数字均 false） */
static bool json_has_nonneg_num(const cJSON *obj, const char *key)
{
    const cJSON *it;

    if (obj == NULL || key == NULL) {
        return false;
    }
    it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) && it->valueint >= 0;
}

/** `commands[]` 里至少有一条 `telemetry:true` 且 `fields[]` 非空 —— 否则档案能扫描/连接，
 *  但**解不出任何 TLV**（真机：云端"扁平最小档"仅 114 B，commands 缺失 ⇒ 全部帧退化成
 *  RAW_FRAME、温度进不了 TLV 0x10）。 */
static bool profile_has_decodable_cmd(const cJSON *doc)
{
    const cJSON *cmds;
    const cJSON *c;

    if (doc == NULL) {
        return false;
    }
    cmds = cJSON_GetObjectItemCaseSensitive(doc, "commands");
    if (!cJSON_IsArray(cmds)) {
        return false;
    }
    cJSON_ArrayForEach(c, cmds) {
        const cJSON *tel = cJSON_GetObjectItemCaseSensitive(c, "telemetry");
        const cJSON *flds = cJSON_GetObjectItemCaseSensitive(c, "fields");

        if (cJSON_IsTrue(tel) && cJSON_IsArray(flds) && (cJSON_GetArraySize(flds) > 0)) {
            return true;
        }
    }
    return false;
}

/** 校验外部档案的**结构完整性**；返回 NULL = 合法，否则返回第一个缺失键名（静态串）。 */
static const char *profile_missing_key(const char *json, size_t len)
{
    cJSON *doc;
    const cJSON *match;
    const cJSON *gatt;
    const cJSON *frame;
    const char *miss = NULL;

    if (json == NULL || len == 0) {
        return "JSON(空)";
    }
    doc = cJSON_ParseWithLength(json, len);
    if (doc == NULL) {
        return "JSON(解析失败)";
    }
    match = cJSON_GetObjectItemCaseSensitive(doc, "match");
    gatt = cJSON_GetObjectItemCaseSensitive(doc, "gatt");
    frame = cJSON_GetObjectItemCaseSensitive(doc, "frame");

    if (!json_has_text(match, "name_prefix")) {
        miss = "match.name_prefix";
    } else if (!json_has_text(match, "service_uuid")) {
        miss = "match.service_uuid";
    } else if (!json_has_text(gatt, "notify_uuid")) {
        miss = "gatt.notify_uuid";
    } else if (!json_has_text(frame, "sop")) {
        miss = "frame.sop";
    } else if (!json_has_text(frame, "eop")) {
        miss = "frame.eop";
    } else if (!json_has_nonneg_num(frame, "body_len")) {
        miss = "frame.body_len";
    } else if (!profile_has_decodable_cmd(doc)) {
        /* 2026-10-23 真机第 2 次：云端下发 114 B 扁平档——match/gatt/frame 都在、commands 缺失，
         *   旧校验放行 ⇒ 覆盖掉 SIM 完整档 ⇒ 每帧只有 RAW_FRAME、温度解不出。 */
        miss = "commands[].fields";
    }
    cJSON_Delete(doc);
    return miss;
}

/* ★ 2026-10-23（真机解码修正）：本工程的**完整自洽 T31 档案**（唯一事实源；声明与理由见 gw_child.h）。
 * 内容与 main.c 原先就地写的那份一致，只是把 frame 偏移**改对**（见下）。
 *
 * 🔴 frame 偏移的实测口径（oneye_link_profile.c:455/465，⛔ 不要再按"含 SOP 的绝对偏移"填）：
 *   解析器会自己 +1 个 SOP 字节：
 *     cmd_pos  = has_sop ? (1 + cmd_offset)  : cmd_offset
 *     body_off = has_sop ? (1 + body_offset) : body_offset
 *   本仓帧 = `A6 <cmd> <payload> <checksum> 6A`（t31_sim.c）⇒ cmd 在绝对偏移 1、body 在绝对偏移 2
 *   ⇒ 正确取值 = cmd_offset **0**、body_offset **1**（旧值 1/2 是绝对偏移，真机据此解出 cmd=0x00
 *   并把整帧退化成 RAW_FRAME[8]）。
 *   `body_len: 0` = 动态取长（= end - body_off，end 已扣 EOP；含末尾 checksum 字节，不读它即可）。 */
static const char *SIM_PROFILE_T31 =
    "{"
    "\"schema\":1,\"model\":\"T31\",\"vendor\":\"Genial\","
    "\"match\":{\"name_prefix\":\"T31\",\"service_uuid\":\"fff0\"},"
    "\"gatt\":{\"notify_uuid\":\"fff1\",\"write_uuid\":\"fff2\",\"require_bond\":false},"
    "\"frame\":{\"sop\":\"A6\",\"eop\":\"6A\",\"cmd_offset\":0,\"body_offset\":1,\"body_len\":0},"
    "\"commands\":["
    "{\"cmd\":\"01\",\"telemetry\":true,\"fields\":["
    "{\"name\":\"temperature\",\"type\":\"i16le\",\"offset\":0,\"scale\":0.01,\"tlv\":16},"
    "{\"name\":\"seq\",\"type\":\"u16le\",\"offset\":2,\"scale\":1,\"tlv\":2}]},"
    "{\"cmd\":\"B1\",\"telemetry\":true,\"fields\":["
    "{\"name\":\"battery\",\"type\":\"u8\",\"offset\":0,\"scale\":1,\"tlv\":8},"
    "{\"name\":\"voltage\",\"type\":\"u16le\",\"offset\":1,\"scale\":1,\"tlv\":9}]}"
    "],"
    "\"passthrough\":{\"unknown_cmd\":true,\"raw_frame\":true}"
    "}";

const char *gw_child_sim_profile_t31(void)
{
    return SIM_PROFILE_T31;
}

/** 回退：注册**本工程的完整自洽 T31 档案**（同 model 幂等覆盖；main.c 启动时也注册过一次）。
 *  ⚠️ 为什么优先它而不是 link-sdk 内置 t31.json：内置档的 `cmd 0x01.fields` 为空且
 *     `frame.body_len=8` ⇒ 本仓 COM13 的 8 B 帧里温度进不了 TLV 0x10，transport 会丢弃该帧。
 *     本档是本仓真机唯一验证过"能产出温度"的档。
 *  @return 注册的字节数；0 = 注册失败。 */
static size_t register_builtin_t31(void)
{
    size_t n = strlen(SIM_PROFILE_T31);

    if (oneye_link_profile_register_json(SIM_PROFILE_T31, n) == ONEYE_LINK_OK) {
        return n;
    }
    /* 最后兜底：link-sdk 内置 T31（结构完整，能扫描/连接；温度只能靠透传原始帧） */
    {
        const char *bi = oneye_link_profile_builtin_t31();

        if (bi == NULL || bi[0] == '\0') {
            return 0;
        }
        n = strlen(bi);
        return (oneye_link_profile_register_json(bi, n) == ONEYE_LINK_OK) ? n : 0;
    }
}

/* ========================================================================
 * ★🔴 2026-10-23 开机自检（用户要求）：用**硬编码样本帧**验证「SIM 完整档」的解码路径。
 *
 * 为什么必须自检：真机连续两轮出现「帧字节完全正确、却退化成 RAW_FRAME[8]」——
 *   说明运行时生效的档案不是 SIM 档（或 frame{} 参数错），**只看运行日志无法区分**。
 *   ⇒ 这里在 `link_start` **之前**、对 SIM 档跑一次**离线解码**，把答案钉死在串口上。
 *
 * 复刻的是 link-sdk `oneye_link_frame_decode()` 的偏移语义（`oneye_link_profile.c:455/465`，
 * 本仓已逐行核对；⛔ 不改 SDK）：
 *   cmd_pos  = has_sop ? (1 + cmd_offset)  : cmd_offset
 *   body_off = has_sop ? (1 + body_offset) : body_offset
 *   body_len = body_len>0 ? body_len : (end - body_off)   // end 已扣 EOP
 * 样本帧 = t31_sim.c 的真实帧：`A6 01 54 0E 81 18 FC 6A`
 *   → cmd=0x01、TEMP=0x0E54=3668(0.01℃)、SEQ=0x1881、期望 TLV = TEMP(4)+SEQ(4)+RAW(10) = 18 B。
 * ======================================================================== */

static const char *jget_str(const cJSON *o, const char *k, const char *dflt)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);

    return (cJSON_IsString(it) && it->valuestring != NULL) ? it->valuestring : dflt;
}

static int jget_int(const cJSON *o, const char *k, int dflt)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsNumber(it) ? it->valueint : dflt;
}

/** SIM 档的有效载荷编码宽度（与 link-sdk TLV 注册表一致：TEMP/SEQ 2 B、BATTERY_PCT 1 B） */
static size_t field_width(const char *type)
{
    if (type == NULL) {
        return 0;
    }
    if (strcmp(type, "u8") == 0 || strcmp(type, "i8") == 0) {
        return 1;
    }
    if (strcmp(type, "u16le") == 0 || strcmp(type, "u16be") == 0 ||
        strcmp(type, "i16le") == 0 || strcmp(type, "i16be") == 0) {
        return 2;
    }
    return 0;
}

void gw_child_profile_selftest(void)
{
    static const uint8_t sample[] = { 0xA6u, 0x01u, 0x54u, 0x0Eu, 0x81u, 0x18u, 0xFCu, 0x6Au };
    const char *sim = SIM_PROFILE_T31;
    char errbuf[96];
    oneye_link_err_t vrc;
    cJSON *doc;
    const cJSON *fr;
    const cJSON *cmds;
    size_t n_cmds = 0;
    size_t n_fields = 0;
    bool has_sop;
    bool has_eop;
    int cmd_offset;
    int body_offset;
    int body_len_cfg;
    size_t start;
    size_t end = sizeof(sample);
    size_t cmd_pos;
    size_t body_off;
    size_t body_len;
    uint8_t cmd = 0;
    int64_t temp_centi = -1;
    size_t tlv_len = 0;
    size_t fields_hit = 0;
    bool ok = true;

    errbuf[0] = '\0';
    vrc = oneye_link_profile_validate(sim, strlen(sim), errbuf, sizeof(errbuf));
    ESP_LOGI(TAG, "[自检] SIM 档 validate=%d（%s），%u B；运行时注册表 %u 条",
             (int)vrc, (errbuf[0] != '\0') ? errbuf : "ok",
             (unsigned)strlen(sim), (unsigned)oneye_link_profile_count());
    for (size_t i = 0; i < oneye_link_profile_count(); i++) {
        const char *m = oneye_link_profile_model_at(i);

        ESP_LOGI(TAG, "[档案] 注册表[%u] model=%s", (unsigned)i, (m != NULL) ? m : "(null)");
    }

    doc = cJSON_ParseWithLength(sim, strlen(sim));
    if (doc == NULL) {
        ESP_LOGE(TAG, "[自检] SIM 档 JSON 解析失败");
        return;
    }
    fr = cJSON_GetObjectItemCaseSensitive(doc, "frame");
    cmds = cJSON_GetObjectItemCaseSensitive(doc, "commands");
    has_sop = json_has_text(fr, "sop");
    has_eop = json_has_text(fr, "eop");
    cmd_offset = jget_int(fr, "cmd_offset", 1);
    body_offset = jget_int(fr, "body_offset", 2);
    body_len_cfg = jget_int(fr, "body_len", 0);

    if (cJSON_IsArray(cmds)) {
        const cJSON *c;

        n_cmds = (size_t)cJSON_GetArraySize(cmds);
        cJSON_ArrayForEach(c, cmds) {
            n_fields += (size_t)cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(c, "fields"));
        }
    }
    ESP_LOGI(TAG, "[档案] 运行时生效 model=T31 来源=内置SIM(%u B) 命令数=%u 字段数=%u"
                  "（同 model 最后注册者生效；main.c 顺序：内置→NVS→SIM）",
             (unsigned)strlen(sim), (unsigned)n_cmds, (unsigned)n_fields);

    /* ---- 复刻 link-sdk 解码 ---- */
    start = has_sop ? 1u : 0u;
    if (has_eop) {
        end = sizeof(sample) - 1u;
    }
    if (has_sop && (sample[0] != (uint8_t)strtoul(jget_str(fr, "sop", "0"), NULL, 16))) {
        ok = false;
    }
    if (ok && has_eop &&
        (sample[sizeof(sample) - 1u] != (uint8_t)strtoul(jget_str(fr, "eop", "0"), NULL, 16))) {
        ok = false;
    }
    cmd_pos = has_sop ? (size_t)(1 + cmd_offset) : (size_t)cmd_offset;
    if (ok && (cmd_pos >= end || cmd_pos < start)) {
        ok = false;
    }
    if (ok) {
        cmd = sample[cmd_pos];
        body_off = (has_sop ? 1u : 0u) + (size_t)body_offset;
        body_len = (body_len_cfg > 0) ? (size_t)body_len_cfg : (end - body_off);
        if (body_off >= end || (body_off + body_len) > end) {
            ok = false;
        } else if (cJSON_IsArray(cmds)) {
            const cJSON *c;

            cJSON_ArrayForEach(c, cmds) {
                const char *cmd_s = jget_str(c, "cmd", "");
                const cJSON *flds;

                if ((uint8_t)strtoul(cmd_s, NULL, 16) != cmd) {
                    continue;
                }
                flds = cJSON_GetObjectItemCaseSensitive(c, "fields");
                if (!cJSON_IsArray(flds)) {
                    continue;
                }
                for (const cJSON *f = flds->child; f != NULL; f = f->next) {
                    const char *ft = jget_str(f, "type", "");
                    size_t w = field_width(ft);
                    int off = jget_int(f, "offset", 0);
                    int tlv = jget_int(f, "tlv", 0);
                    const uint8_t *q;

                    if (w == 0 || off < 0 || ((size_t)off + w) > body_len) {
                        continue;
                    }
                    q = &sample[body_off + (size_t)off];
                    if (strcmp(ft, "i16le") == 0) {
                        int16_t v = (int16_t)(uint16_t)((uint16_t)q[0] | ((uint16_t)q[1] << 8));
                        if (tlv == 16) { /* ONEYE_TLV_TEMP */
                            temp_centi = (int64_t)v * 1; /* scale=0.01 → 原始即 0.01℃ 单位 */
                        }
                    } else if (strcmp(ft, "u16le") == 0) {
                        /* 其他字段仅计入 TLV 宽度 */
                    }
                    if (tlv > 0) {
                        tlv_len += 2u + w; /* type + len + payload */
                        fields_hit++;
                    }
                }
                break;
            }
        }
        if (json_has_text(cJSON_GetObjectItemCaseSensitive(doc, "passthrough"), "raw_frame") ||
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                cJSON_GetObjectItemCaseSensitive(doc, "passthrough"), "raw_frame"))) {
            tlv_len += 2u + sizeof(sample); /* RAW_FRAME(0x80) */
        }
    }
    cJSON_Delete(doc);

    ESP_LOGI(TAG, "[自检] 样本帧 %u B → cmd=0x%02X temp=%d(0.01℃) tlv=%u B"
                  "（frame{cmd_offset=%d,body_offset=%d,body_len=%d}，字段命中=%u）",
             (unsigned)sizeof(sample), (unsigned)cmd, (int)temp_centi, (unsigned)tlv_len,
             cmd_offset, body_offset, body_len_cfg, (unsigned)fields_hit);
    if (!ok || cmd != 0x01u || temp_centi != 3668) {
        ESP_LOGE(TAG, "[自检] ❌ 与预期不符（应 cmd=0x01 temp=3668 且 tlv=18 B）"
                      "⇒ 是 frame{} 参数错，按本行读数订正后再烧；⛔ 不要怀疑子设备帧");
    } else {
        ESP_LOGI(TAG, "[自检] ✅ 通过：frame{} 与 SIM 档字段映射自洽（真机若仍 passthrough ⇒"
                      "检查运行时生效档是否被 NVS/云端档覆盖）");
    }

    /* 🔴 2026-10-25：GNSS 定位帧（cmd 0x02）**离线自检** —— 用硬编码样本帧走一遍网关侧解码。
     *   为什么必须在开机自检里验一次：室内/无天线时模组一直 fix=0，**真机根本走不到有经纬度
     *   的分支**；若把"解码/并 TLV"这段留到有信号时才第一次执行，等于把 bug 留到户外现场。
     *   样本值：lat 39.9123456° / lon 116.4123456° / alt 43 m / speed 0.50 m/s / sats 9 / fix 2。 */
    {
        gw_gps_t g;
        uint8_t f[GW_GPS_FRAME_LEN] = { 0 };
        int32_t lat = 399123456;
        int32_t lon = 1164123456;
        int16_t alt = 43;
        uint16_t spd = 50;
        uint8_t sum = 0;
        bool dok;

        f[0] = 0xA6u;
        f[1] = 0x02u;
        f[2] = (uint8_t)((uint32_t)lat & 0xFFu);
        f[3] = (uint8_t)(((uint32_t)lat >> 8) & 0xFFu);
        f[4] = (uint8_t)(((uint32_t)lat >> 16) & 0xFFu);
        f[5] = (uint8_t)(((uint32_t)lat >> 24) & 0xFFu);
        f[6] = (uint8_t)((uint32_t)lon & 0xFFu);
        f[7] = (uint8_t)(((uint32_t)lon >> 8) & 0xFFu);
        f[8] = (uint8_t)(((uint32_t)lon >> 16) & 0xFFu);
        f[9] = (uint8_t)(((uint32_t)lon >> 24) & 0xFFu);
        f[10] = (uint8_t)((uint16_t)alt & 0xFFu);
        f[11] = (uint8_t)(((uint16_t)alt >> 8) & 0xFFu);
        f[12] = (uint8_t)(spd & 0xFFu);
        f[13] = (uint8_t)((spd >> 8) & 0xFFu);
        f[14] = 9u;
        f[15] = 2u;
        for (size_t i = 1; i < GW_GPS_FRAME_LEN - 2u; i++) {
            sum = (uint8_t)(sum + f[i]);
        }
        f[GW_GPS_FRAME_LEN - 2u] = sum;
        f[GW_GPS_FRAME_LEN - 1u] = 0x6Au;

        dok = gw_gps_decode_frame(f, sizeof(f), &g);
        if (!dok || !g.valid || g.fix != 2u || g.sats != 9u ||
            g.lat_e7 != lat || g.lon_e7 != lon || g.alt_m != alt || g.speed_cms != spd) {
            ESP_LOGE(TAG, "[自检] ❌ GPS 样本帧解码失败（ok=%d lat=%ld lon=%ld alt=%d spd=%u fix=%u）"
                          "⇒ GPS 帧格式与网关解码不一致，按 t31_sim.c 文件头订正",
                     (int)dok, (long)g.lat_e7, (long)g.lon_e7, (int)g.alt_m,
                     (unsigned)g.speed_cms, (unsigned)g.fix);
        } else {
            ESP_LOGI(TAG, "[自检] ✅ GPS 样本帧 %u B → lat=%.7f lon=%.7f alt=%d m speed=%.2f m/s "
                          "sats=%u fix=%u（TLV 将并出 26 B：0x50–0x55）",
                     (unsigned)sizeof(f), (double)g.lat_e7 / 1e7, (double)g.lon_e7 / 1e7,
                     (int)g.alt_m, (double)g.speed_cms / 100.0,
                     (unsigned)g.sats, (unsigned)g.fix);
        }
    }
}

static void do_pair(const gw_cmd_t *c)
{
    char errbuf[96];
    gw_child_bind_t nb;
    int idx;
    int free_idx = -1;
    oneye_link_err_t prc;
    const char *missing = NULL;
    bool use_builtin = false;

    if (c->node[0] == '\0') {
        ack_command(c->id, "missing node");
        return;
    }
    if (c->model[0] == '\0') {
        ack_command(c->id, "missing model");
        return;
    }
    if (c->profile_too_large) {
        ESP_LOGE(TAG, "linkProfile 超过 %d B 上限 ⇒ 拒绝配对（node=%s）",
                 (int)GW_CHILD_PROFILE_MAX, c->node);
        ack_command(c->id, "linkProfile too large");
        return;
    }

    /* 档案先**校验**（不注册）：JSON 不合法就别动 NVS，避免留下"重启后注册失败"的坏档 */
    if (c->has_profile) {
        errbuf[0] = '\0';
        prc = oneye_link_profile_validate(c->profile, c->profile_len, errbuf, sizeof(errbuf));
        if (prc != ONEYE_LINK_OK) {
            ESP_LOGE(TAG, "linkProfile 校验失败（%d）：%s ⇒ 拒绝配对（node=%s）",
                     (int)prc, (errbuf[0] != '\0') ? errbuf : "(无说明)", c->node);
            ack_command(c->id, "linkProfile invalid");
            return;
        }

        /* ★ 2026-10-23 真机定位：**结构完整性**再校验一遍（link-sdk 的 validate 只保证 JSON 自洽，
         *   不保证"可扫描可收帧"）。真机事故 = 云端下发 114 B 扁平档，把内置 517 B 完整 T31 档案
         *   按同 model 覆盖掉 ⇒ 之后扫描匹配/收帧全不成立。缺必需键 ⇒ ⛔ 不注册该 JSON，
         *   改注册内置 T31 档案（并把内置档写回 NVS，重启后 restore 不再撞坏档）。 */
        missing = profile_missing_key(c->profile, c->profile_len);
        if (missing != NULL) {
            ESP_LOGW(TAG, "下发档案校验失败(缺%s) ⇒ 回退内置 T31 档案（node=%s）",
                     missing, c->node);
            use_builtin = true;
        }
    }

    idx = bind_find_by_node(c->node);
    if (idx < 0) {
        free_idx = bind_find_free();
        if (free_idx < 0) {
            ESP_LOGE(TAG, "绑定表已满（上限 %d）⇒ 拒绝配对（node=%s）",
                     (int)GW_CHILD_MAX, c->node);
            ack_command(c->id, "child table full");
            return;
        }
        idx = free_idx;
    }

    memset(&nb, 0, sizeof(nb));
    nb.used = true;
    str_copy(nb.node, sizeof(nb.node), c->node);
    str_copy(nb.product_key, sizeof(nb.product_key), c->product_key);
    str_copy(nb.model, sizeof(nb.model), c->model);
    /* 重配对时保留已学到的 BLE 地址（云端不知道地址，丢了就得等下次连上才补） */
    if (s_binds[idx].used) {
        str_copy(nb.addr, sizeof(nb.addr), s_binds[idx].addr);
    }
    /* 2026-10-26〔子设备发现〕云端带来的"选中地址"（可选）：
     * 非空 ⇒ 本绑定锁定该 MAC（bind_resolve 只接受它）；空 ⇒ 保留旧锁定值（重配对不会丢锁定）。 */
    if (c->addr[0] != '\0') {
        str_copy(nb.want_addr, sizeof(nb.want_addr), c->addr);
    } else if (s_binds[idx].used) {
        str_copy(nb.want_addr, sizeof(nb.want_addr), s_binds[idx].want_addr);
    }

    /* ① 先落 NVS（fail-closed：写不进去就不注册档案、也不 ack ok） */
    if (nvs_store_bind((size_t)idx, &nb) != ESP_OK) {
        ESP_LOGE(TAG, "绑定写 NVS 失败 ⇒ 拒绝配对（node=%s）", c->node);
        ack_command(c->id, "nvs write failed");
        return;
    }
    /* 内存先落地：下面 nvs_store_count() 是按内存表数的，晚一步就会把新绑定漏掉 */
    s_binds[idx] = nb;

    /* NVS 里的档案：只存**通过完整性校验**的那一份；回退时存内置 T31（自愈 —— 重启后
     * restore 读到的就是合法档，不再每次开机都撞一次坏档）。 */
    if (c->has_profile && !use_builtin) {
        if (nvs_store_profile((size_t)idx, c->profile) != ESP_OK) {
            ESP_LOGE(TAG, "linkProfile 写 NVS 失败 ⇒ 撤销本次配对（node=%s）", c->node);
            (void)nvs_erase_bind((size_t)idx);
            memset(&s_binds[idx], 0, sizeof(s_binds[idx]));
            (void)nvs_store_count();
            ack_command(c->id, "nvs write failed");
            return;
        }
    } else if (use_builtin) {
        const char *bi = oneye_link_profile_builtin_t31();
        if (bi == NULL || nvs_store_profile((size_t)idx, bi) != ESP_OK) {
            ESP_LOGW(TAG, "内置 T31 档案写 NVS 失败 ⇒ 重启后靠启动期注册的内置档运行（node=%s）",
                     c->node);
        }
    }
    if (nvs_store_count() != ESP_OK) {
        ESP_LOGW(TAG, "绑定条数写 NVS 失败（下次开机按槽位扫描仍能恢复，仅条数不准）");
    }

    /* ② 再注册档案（link-sdk 的**运行时**档案表；重启后由 gw_child_restore_profiles 恢复） */
    if (c->has_profile && !use_builtin) {
        prc = oneye_link_profile_register_json(c->profile, c->profile_len);
        if (prc != ONEYE_LINK_OK) {
            ESP_LOGE(TAG, "档案注册失败（%d）⇒ 撤销本次配对（node=%s）", (int)prc, c->node);
            (void)nvs_erase_bind((size_t)idx);
            memset(&s_binds[idx], 0, sizeof(s_binds[idx]));
            (void)nvs_store_count();
            ack_command(c->id, "profile register failed");
            return;
        }
        ESP_LOGI(TAG, "已注册档案 model=%s（来源=下发，%u B）",
                 c->model, (unsigned)c->profile_len);

        /* 🔴 2026-10-26 真机踩坑（解绑后重配 ⇒ 云端档覆盖解码档，温度全变 RAW_FRAME）：
         *   开机顺序本是 内置 → NVS(云端档) → SIM（**SIM 最后注册生效**，自检通过）；
         *   但 `child.pair` 在运行期把云端档注册成"同 model 最后注册者" ⇒ 解码档被换成云端档，
         *   本仓 T31 = COM4 模拟器，其帧/字段映射与云端 668 B 档不一致 ⇒ cmd 被解成 0xA6、
         *   TLV 只剩 RAW_FRAME，云端按"没有温度"丢弃 ⇒ App 显示离线。
         *   ⇒ 运行期重配后**再注册一次内置完整档**，保证解码口径与开机自检一致（T31 专属）。 */
        if (strcmp(c->model, "T31") == 0) {
            size_t rn = register_builtin_t31();
            ESP_LOGI(TAG, "T31 解码档复位：已重新注册内置完整档（%u B，来源=内置）",
                     (unsigned)rn);
        }
    } else {
        size_t bn = register_builtin_t31();
        if (bn == 0) {
            ESP_LOGE(TAG, "内置 T31 档案注册失败 ⇒ 子设备将无法匹配（检查 link-sdk 内置档案）");
        } else {
            ESP_LOGI(TAG, "已注册档案 model=T31（来源=内置，%u B）", (unsigned)bn);
        }
        if (!c->has_profile) {
            ESP_LOGW(TAG, "child.pair 未带 linkProfile ⇒ 使用内置 T31 档案（model=%s）", c->model);
        }
    }

    ESP_LOGI(TAG, "★ 子设备已配对：node=%s model=%s productKey=%s（槽位 %d）%s",
             nb.node, nb.model, (nb.product_key[0] != '\0') ? nb.product_key : "(未给)",
             idx, use_builtin ? " ⚠️ 下发档案残缺，已回退内置" :
                  ((c->has_profile) ? "" : " ⚠️ 无 linkProfile"));
    ESP_LOGI(TAG, "  BLE 地址：学到=%s / 锁定=%s（锁定非空 ⇒ 只连该地址）",
             (nb.addr[0] != '\0') ? nb.addr : "(未学到)",
             (nb.want_addr[0] != '\0') ? nb.want_addr : "(未指定)");
    /* 2026-10-25〔共存降载〕绑定数 +1 ⇒ 让 link-sdk 重新开扫（直到新子设备连上）。 */
    sync_target_links();
    ack_command(c->id, NULL);
}

static void do_unpair(const gw_cmd_t *c)
{
    int idx;
    char model[GW_CHILD_MODEL_MAX];
    char addr[GW_CHILD_ADDR_MAX];

    idx = bind_find_by_node(c->node);
    if (idx < 0) {
        ESP_LOGW(TAG, "child.unpair：node=%s 不在绑定表里（可能已解绑）", c->node);
        ack_command(c->id, "child not found");
        return;
    }

    str_copy(model, sizeof(model), s_binds[idx].model);
    str_copy(addr, sizeof(addr), s_binds[idx].addr);

    /* ① 清 NVS + 内存槽位（失败也要继续把内存清掉：不留"看着还在绑定"的半套状态） */
    if (nvs_erase_bind((size_t)idx) != ESP_OK) {
        ESP_LOGW(TAG, "绑定删除写 NVS 失败（node=%s）⇒ 重启后可能又出现该绑定", c->node);
    }
    memset(&s_binds[idx], 0, sizeof(s_binds[idx]));
    if (nvs_store_count() != ESP_OK) {
        ESP_LOGW(TAG, "绑定条数写 NVS 失败（仅条数不准）");
    }

    /* ② 停止对该子设备的匹配：还有别的绑定用同一 model 时**不能**注销档案 */
    if (model[0] != '\0' && !model_in_use(model, -1)) {
        oneye_link_err_t urc = oneye_link_profile_unregister(model);
        ESP_LOGI(TAG, "已注销档案 model=%s（%d；在线连接不受影响，重连后不再匹配）",
                 model, (int)urc);
    }

    /* ③ 已连着的就主动断开（解绑后不该继续代传它的数据） */
    if (addr[0] != '\0') {
        oneye_link_addr_t a;
        if (oneye_link_addr_parse(addr, &a)) {
            oneye_link_err_t drc = oneye_link_disconnect(&a);
            ESP_LOGI(TAG, "主动断开子设备 %s（%d）", addr, (int)drc);
        }
    }

    ESP_LOGI(TAG, "★ 子设备已解绑：node=%s model=%s", c->node, model);
    /* 2026-10-25〔共存降载〕绑定数 -1 ⇒ 目标数变小，link-sdk 视情况停扫。 */
    sync_target_links();
    ack_command(c->id, NULL);
}

static void handle_cmd(const gw_cmd_t *c)
{
    if (strcmp(c->service, GW_SVC_PAIR) == 0) {
        do_pair(c);
    } else if (strcmp(c->service, GW_SVC_UNPAIR) == 0) {
        do_unpair(c);
    } else {
        /* 未知命令必须回执 error（不回时云端 dev_command.status 会停到超时） */
        ESP_LOGW(TAG, "未知 service=\"%s\"（id=%s）⇒ 回执 error",
                 c->service, (c->id[0] != '\0') ? c->id : "(无 id)");
        ack_command(c->id, "unknown service");
    }
}

/* ---------------------------------------------------------------- 工作任务 */

static void gw_child_task(void *arg)
{
    (void)arg;
    for (;;) {
        gw_cmd_t cmd;

        /* 2026-10-26：云断清队列申请——在本任务（普通上下文）里执行，⛔ 不在 SDK 回调里调 SDK。 */
        if (s_purge_req) {
            s_purge_req = false;
            uint32_t purged = oneye_dev_sdk_purge_event_queue();
            ESP_LOGW(TAG, "云断 ⇒ 已清空 SDK 事件队列 %u 帧（把内存让给 TLS 重连）",
                     (unsigned)purged);
        }

        /* 2026-10-26〔子设备发现〕发现窗口状态机：开窗（8 s 低占空比扫描）→ 关窗 → 汇总上报。
         * 云断不动作（on_cloud_state 会关扫描并清窗口），⛔ 断网时白耗射频。 */
        {
            int64_t now = now_ms();
            if (s_disc_window_end_ms != 0 && now >= s_disc_window_end_ms) {
                s_disc_window_end_ms = 0;
                oneye_link_set_discovery(false);
                gw_disc_report();
            } else if (s_disc_window_end_ms == 0 && s_cloud_up &&
                       now - s_disc_last_report_ms >= GW_DISC_REPORT_MS) {
                s_disc_last_report_ms = now;
                oneye_link_set_discovery(true);
                s_disc_window_end_ms = now + GW_DISC_SCAN_WINDOW_MS;
            }
        }

        /* 命令优先（低频、要回执）；最多等一个轮询窗口，随后排空事件队列 */
        if (xQueueReceive(s_cmd_q, &cmd, pdMS_TO_TICKS(GW_CHILD_POLL_MS)) == pdPASS) {
            handle_cmd(&cmd);
        }
        {
            gw_sub_evt_t e;
            while (xQueueReceive(s_sub_q, &e, 0) == pdPASS) {
                handle_sub(&e);
            }
        }
    }
}

bool gw_child_report_start(void)
{
    if (s_task != NULL) {
        return true;
    }
    if (s_cmd_q == NULL) {
        s_cmd_q = xQueueCreate(GW_CHILD_CMD_Q_DEPTH, sizeof(gw_cmd_t));
    }
    if (s_sub_q == NULL) {
        s_sub_q = xQueueCreate(GW_CHILD_SUB_Q_DEPTH, sizeof(gw_sub_evt_t));
    }
    if (s_cmd_q == NULL || s_sub_q == NULL) {
        ESP_LOGE(TAG, "队列创建失败（命令 %u B×%d / 事件 %u B×%d）⇒ child.pair 与 sub.* 都不可用",
                 (unsigned)sizeof(gw_cmd_t), (int)GW_CHILD_CMD_Q_DEPTH,
                 (unsigned)sizeof(gw_sub_evt_t), (int)GW_CHILD_SUB_Q_DEPTH);
        return false;
    }
    if (xTaskCreate(gw_child_task, "gw_child", GW_CHILD_TASK_STACK, NULL,
                    GW_CHILD_TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "工作任务创建失败（内存不足）⇒ child.pair 与 sub.* 都不可用");
        return false;
    }
    ESP_LOGI(TAG, "子设备上报任务已起（stack=%d）：命令 %u B×%d / 事件 %u B×%d；"
                  "遥测节流 %d ms",
             (int)GW_CHILD_TASK_STACK, (unsigned)sizeof(gw_cmd_t), (int)GW_CHILD_CMD_Q_DEPTH,
             (unsigned)sizeof(gw_sub_evt_t), (int)GW_CHILD_SUB_Q_DEPTH,
             (int)CONFIG_ONEYE_T31_GW_SUB_TELE_MIN_MS);

    /* 2026-10-26〔子设备发现〕只挂回调；**扫描由任务里的发现窗口节制**（8 s/30 s），
     * 不再常开（子设备连上后 link-sdk 本来就会停扫，常开也拿不到广播）。 */
    if (oneye_link_register_disc_cb(gw_disc_on_adv, NULL) == ONEYE_LINK_OK) {
        ESP_LOGI(TAG, "子设备发现回调已挂（窗口 %d ms / 周期 %d ms）",
                 GW_DISC_SCAN_WINDOW_MS, GW_DISC_REPORT_MS);
    } else {
        ESP_LOGW(TAG, "子设备发现回调注册失败 ⇒ App 侧只能看到已配对子设备");
    }
    return true;
}

/* ---------------------------------------------------------------- 回调侧（只解析 + 投递） */

/* 取一个字符串字段（不存在/非字符串 → 空串）。 */
static void json_str(const cJSON *obj, const char *key, char *out, size_t cap)
{
    const cJSON *it;

    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!cJSON_IsObject(obj)) {
        return;
    }
    it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring != NULL) {
        str_copy(out, cap, it->valuestring);
    }
}

void gw_child_on_command(const void *payload, size_t len)
{
    cJSON *doc;
    const cJSON *params;
    const cJSON *prof;
    char *prof_txt = NULL;      /* IDF 自带 cJSON 的 PrintUnformatted 返回 char*（不是 cJSON*） */
    gw_cmd_t cmd;

    if (payload == NULL || len == 0) {
        return;
    }
    if (s_cmd_q == NULL) {
        ESP_LOGW(TAG, "命令队列未就绪 ⇒ 本次下发丢弃（链路刚起时会短暂出现，云端可重发）");
        return;
    }

    /* MQTT 载荷**不保证 NUL 结尾** ⇒ 必须用带长度的解析 */
    doc = cJSON_ParseWithLength((const char *)payload, len);
    if (doc == NULL) {
        ESP_LOGW(TAG, "命令负载不是合法 JSON（%u B）⇒ 无法回执（契约按 id 关联）", (unsigned)len);
        return;
    }
    memset(&cmd, 0, sizeof(cmd));

    json_str(doc, "id", cmd.id, sizeof(cmd.id));
    params = cJSON_GetObjectItemCaseSensitive(doc, "params");

    /* service：契约示例放在 params 里；为兼容也接受顶层同名键 */
    json_str(params, "service", cmd.service, sizeof(cmd.service));
    if (cmd.service[0] == '\0') {
        json_str(doc, "service", cmd.service, sizeof(cmd.service));
    }

    /* node：用户给定键名是 node；另接受 childNode 作为别名（两者取到哪个用哪个） */
    json_str(params, "node", cmd.node, sizeof(cmd.node));
    if (cmd.node[0] == '\0') {
        json_str(params, "childNode", cmd.node, sizeof(cmd.node));
    }
    /* productKey：真机日志里云端下发的是 `childProductKey`（身份键 productKey 被命令面禁用作
     * params 键名）⇒ 三个键名都接受：childProductKey → productKey → product_key。
     * ⚠️ 顺序即优先级；⛔ 不要只留一个（真机已实测 `productKey=(未给)`）。 */
    json_str(params, "childProductKey", cmd.product_key, sizeof(cmd.product_key));
    if (cmd.product_key[0] == '\0') {
        json_str(params, "productKey", cmd.product_key, sizeof(cmd.product_key));
    }
    if (cmd.product_key[0] == '\0') {
        json_str(params, "product_key", cmd.product_key, sizeof(cmd.product_key));
    }
    json_str(params, "model", cmd.model, sizeof(cmd.model));
    /* 2026-10-26〔子设备发现〕App 从"发现候选"里选中的子设备 MAC（可选键 `addr`）：
     * 有 ⇒ 锁定只连该地址；无 ⇒ 老行为（按 model 匹配 + 首次连上学习地址）。 */
    json_str(params, "addr", cmd.addr, sizeof(cmd.addr));

    /* linkProfile：既可能是**对象**（用户给定的形态），也可能是已序列化的字符串 */
    prof = cJSON_GetObjectItemCaseSensitive(params, "linkProfile");
    if (cJSON_IsString(prof) && prof->valuestring != NULL && prof->valuestring[0] != '\0') {
        cmd.profile_len = strlen(prof->valuestring);
        if (cmd.profile_len >= sizeof(cmd.profile)) {
            cmd.profile_too_large = true;
            cmd.profile_len = 0;
        } else {
            memcpy(cmd.profile, prof->valuestring, cmd.profile_len);
            cmd.profile[cmd.profile_len] = '\0';
            cmd.has_profile = true;
        }
    } else if (cJSON_IsObject(prof)) {
        /* cJSON_PrintUnformatted 会 malloc 一份：小对象、立刻释放，且本回调不做任何 IO/阻塞 */
        prof_txt = cJSON_PrintUnformatted(prof);
        if (prof_txt != NULL) {
            cmd.profile_len = strlen(prof_txt);
            if (cmd.profile_len >= sizeof(cmd.profile)) {
                cmd.profile_too_large = true;
                cmd.profile_len = 0;
            } else {
                memcpy(cmd.profile, prof_txt, cmd.profile_len);
                cmd.profile[cmd.profile_len] = '\0';
                cmd.has_profile = true;
            }
            cJSON_free(prof_txt);
        }
    }

    if (xQueueSend(s_cmd_q, &cmd, 0) != pdPASS) {
        ESP_LOGW(TAG, "命令队列满 ⇒ 丢弃 service=%s id=%s（云端会超时）",
                 (cmd.service[0] != '\0') ? cmd.service : "(未给)",
                 (cmd.id[0] != '\0') ? cmd.id : "(无 id)");
    } else {
        ESP_LOGI(TAG, "收到命令 service=%s id=%s node=%s model=%s（已投递队列）",
                 (cmd.service[0] != '\0') ? cmd.service : "(未给)",
                 (cmd.id[0] != '\0') ? cmd.id : "(无 id)",
                 (cmd.node[0] != '\0') ? cmd.node : "(未给)",
                 (cmd.model[0] != '\0') ? cmd.model : "(未给)");
    }
    cJSON_Delete(doc);
}

void gw_child_on_link_event(const oneye_link_event_t *evt)
{
    gw_sub_evt_t e;

    if (evt == NULL || s_sub_q == NULL) {
        return;
    }
    memset(&e, 0, sizeof(e));
    str_copy(e.model, sizeof(e.model), evt->model);
    (void)oneye_link_addr_str(&evt->sub, e.addr, sizeof(e.addr));
    e.rssi = evt->rssi;

    switch (evt->type) {
    case ONEYE_LINK_EVT_SUB_ONLINE:
        e.kind = GW_SUB_KIND_ONLINE;
        break;
    case ONEYE_LINK_EVT_SUB_OFFLINE:
        e.kind = GW_SUB_KIND_OFFLINE;
        break;
    case ONEYE_LINK_EVT_SUB_ERROR:
        e.kind = GW_SUB_KIND_ERROR;
        e.err = (int16_t)evt->err;
        break;
    case ONEYE_LINK_EVT_SUB_DATA:
        e.kind = GW_SUB_KIND_TELEMETRY;
        e.cmd = evt->cmd;
        e.has_cmd = evt->has_cmd;
        /* evt->tlv = 归一化 TLV（档案解出的）；evt->raw = **原始 notify 字节**
         * （oneye_link_ble.c:148-149 恒填，命中档案时也有）⇒ 两者都给云端一份。 */
        e.tlv_len = (uint8_t)copy_bytes(e.tlv, sizeof(e.tlv), evt->tlv, evt->tlv_len, &e.tlv_trunc);
        e.raw_len = (uint8_t)copy_bytes(e.raw, sizeof(e.raw), evt->raw, evt->raw_len, &e.raw_trunc);
        break;
    default:
        return;   /* CLOUD / STATS 与本模块无关 */
    }

    if (xQueueSend(s_sub_q, &e, 0) != pdPASS) {
        s_sub_q_dropped++;
        /* 🔴 2026-10-23：队列满时必须能看出"任务活着但慢"还是"任务没起"——打印深度/任务句柄。 */
        if ((s_sub_q_dropped % 10u) == 1u) {
            ESP_LOGW(TAG, "子设备事件队列满 ⇒ 丢弃 kind=%u（%s；累计丢 %u；队列 %u/%u；任务=%s）",
                     (unsigned)e.kind,
                     (e.addr[0] != '\0') ? e.addr : "(未知地址)",
                     (unsigned)s_sub_q_dropped,
                     (unsigned)((s_sub_q != NULL) ? uxQueueMessagesWaiting(s_sub_q) : 0u),
                     (unsigned)GW_CHILD_SUB_Q_DEPTH,
                     (s_task != NULL) ? "已起" : "**未起**");
        }
    }
}

/* ---------------------------------------------------------------- 启动恢复 */

void gw_child_boot(void)
{
    size_t n = nvs_load_binds();

    ESP_LOGI(TAG, "子设备绑定：NVS 恢复 %u 条（上限 %d）；档案在 oneye_link_init 之后注册",
             (unsigned)n, (int)GW_CHILD_MAX);
    for (size_t i = 0; i < GW_CHILD_MAX; i++) {
        if (!s_binds[i].used) {
            continue;
        }
        ESP_LOGI(TAG, "  [%u] node=%s model=%s productKey=%s addr=%s", (unsigned)i,
                 s_binds[i].node,
                 (s_binds[i].model[0] != '\0') ? s_binds[i].model : "(空)",
                 (s_binds[i].product_key[0] != '\0') ? s_binds[i].product_key : "(未给)",
                 (s_binds[i].addr[0] != '\0') ? s_binds[i].addr : "(未学到)");
    }
    /* 2026-10-25〔共存降载〕把"已绑定子设备数"同步给 link-sdk：
     *   全部在线后停扫（省射频给 Wi-Fi）；本函数早于 oneye_link_init，但 setter 只写目标值，
     *   NimBLE 同步完成（ble_scan_start）时自然按目标判断。 */
    sync_target_links();
}

void gw_child_restore_profiles(void)
{
    /* 1 KB scratch 放静态区（⛔ 不放栈：本函数在 cloud_start_task 里跑，栈只有 6144） */
    static char prof[GW_CHILD_PROFILE_MAX];

    for (size_t i = 0; i < GW_CHILD_MAX; i++) {
        size_t n;

        if (!s_binds[i].used) {
            continue;
        }
        n = nvs_load_profile(i, prof, sizeof(prof));
        if (n == 0) {
            ESP_LOGW(TAG, "[%u] node=%s 没有存档案（配对时未带 linkProfile）⇒ 依赖已注册的内置档案",
                     (unsigned)i, s_binds[i].node);
            continue;
        }
        /* ★ 2026-10-23：NVS 里的档案也要过**同一套完整性校验**。
         *   真机已有一条"扁平最小档"（114 B）被写进 NVS ⇒ 只修 child.pair 分支不够，
         *   重启后这里会把它再注册回来、再次覆盖内置完整档。
         *   不合法 ⇒ 跳过该条 + 注册内置 T31 + 把内置档写回该槽位（自愈，下次开机即合法）。 */
        {
            const char *missing = profile_missing_key(prof, n);

            if (missing != NULL) {
                size_t bn = register_builtin_t31();
                const char *bi = oneye_link_profile_builtin_t31();

                ESP_LOGW(TAG, "[档案] NVS[%u] 校验=失败(缺%s) model=%s ⇒ 用内置 T31 替代"
                              "（内置 %u B / 原档 %u B）",
                         (unsigned)i, missing, s_binds[i].model, (unsigned)bn, (unsigned)n);
                if (bn == 0) {
                    ESP_LOGE(TAG, "[%u] 内置 T31 档案注册失败 ⇒ 重启后该子设备无法匹配", (unsigned)i);
                }
                if (bi != NULL && nvs_store_profile(i, bi) != ESP_OK) {
                    ESP_LOGW(TAG, "[%u] 内置档写回 NVS 失败（下次开机仍会撞坏档，但不影响本次运行）",
                             (unsigned)i);
                }
                continue;
            }
            ESP_LOGI(TAG, "[档案] NVS[%u] 校验=通过（%u B，model=%s）",
                     (unsigned)i, (unsigned)n, s_binds[i].model);
        }
        {
            oneye_link_err_t rc = oneye_link_profile_register_json(prof, n);
            if (rc == ONEYE_LINK_OK) {
                ESP_LOGI(TAG, "[%u] 已恢复档案 model=%s（来源=NVS，%u B）", (unsigned)i,
                         s_binds[i].model, (unsigned)n);
            } else {
                ESP_LOGE(TAG, "[%u] 恢复档案失败（%d）model=%s ⇒ 该子设备重启后不匹配"
                              "（需云端重发 child.pair）", (unsigned)i, (int)rc, s_binds[i].model);
            }
        }
    }
}

#endif /* CONFIG_ONEYE_T31_ROLE_GATEWAY */
