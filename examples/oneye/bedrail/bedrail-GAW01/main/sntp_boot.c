#include "sntp_boot.h"

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

static const char *TAG = "sntp_boot";

/* 两个服务器：国内优先，公网兜底。都不可达即视为失败（不阻塞上云）。 */
#define SNTP_SERVER_1 "ntp.aliyun.com"
#define SNTP_SERVER_2 "pool.ntp.org"
#define SNTP_WAIT_MS  10000

bool sntp_boot_sync(void)
{
    time_t now = 0;
    struct tm tm_utc;

    /* 本地时区（中国标准时间，UTC+8）：仅影响日志可读性，不影响 TLS 校验（校验用 UTC） */
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER_1);
    cfg.start = true;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "[sntp] 初始化失败（服务器 %s）", SNTP_SERVER_1);
        return false;
    }
    ESP_LOGW(TAG, "[sntp] 正在向 %s 取时间（最多 %d ms）…", SNTP_SERVER_1, SNTP_WAIT_MS);

    esp_err_t rc = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_WAIT_MS));
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "[sntp] 首次同步失败（rc=0x%x），改用 %s 重试", rc, SNTP_SERVER_2);
        esp_netif_sntp_deinit();
        esp_sntp_config_t cfg2 = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER_2);
        cfg2.start = true;
        if (esp_netif_sntp_init(&cfg2) == ESP_OK) {
            rc = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_WAIT_MS));
        }
    }

    now = time(NULL);
    gmtime_r(&now, &tm_utc);
    if (rc == ESP_OK && now > 1700000000) { /* 2023-11 之后才算"真时间"（防 1970 假成功） */
        ESP_LOGW(TAG, "[sntp] 已同步：UTC=%04d-%02d-%02d %02d:%02d:%02d（epoch=%lld）",
                 tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                 tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, (long long)now);
        return true;
    }

    ESP_LOGE(TAG, "[sntp] 未能取到有效时间（epoch=%lld）⇒ 严格 TLS 校验将失败；"
                  "若本机为产测环境可临时置 CONFIG_ONEYE_FW_TLS_INSECURE=y，生产**禁止**",
             (long long)now);
    return false;
}
