/*
 * 启动期授时（SNTP）——解决的问题：**设备侧 TLS 严格校验必须先有时间**。
 *
 * 实测（2026-09-20，Korvo-2）：板子无 RTC，首次上线时系统时间是 1970；而服务端证书
 * notBefore 落在 2026 ⇒ IDF 的 mbedtls 已编译 `MBEDTLS_HAVE_TIME_DATE`，严格校验必然
 * 报 `BADCERT_FUTURE`。此前只能靠 `CONFIG_ONEYE_FW_TLS_INSECURE=y` 绕开（等于不校验证书）。
 * 本模块在联网后、SDK 建链前向 NTP 取一次时间，使"严格校验"成为可能。
 *
 * 口径：最长等待 SNTP_WAIT_MS；失败**不阻塞**上云（只打印警告并继续），但此时若
 * `CONFIG_ONEYE_FW_TLS_INSECURE=n`，TLS 会因证书时间校验失败而连不上 —— 这是**故意**的
 * 可见失败，而不是静默降级（宁可见地失败，也不要静默失去服务端身份校验）。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 尝试经 SNTP 取时间。返回 true = 已同步（此后 time() 为真实 UTC）。
 * 失败返回 false（已打印警告；调用方可据此决定是否仍尝试上云）。 */
bool sntp_boot_sync(void);

#ifdef __cplusplus
}
#endif
