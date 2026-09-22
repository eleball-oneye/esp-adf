// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0
//
// 设备凭证分区（`creds`）的**设备侧读取**——设计 §8.3。
//
// 为什么有它：今天证书与私钥是**编译进固件**的（objcopy 嵌进去），固件因而不通用、私钥随固件
// 扩散、换证书要重烧固件。量产口径改成"**通用固件 + 产线写一次凭证分区**"，于是设备启动时要
// 先去这个分区取自己的身份。
//
// 读取顺序与失败姿态（**两条都是刻意的**）：
//   1. **分区优先**：`creds` 分区里有合法镜像 ⇒ 用它，且 `device_id/username/client_id` 都取自
//      分区里的 node_id（**不再**用编译期常量 —— 否则通用固件这一条不成立）。
//   2. **分区存在但坏了 ⇒ 不回退**（fail-closed）：CRC 不对、版本不认识、JSON 不合法都属于
//      "**这台设备的凭证是坏的**"，此时回退到内嵌证书意味着"拿一份公用凭证冒充它上线"，
//      比不联网更糟。调用方据此**不要继续联网**，并把原因打到串口。
//   3. **分区没写过（空/全 0xFF）⇒ 回退**到编译进固件的那份（开发板方便），但日志必须说清
//      走的是哪一份；量产固件应把内嵌证书关掉（`ONEYE_FW_EMBED_CERTS` 不定义），于是这条回退
//      自然不存在。
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 分区名与类型（与 `partitions.csv` 必须一致）。 */
#define DEVICE_CREDS_PARTITION_NAME "creds"
#define DEVICE_CREDS_PARTITION_SUBTYPE 0x40

/** 镜像头（与 backend 的 `src/utils/credsimage` 逐字节对应）。 */
#define DEVICE_CREDS_MAGIC "ONEYECR1"
#define DEVICE_CREDS_MAGIC_LEN 8
#define DEVICE_CREDS_HEADER_LEN 20

/** 载入结果：字符串都在堆上，用完调用 device_creds_free()。 */
typedef struct {
    char *node_id;         /* 证书 CN = MQTT client_id/username */
    char *mac;             /* 可空（清单里没给 MAC 时） */
    char *cert_pem;        /* 设备证书（PEM，NUL 结尾） */
    char *key_pem;         /* 设备私钥（PEM） */
    char *ca_pem;          /* 信任锚（PEM，可能多张） */
    size_t cert_len;
    size_t key_len;
    size_t ca_len;
} device_creds_t;

/**
 * 从 `creds` 分区读取凭证。
 *
 * @return
 *   - ESP_OK                    读到合法镜像
 *   - ESP_ERR_NOT_FOUND         分区不存在，或**没写过**（空 / 全 0xFF）⇒ 调用方可以回退
 *   - ESP_ERR_INVALID_CRC       镜像坏了（CRC 不符）⇒ **不要回退**，这台设备该被拦下
 *   - ESP_ERR_INVALID_VERSION   版本不认识（固件太旧 / 镜像太新）⇒ 不要回退
 *   - ESP_ERR_INVALID_ARG       内容不合规（缺字段、JSON 不合法、头里声称有私钥而内容没有）
 *   - ESP_ERR_NO_MEM            内存不足
 */
esp_err_t device_creds_load(device_creds_t *out);

/** 释放 device_creds_load 分配的字符串（可重复调用）。 */
void device_creds_free(device_creds_t *c);

/** 把上面的错误码翻译成**给人看的一句话**（串口日志与自检都用它，别让现场猜）。 */
const char *device_creds_strerror(esp_err_t err);

/** 这个错误码是否属于"分区坏了"（= 不该回退，该拦下设备）。 */
bool device_creds_is_corrupt(esp_err_t err);

#ifdef __cplusplus
}
#endif
