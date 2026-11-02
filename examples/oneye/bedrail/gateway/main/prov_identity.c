/*
 * prov_identity.c —— **会话级节点身份**（`prov.verify.req` 的激活码）的唯一落点
 *
 * 口径、三条硬边界、为什么走 SDK 公开的 `frame_cb` 而不是 internal 头：见 `prov_identity.h` 头注。
 *
 * **旧边界逐字留痕（⛔ 不抹）**：「⛔ 本文件**不碰**任何存储（无 `nvs_*` / 无 `esp_partition_*` / 无文件）
 * —— 只有一块 RAM。」
 * 🔴 **2026-10-20〔尽力写 nvs 批〕就地订正**：按用户裁定「乙型可能无 nvs 等分区：**只尝试写 nvs**，
 *   写失败**不阻断**」⇒ 本文件新增两件事（**都只在"尽力而为"这一档**）：
 *   ① [prov_identity_nvs_save]：把 RAM 里的 node 写进 nvs（`esp_err_t` 原样返回，调用方只打日志）；
 *   ② [prov_identity_nvs_load]：读回上次写入的 node，**只**供"本次上电没收到 `prov.verify.req`"
 *       （已存 Wi-Fi 凭据、开机直接入网）这一档兜底。
 *   ⛔ 仍然**不碰** `creds` 分区、⛔ 不碰 SDK 的绑定记录命名空间、⛔ 不在 nvs 里声称"云侧已确认"。
 *   命名空间/键：`oneye_prov` / `node`（都 ≤ 15 字符，符合 NVS 上限）。
 *
 * 为什么不引 cJSON：本工程 `main/CMakeLists.txt` 的 `REQUIRES` 里有 IDF 的 `json`，而
 * `oneye-dev-sdk` 在**源码回落**路径（`tools/build-devkitc.sh` 强制走的那条）下也编了一份
 * vendor cJSON（`CMakeLists.txt:301-309` 登记了"与 IDF json 同镜像可能 `multiple definition`"的
 * 已知遗留）⇒ 为了**一行身份**去拉一整个 cJSON 解析器，会把一个已知的链接期风险引进来。
 * 本帧的形状是**契约冻结**的（`envelope{v,t,from,p}` ＋ `p{code}`），故这里用一个**只认结构**的
 * 最小扫描器：⛔ 不解析任意 JSON、⛔ 不猜、⛔ 不做宽松容忍（形状不对 ⇒ 一个字节都不记）。
 */

#include "prov_identity.h"

#include <string.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "prov_ident";

/* NVS 存放面（⚠️ 与 SDK `oneye_ble_bindid` 的命名空间**不是同一个**；理由见头注 ①）。 */
#define PROV_IDENTITY_NVS_NS  "oneye_prov"
#define PROV_IDENTITY_NVS_KEY "node"

/* ---------------------------------------------------------------- 会话状态（只有这一块 RAM） */

static struct {
    bool     valid;                            /* 记过一个**合法**的 code */
    char     node[PROV_IDENTITY_NODE_MAX + 1u];/* NUL 结尾（含 NUL ≤ 65 B） */
    unsigned hits;                             /* 收到过几次（W2 = App 绑定成功后重发，>1） */
} s_prov_node;

/* 写侧在 NimBLE 主机任务、读侧在 `wifi_ready` 任务 ⇒ 用临界区把"valid + 串"当一个整体换入换出。
 * ⚠️ 临界区里**只**做定长 `memcpy`（≤ 65 B），⛔ 不放日志、不放任何调用 —— 这是 NimBLE 主机任务。 */
static portMUX_TYPE s_prov_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---------------------------------------------------------------- 最小结构扫描 */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    return p;
}

/* `p` 指向开引号；返回**闭引号之后**的位置；未闭合 ⇒ NULL。
 * ⚠️ 支持 `\"` / `\\` 的跳过（只需要"不误判结束"，⛔ 不解码）。 */
static const char *skip_string(const char *p, const char *end)
{
    if (p >= end || *p != '"') {
        return NULL;
    }
    p++;
    while (p < end) {
        if (*p == '\\') {
            p += 2;
            continue;
        }
        if (*p == '"') {
            return p + 1;
        }
        p++;
    }
    return NULL;
}

/* 跳过一个任意值（字符串 / 对象 / 数组 / 字面量）；返回其**之后**的位置；失败 ⇒ NULL */
static const char *skip_value(const char *p, const char *end)
{
    p = skip_ws(p, end);
    if (p >= end) {
        return NULL;
    }
    if (*p == '"') {
        return skip_string(p, end);
    }
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        int  depth = 0;
        while (p < end) {
            if (*p == '"') {
                const char *q = skip_string(p, end);
                if (q == NULL) {
                    return NULL;
                }
                p = q;
                continue;
            }
            if (*p == open) {
                depth++;
            } else if (*p == close) {
                depth--;
                if (depth == 0) {
                    return p + 1;
                }
            }
            p++;
        }
        return NULL;
    }
    /* 字面量（数字 / true / false / null）：到结构分隔符为止 */
    while (p < end && *p != ',' && *p != '}' && *p != ']') {
        p++;
    }
    return p;
}

/*
 * 在对象 `obj`（必须以 '{' 开头、`obj_end` 为其配对 '}' 之后）的**第 1 层**找 `key`。
 * 找到 ⇒ 返回 1（字符串值：`*v` 指向**引号内**内容、`*vlen` = 内容长度）
 *        或 2（非字符串值：`*v` = 值起点、`*vlen` = 值整段长度）；
 * 找不到 ⇒ 0。
 * ⛔ 只认第 1 层（嵌套里的同名键不算）—— 本帧的 `code` 就在 `p` 的第 1 层。
 */
static int obj_get(const char *obj, const char *obj_end, const char *key, size_t klen,
                   const char **v, size_t *vlen)
{
    const char *p = skip_ws(obj, obj_end);

    if (p >= obj_end || *p != '{') {
        return 0;
    }
    p++;
    for (;;) {
        const char *kstart;
        const char *kend;
        size_t      this_klen;

        p = skip_ws(p, obj_end);
        if (p >= obj_end) {
            return 0;
        }
        if (*p == '}') {
            return 0;
        }
        if (*p != '"') {
            return 0; /* 形状不对：不猜 */
        }
        kstart = p + 1;
        kend = skip_string(p, obj_end);
        if (kend == NULL) {
            return 0;
        }
        this_klen = (size_t)((kend - 1) - kstart);
        p = skip_ws(kend, obj_end);
        if (p >= obj_end || *p != ':') {
            return 0;
        }
        p = skip_ws(p + 1, obj_end);
        if (this_klen == klen && memcmp(kstart, key, klen) == 0) {
            const char *vend = skip_value(p, obj_end);
            if (vend == NULL || vend <= p) {
                return 0;
            }
            if (*p == '"') {
                *v = p + 1;
                *vlen = (size_t)((vend - 1) - (p + 1));
                return 1;
            }
            *v = p;
            *vlen = (size_t)(vend - p);
            return 2;
        }
        p = skip_value(p, obj_end);
        if (p == NULL) {
            return 0;
        }
        p = skip_ws(p, obj_end);
        if (p < obj_end && *p == ',') {
            p++;
            continue;
        }
        if (p < obj_end && *p == '}') {
            return 0;
        }
        return 0;
    }
}

/* 契约 `ble_gatt.pair.code_charset`：`^[A-Za-z0-9._-]{1,64}$`（⛔ 不 trim / ⛔ 不折叠大小写） */
static bool node_shape_ok(const char *s, size_t len)
{
    size_t i;

    if (len == 0u || len > (size_t)PROV_IDENTITY_NODE_MAX) {
        return false;
    }
    for (i = 0u; i < len; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- 对外 */

void prov_identity_frame_cb(const char *json, size_t len, void *ctx)
{
    const char *end;
    const char *t;
    const char *p;
    const char *code;
    size_t      tlen = 0u;
    size_t      plen = 0u;
    size_t      clen = 0u;
    bool        changed = false;

    (void)ctx;

    if (json == NULL || len == 0u) {
        return;
    }
    end = json + len;

    /* ① `t` 必须是 `prov.verify.req`（其余帧一律不看：本模块**只**关心身份交接） */
    if (obj_get(json, end, "t", 1u, &t, &tlen) != 1 || tlen != 15u ||
        memcmp(t, "prov.verify.req", 15u) != 0) {
        return;
    }
    /* ② `p` 必须是对象，且其第 1 层有字符串 `code` */
    if (obj_get(json, end, "p", 1u, &p, &plen) != 2) {
        return;
    }
    if (obj_get(p, p + plen, "code", 4u, &code, &clen) != 1) {
        ESP_LOGW(TAG, "prov.verify.req 缺 `code`（p=%u B）⇒ 本帧不采用（⛔ 不编一个身份出来）",
                 (unsigned)plen);
        return;
    }
    /* ③ 形态判据：与 SDK/契约**同一**字符集与上界（空码 / 超长 / 非法字符一律不采用）。
     * ⚠️ 这一步**不是**身份证明（权威在云侧准入）；它只保证"接下来拿去当 MQTT username /
     *    client_id 的串"是合法的 —— 非法串交给 SDK 会在 `base_init` 里被判 INVALID 而不联网。 */
    if (!node_shape_ok(code, clen)) {
        ESP_LOGW(TAG, "prov.verify.req 的 `code` 形态非法（%u B，要求 ^[A-Za-z0-9._-]{1,64}$）"
                      "⇒ 不采用（⛔ 不截断、⛔ 不 trim 后采用）", (unsigned)clen);
        return;
    }

    taskENTER_CRITICAL(&s_prov_mux);
    changed = (!s_prov_node.valid) || (strlen(s_prov_node.node) != clen) ||
              (memcmp(s_prov_node.node, code, clen) != 0);
    memcpy(s_prov_node.node, code, clen);
    s_prov_node.node[clen] = '\0';
    s_prov_node.valid = true;
    s_prov_node.hits++;
    taskEXIT_CRITICAL(&s_prov_mux);

    if (changed) {
        /* ⚠️ 这条日志的取值口径与 `adv_identity.c:87` 一致：激活码**本来就会**出现在启动日志里
         *    （甲型的核验基准），本行只是把"它从哪来"讲清；⛔ 不额外扩散到别的通道。 */
        ESP_LOGI(TAG, "收到 prov.verify.req ⇒ 会话身份 node=%s（来源=**激活码**；"
                      "用途=建云链路的 device_id/username/client_id；⛔ 仅 RAM、⛔ 不落盘）",
                 s_prov_node.node);
    }
}

bool prov_identity_get(char *out, size_t cap)
{
    bool ok;

    if (out != NULL && cap > 0u) {
        out[0] = '\0';
    }
    if (out == NULL || cap == 0u) {
        return false;
    }
    taskENTER_CRITICAL(&s_prov_mux);
    ok = s_prov_node.valid;
    if (ok) {
        size_t n = strlen(s_prov_node.node);
        if (n + 1u > cap) {
            ok = false; /* 装不下 ⇒ 拒绝（⛔ 不产出半截 node） */
        } else {
            memcpy(out, s_prov_node.node, n + 1u);
        }
    }
    taskEXIT_CRITICAL(&s_prov_mux);
    return ok;
}

unsigned prov_identity_hits(void)
{
    unsigned n;

    taskENTER_CRITICAL(&s_prov_mux);
    n = s_prov_node.hits;
    taskEXIT_CRITICAL(&s_prov_mux);
    return n;
}

/* ---------------------------------------------------------------- nvs（**尽力而为**，⛔ 不阻断） */

esp_err_t prov_identity_nvs_save(const char *node)
{
    nvs_handle_t h;
    esp_err_t    err;

    if (node == NULL || node[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!node_shape_ok(node, strlen(node))) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 无 nvs 分区 / `nvs_flash_init` 失败 ⇒ 这一步就返回错误
     * （乙型「可能没有 nvs 分区」是**合法配置**，⛔ 不是故障；调用方只告警）。 */
    err = nvs_open(PROV_IDENTITY_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, PROV_IDENTITY_NVS_KEY, node);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t prov_identity_nvs_probe(void)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(PROV_IDENTITY_NVS_NS, NVS_READWRITE, &h);

    if (err == ESP_OK) {
        nvs_close(h);
    }
    return err;
}

bool prov_identity_nvs_load(char *out, size_t cap)
{
    nvs_handle_t h;
    esp_err_t    err;
    size_t       need = 0u;

    if (out != NULL && cap > 0u) {
        out[0] = '\0';
    }
    if (out == NULL || cap == 0u) {
        return false;
    }
    err = nvs_open(PROV_IDENTITY_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_get_str(h, PROV_IDENTITY_NVS_KEY, NULL, &need);
    if (err != ESP_OK || need == 0u || need > cap) {
        nvs_close(h);
        return false;
    }
    err = nvs_get_str(h, PROV_IDENTITY_NVS_KEY, out, &need);
    nvs_close(h);
    if (err != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    /* 形态判据与"采用"那一侧**同一份**（`^[A-Za-z0-9._-]{1,64}$`）：盘上的串也不许绕过形态。 */
    if (!node_shape_ok(out, strlen(out))) {
        out[0] = '\0';
        return false;
    }
    return true;
}
