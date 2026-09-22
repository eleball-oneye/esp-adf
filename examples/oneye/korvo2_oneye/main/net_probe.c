#include "net_probe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "net_probe";

/* 非阻塞 connect + select，把"连不上"的等待时间钉死在 timeout_ms 上。
 * 返回值：0 = 连上；-1 = 失败（*err_out 为 errno）；-2 = 超时（errno 可能是 EINPROGRESS）；
 *         -3 = 域名解析失败（*err_out 为 EINVAL）。 */
static int probe_tcp(const char *host, unsigned port, int timeout_ms, int *err_out, int *ms_out)
{
    struct sockaddr_in dst;
    struct timeval tv;
    fd_set wset;
    int fd, rc, err = 0;
    socklen_t elen = sizeof(err);
    int64_t t0 = esp_timer_get_time();

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) {
        /* ⚠️ 这段是 2026-09-21 补的，起因是一个**会说谎的诊断**：此前的实现只认 IP 字面量
         * （inet_pton 失败即 EINVAL ⇒ 打印"失败"）。设备云端端点从 IP 改成域名
         * （mqtt.oneye.me）之后，探针就在串口里报 `cloud endpoint mqtt.oneye.me:18885 ->
         * rc=-1 errno=22(Invalid argument) 0ms 失败`，**而同一次启动的 `link up` 是成功的** ——
         * 也就是说，诊断工具在链路完全正常时给出了"失败"，这比没有诊断更容易把人带偏。
         * 现在补一次 DNS 解析：解析不到才判失败，且用独立的 rc=-3 与"连不上"区分。
         * 引用型实现注意：这里的解析是**同步**的，只在启动探针路径上跑一次，不在数据路径上。 */
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int gai = getaddrinfo(host, NULL, &hints, &res);
        if (gai != 0 || res == NULL) {
            *err_out = EINVAL;
            *ms_out = 0;
            return -3;
        }
        dst.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        *err_out = errno;
        *ms_out = 0;
        return -1;
    }
    /* 设成非阻塞，避免 lwip 默认 connect 的长时间阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    rc = connect(fd, (struct sockaddr *)&dst, sizeof(dst));
    if (rc == 0) {
        close(fd);
        *err_out = 0;
        *ms_out = (int)((esp_timer_get_time() - t0) / 1000);
        return 0;
    }
    if (errno != EINPROGRESS && errno != EALREADY) {
        *err_out = errno;
        close(fd);
        *ms_out = (int)((esp_timer_get_time() - t0) / 1000);
        return -1;
    }

    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    rc = select(fd + 1, NULL, &wset, NULL, &tv);
    if (rc <= 0) {
        *err_out = (rc == 0) ? ETIMEDOUT : errno;
        close(fd);
        *ms_out = (int)((esp_timer_get_time() - t0) / 1000);
        return (rc == 0) ? -2 : -1;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0) {
        err = errno;
    }
    close(fd);
    *err_out = err;
    *ms_out = (int)((esp_timer_get_time() - t0) / 1000);
    return (err == 0) ? 0 : -1;
}

static void probe_and_log(const char *label, const char *host, unsigned port)
{
    int err = 0, ms = 0;
    int rc = probe_tcp(host, port, 3000, &err, &ms);
    const char *verdict;

    if (rc == 0) {
        verdict = "OK（TCP 已建立）";
    } else if (rc == -3) {
        verdict = "域名解析失败（DNS 不通或名字写错）";
    } else if (err == ENETUNREACH) {
        verdict = "无路由（默认网关缺失）";
    } else if (err == EHOSTUNREACH) {
        verdict = "网关/链路层不可达（ARP 不通）";
    } else if (err == ETIMEDOUT) {
        verdict = "SYN 无应答（上行被丢或对端不可达）";
    } else {
        verdict = "失败";
    }
    ESP_LOGW(TAG, "[net-probe] %-22s %s:%-5u -> rc=%d errno=%d(%s) %dms  %s",
             label, host, port, rc, err, err == 0 ? "-" : strerror(err), ms, verdict);
}

void net_probe_report(const char *cloud_host, unsigned cloud_port)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    esp_netif_dns_info_t dns;
    char gw[16] = "-", mask[16] = "-", addr[16] = "-", dns_s[16] = "-";

    if (netif == NULL) {
        ESP_LOGW(TAG, "[net-probe] STA netif 未就绪：跳过（未联网？）");
        return;
    }
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(addr, sizeof(addr), IPSTR, IP2STR(&ip.ip));
        snprintf(mask, sizeof(mask), IPSTR, IP2STR(&ip.netmask));
        snprintf(gw, sizeof(gw), IPSTR, IP2STR(&ip.gw));
    }
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        snprintf(dns_s, sizeof(dns_s), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }

    ESP_LOGW(TAG, "[net-probe] ip=%s mask=%s gw=%s dns=%s", addr, mask, gw, dns_s);

    /* ① 同网段控制组：网关的 80 端口（网关不一定开 80 ⇒ 只作参考，ETIMEDOUT/REFUSED 都算"链路可达"） */
    if (strcmp(gw, "-") != 0) {
        probe_and_log("gateway(LAN control)", gw, 80);
    }
    /* ② 真实目标 */
    if (cloud_host != NULL && cloud_host[0] != '\0') {
        probe_and_log("cloud endpoint", cloud_host, cloud_port);
    }
    /* ③ 公网出口（AliDNS；UDP/TCP 53 通常可达） */
    probe_and_log("public egress", "223.5.5.5", 53);
}
