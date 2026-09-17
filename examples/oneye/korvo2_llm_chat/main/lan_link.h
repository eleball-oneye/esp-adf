/*
 * lan_link.h —— 本地面「局域网信道」数据面（UDP 发现 + HTTP 帧面）
 *
 * 契约：contracts/local/lan-link.md
 *   - 发现：设备监听 `57321/UDP`，收到 `{"t":"oneye.discover","v":1}` 后**单播**回 `node.announce`；
 *   - 帧面：`POST /api/link/frame`（单帧 ≤8 KB，`Content-Type: application/json`），
 *     设备把帧交给 `oneye_dev_link_inject_frame(..., CHAN_LAN)`；
 *   - 安全：本信道**明文**，仅台面/研发（Kconfig `ONEYE_LLM_ENABLE_LAN_LINK`，量产必须 n）；
 *     无配对令牌时**只**接受 `prov.hello` / `link.ping` / `node.announce`（契约 §2）。
 *
 * 实现说明（契约待办，需在 R-L1 会签时定稿）：
 *   设备→手机方向在本实现中走 **HTTP 响应体**（`oneye_dev_link` 的 lan 发送器把待发帧写入
 *   单槽回复缓冲，下一次 HTTP 请求的响应体即为该帧；无待发帧时回 `{}`）。
 *   contracts/local/lan-link.md §2 只定义了请求方向；异步推送（手机侧 HTTP 端点/长轮询）
 *   尚未定义，故本实现**不臆造**协议，改为「响应携带」并在 README 的未决项中登记。
 */

#ifndef _LAN_LINK_H_
#define _LAN_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *node_id;   /* 本机节点 id（node.announce.from） */
    const char *name;      /* 展示名 */
    const char *caps;      /* 本地面能力（逗号分隔） */
    const char *channels;  /* 可达信道（逗号分隔，须含 lan） */
    uint16_t    http_port; /* HTTP 帧面端口（写进 node.announce 的 p.http_port） */
} lan_link_config_t;

/** 启动 UDP 发现应答 + HTTP 帧面，并把 lan 信道注册为 link 的出站发送器 */
esp_err_t lan_link_init(const lan_link_config_t *cfg);

/** 设置配对令牌（BLE 配对成功后由应用下发；空 = 清除）。未设置时按契约只放行白名单帧 */
void lan_link_set_token(const char *token);

bool lan_link_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* _LAN_LINK_H_ */
