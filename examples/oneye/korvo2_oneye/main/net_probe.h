/*
 * 板级网络自检（`net_probe`）——**取证用**，不改变任何连接行为。
 *
 * 为什么需要它（2026-09-20 实测踩到的坑）：
 *   设备侧只报 `[oneye][base][W] connect <ip>:<port> (mqtt-tls) failed`，**不带 errno**；
 *   而云端 tcpdump 在 1883/18884 上**一个 SYN 都没看到**。此时"没路由 / 网关 ARP 不通 /
 *   上行丢包"三种根因在日志上完全同形 —— 只能靠猜。本探针把这三者用 errno 区分开：
 *     ENETUNREACH   ⇒ 没有默认路由（DHCP 没给网关）
 *     EHOSTUNREACH  ⇒ 网关 ARP/链路层不通（同网段就有问题）
 *     ETIMEDOUT     ⇒ SYN 发出去了但没人应答（上行被丢，或对端不可达）
 *     0             ⇒ 通了（随后立即关闭）
 *   同时打印 IP/掩码/网关/DNS，便于一眼看出 DHCP 是否只给了一半。
 *
 * 口径：只读、只做 connect 后立刻 close，不发任何应用层字节；三个目标最坏 ~9 s 后返回。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 打印 IP/掩码/网关/DNS，并对以下目标各做一次带 errno 的 TCP 连通性探测：
 *   ① 网关:80（同网段控制组）② <cloud_host>:<cloud_port>（真实目标）③ 223.5.5.5:53（公网出口） */
void net_probe_report(const char *cloud_host, unsigned cloud_port);

#ifdef __cplusplus
}
#endif
