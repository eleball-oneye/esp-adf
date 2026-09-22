/*
 * 产测自证（Provisional attestation）—— 见 backend/contracts/domain/设备凭据分区与产测自证契约.md §6
 *
 * 为什么要它：伙伴产线的 flash 布局由伙伴决定，**凭证分区在哪个偏移我们不确定**，所以"我们回读
 * 一段 flash 再和镜像比对"这条路走不通。而且回读只能证明"flash 里有这份字节"，**不能证明"这台
 * 设备真的能用这个身份上线"**（证书与私钥不配对、私钥读不出来、代码取错了来源，回读都看不出来）。
 *
 * 做法：设备用**分区里那把私钥**对一段规范化文本签名，产线上位机用**该设备的证书**验签。
 * 于是"这台机器 = 台账里那一行"就成了密码学证明，与偏移、与谁编译、与谁烧录都无关。
 *
 * 边界（诚实标注）：自证解决"身份对不对"，**不解决克隆** —— 掌握整片 flash 的人可以直接复制
 * 私钥。抗克隆只有 eFuse 那条路（见契约 §7 红线 5）。
 */

#ifndef PROV_ATTEST_H
#define PROV_ATTEST_H

#include <stddef.h>
#include "esp_err.h"

/* 一行自证串的上限（实测一行约 400 B：证书指纹 64 + 签名 base64 ~96 + 其余字段）。 */
#define PROV_ATTEST_LINE_MAX 768

/*
 * 生成一行自证串：
 *   ONEYE-PROV1 node_id=<..> mac=<..> cert_fp=<..> nonce=<..> sig=<base64>
 *
 * 被签名的规范化原文（逐字节，分隔符 LF，结尾一个 LF）：
 *   "ONEYE-PROV1\nnode_id=<node_id>\nmac=<mac>\ncert_fp=<cert_fp>\nnonce=<nonce>\n"
 *
 * 参数：
 *   node_id / mac        —— 分区里的身份（mac 可为 NULL 或空串）
 *   cert_pem / key_pem   —— 分区里的设备证书与私钥（PEM）
 *   out / cap            —— 输出缓冲（建议 PROV_ATTEST_LINE_MAX）
 *
 * 失败姿态：证书解析不了、私钥与证书不配对、签名失败 ⇒ 返回非 ESP_OK，**不产出半截行**
 *          （半截行会被产线误当成"验签失败"，把工具缺陷读成设备缺陷）。
 */
esp_err_t prov_attest_line(const char *node_id, const char *mac,
                           const char *cert_pem, const char *key_pem,
                           char *out, size_t cap);

#endif /* PROV_ATTEST_H */
