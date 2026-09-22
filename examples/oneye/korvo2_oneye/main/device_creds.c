// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

#include "device_creds.h"

#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char *TAG = "device_creds";

/* 小端读取（镜像格式是显式小端，不依赖主机字节序）。 */
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *device_creds_strerror(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                  return "ok";
    case ESP_ERR_NOT_FOUND:       return "creds 分区不存在，或还没写过（空 / 全 0xFF）";
    case ESP_ERR_INVALID_CRC:     return "creds 分区内容坏了：CRC 不符（写坏了，或写了别的设备的镜像）";
    case ESP_ERR_INVALID_VERSION: return "creds 镜像版本不认识（固件太旧？）";
    case ESP_ERR_INVALID_ARG:     return "creds 镜像内容不合规（缺字段 / JSON 不合法 / 头与内容不一致）";
    case ESP_ERR_NO_MEM:          return "内存不足";
    default:                      return "未知错误";
    }
}

bool device_creds_is_corrupt(esp_err_t err)
{
    /* ⚠️ 只有 NOT_FOUND 允许回退。其余都表示"这台设备的凭证有问题"，回退等于拿公用凭证
     *    冒充它上线 —— 那是比不联网更糟的结果（会被平台当成合法设备接受）。 */
    return err != ESP_OK && err != ESP_ERR_NOT_FOUND;
}

static char *dup_str(const cJSON *obj, const char *key, bool required)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(it) || it->valuestring == NULL || it->valuestring[0] == '\0') {
        return required ? NULL : strdup("");
    }
    return strdup(it->valuestring);
}

esp_err_t device_creds_load(device_creds_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)DEVICE_CREDS_PARTITION_SUBTYPE,
        DEVICE_CREDS_PARTITION_NAME);
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t hdr[DEVICE_CREDS_HEADER_LEN];
    esp_err_t err = esp_partition_read(part, 0, hdr, sizeof(hdr));
    if (err != ESP_OK) {
        return err;
    }
    if (memcmp(hdr, DEVICE_CREDS_MAGIC, DEVICE_CREDS_MAGIC_LEN) != 0) {
        /* 空分区（全 0 或全 0xFF）走到这里：这是"没写过"，**不是**"坏了"。 */
        return ESP_ERR_NOT_FOUND;
    }
    uint16_t version = rd_u16(hdr + 8);
    if (version != 1) {
        ESP_LOGE(TAG, "分区版本 %u，本固件只认 1", (unsigned)version);
        return ESP_ERR_INVALID_VERSION;
    }
    uint16_t flags = rd_u16(hdr + 10);
    uint32_t n = rd_u32(hdr + 12);
    uint32_t want_crc = rd_u32(hdr + 16);
    if (n == 0 || n > part->size - DEVICE_CREDS_HEADER_LEN) {
        ESP_LOGE(TAG, "头部声称 %u 字节，超过分区容量 %u", (unsigned)n, (unsigned)part->size);
        return ESP_ERR_INVALID_ARG;
    }

    char *body = malloc(n + 1);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_partition_read(part, DEVICE_CREDS_HEADER_LEN, body, n);
    if (err != ESP_OK) {
        free(body);
        return err;
    }
    body[n] = '\0';

    uint32_t got_crc = esp_rom_crc32_le(0, (const uint8_t *)body, n);
    if (got_crc != want_crc) {
        ESP_LOGE(TAG, "CRC 不符（头部 %08x，实算 %08x）", (unsigned)want_crc, (unsigned)got_crc);
        free(body);
        return ESP_ERR_INVALID_CRC;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);   /* cJSON 自己复制了字符串 */
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    out->node_id = dup_str(root, "node_id", true);
    out->cert_pem = dup_str(root, "cert_pem", true);
    out->key_pem = dup_str(root, "key_pem", (flags & 0x1) != 0);
    out->ca_pem = dup_str(root, "ca_pem", true);
    out->mac = dup_str(root, "mac", false);
    cJSON_Delete(root);

    if (out->node_id == NULL || out->cert_pem == NULL || out->ca_pem == NULL ||
        ((flags & 0x1) != 0 && out->key_pem == NULL)) {
        /* 头里说有私钥而内容里没有，属于两边实现漂移 —— 明确报出来，不要当成"少一个字段"。 */
        ESP_LOGE(TAG, "镜像缺必填字段（或头与内容不一致）");
        device_creds_free(out);
        return ESP_ERR_INVALID_ARG;
    }
    out->cert_len = strlen(out->cert_pem);
    out->key_len = strlen(out->key_pem ? out->key_pem : "");
    out->ca_len = strlen(out->ca_pem);
    return ESP_OK;
}

void device_creds_free(device_creds_t *c)
{
    if (c == NULL) {
        return;
    }
    free(c->node_id);
    free(c->mac);
    free(c->cert_pem);
    free(c->key_pem);
    free(c->ca_pem);
    memset(c, 0, sizeof(*c));
}
