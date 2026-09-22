/*
 * 产测自证实现 —— 契约见 backend/contracts/domain/设备凭据分区与产测自证契约.md §6
 *
 * 密码学选型（与验签工具 src/tools/provverify 一一对应，改一边必须改另一边）：
 *   · 摘要   SHA-256
 *   · 签名   ECDSA P-256，**DER 形式**（ASN.1 ECDSA-Sig-Value）—— mbedtls 的 pk_sign 就是这个形式，
 *            Go 侧用 ecdsa.VerifyASN1 直接验
 *   · 指纹   SHA-256 over **证书 DER**，小写十六进制（= `openssl x509 -fingerprint -sha256` 同口径）
 *   · nonce  16 字节，取自 CSPRNG（**不能用弱随机**：ECDSA 的随机数一旦重用或可预测，私钥可被算出来）
 */

#include "prov_attest.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "prov_attest";

/* 把规范化原文拼成一个缓冲（避免用 snprintf 拼接可变长度字段时反复截断）。 */
static int build_message(const char *node_id, const char *mac, const char *cert_fp,
                         const char *nonce_hex, char *out, size_t cap)
{
    int n = snprintf(out, cap, "ONEYE-PROV1\nnode_id=%s\nmac=%s\ncert_fp=%s\nnonce=%s\n",
                     node_id, mac ? mac : "", cert_fp, nonce_hex);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return n;
}

static void hex_lower(const unsigned char *in, size_t len, char *out)
{
    static const char *D = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = D[in[i] >> 4];
        out[i * 2 + 1] = D[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

esp_err_t prov_attest_line(const char *node_id, const char *mac,
                           const char *cert_pem, const char *key_pem,
                           char *out, size_t cap)
{
    if (!node_id || !cert_pem || !key_pem || !out || cap < 256) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rc = ESP_FAIL;
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    bool crt_ok = false, pk_ok = false, drbg_ok = false;

    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    unsigned char nonce[16];
    unsigned char digest[32];
    unsigned char sig[160];          /* P-256 DER 签名 ~72 B，留足余量 */
    size_t sig_len = 0, b64_len = 0;
    char cert_fp[65];
    char nonce_hex[33];
    char b64[256];
    char msg[512];

    /* 1) 证书：解析 PEM → 取 DER → 指纹。顺带校验它确实是证书（解析不了就不产出半截行）。 */
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert_pem, strlen(cert_pem) + 1) != 0 ||
        crt.raw.len == 0) {
        ESP_LOGE(TAG, "设备证书解析失败 —— 不产出自证串");
        goto done;
    }
    crt_ok = true;
    {
        unsigned char fp[32];
        if (mbedtls_sha256(crt.raw.p, crt.raw.len, fp, 0) != 0) {
            goto done;
        }
        hex_lower(fp, sizeof(fp), cert_fp);
    }

    /* 2) CSPRNG：ECDSA 的随机数质量直接决定私钥安全，这里用 entropy + ctr_drbg，不用 rand()。 */
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)"oneye-prov", 10) != 0) {
        ESP_LOGE(TAG, "CSPRNG 初始化失败 —— 不产出自证串（弱随机签出的 ECDSA 会泄露私钥）");
        goto done;
    }
    drbg_ok = true;
    if (mbedtls_ctr_drbg_random(&drbg, nonce, sizeof(nonce)) != 0) {
        goto done;
    }
    hex_lower(nonce, sizeof(nonce), nonce_hex);

    /* 3) 规范化原文 + 摘要。 */
    int mlen = build_message(node_id, mac, cert_fp, nonce_hex, msg, sizeof(msg));
    if (mlen < 0) {
        ESP_LOGE(TAG, "规范化原文超出缓冲（字段异常长？）");
        goto done;
    }
    if (mbedtls_sha256((const unsigned char *)msg, (size_t)mlen, digest, 0) != 0) {
        goto done;
    }

    /* 4) 私钥：解析（3.x 的 pk_parse_key 需要 RNG）+ **与证书配对校验**。
     *    不校验配对的话，会产出一行"格式正确但永远验不过"的自证串 —— 那是最难查的假装成功。 */
    if (mbedtls_pk_parse_key(&pk, (const unsigned char *)key_pem, strlen(key_pem) + 1,
                             NULL, 0, mbedtls_ctr_drbg_random, &drbg) != 0) {
        ESP_LOGE(TAG, "私钥解析失败 —— 不产出自证串");
        goto done;
    }
    pk_ok = true;
    if (mbedtls_pk_check_pair(&crt.pk, &pk, mbedtls_ctr_drbg_random, &drbg) != 0) {
        ESP_LOGE(TAG, "私钥与证书**不配对** —— 分区里的身份是错的，拒绝产出自证串");
        goto done;
    }

    /* 5) 签名（DER）。 */
    if (mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                        sig, sizeof(sig), &sig_len,
                        mbedtls_ctr_drbg_random, &drbg) != 0) {
        ESP_LOGE(TAG, "签名失败");
        goto done;
    }
    if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &b64_len, sig, sig_len) != 0) {
        goto done;
    }
    b64[b64_len] = '\0';

    if (snprintf(out, cap, "ONEYE-PROV1 node_id=%s mac=%s cert_fp=%s nonce=%s sig=%s",
                 node_id, mac ? mac : "", cert_fp, nonce_hex, b64) >= (int)cap) {
        ESP_LOGE(TAG, "自证串超出缓冲 %u", (unsigned)cap);
        goto done;
    }
    rc = ESP_OK;

done:
    if (pk_ok)  { mbedtls_pk_free(&pk); }
    if (crt_ok) { mbedtls_x509_crt_free(&crt); }
    if (drbg_ok) { mbedtls_ctr_drbg_free(&drbg); }
    mbedtls_entropy_free(&entropy);
    return rc;
}
