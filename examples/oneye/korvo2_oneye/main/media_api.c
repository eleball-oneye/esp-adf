/*
 * media_api.c —— 媒体列表/流式播放（含 Range）与本地动作触发（口径见 media_api.h）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "media_api.h"
#include "aec_capture.h"
#include "player.h"

static const char *TAG = "media_api";

/* 注：注释里避免出现 "/media/" 紧跟星号的写法（会被 -Werror=comment 判为注释内嵌套注释） */

#define MEDIA_CHUNK        4096
#define MEDIA_LIST_MAX     64
#define MEDIA_URI_MAX      200
#define MEDIA_PATH_MAX     512      /* 足够容纳 root + 子目录 + FATFS 长文件名（255），避免截断告警 */
#define MEDIA_SINGLE_SEND_MAX (256 * 1024)   /* ≤ 该长度一次发送（带 Content-Length）；更大者分块流式 */

typedef struct {
    const char *alias;   /* URL 别名 */
    const char *root;    /* 文件系统根 */
} media_root_t;

static const media_root_t s_roots[] = {
    { "sdcard", "/sdcard" },
    { "spiffs", "/spiffs" },
};

/* ------------------------------------------------------------------ 工具 */

static const char *content_type_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return "application/octet-stream";
    }
    if (strcasecmp(dot, ".wav") == 0)   return "audio/wav";
    if (strcasecmp(dot, ".mp3") == 0)   return "audio/mpeg";
    if (strcasecmp(dot, ".aac") == 0)   return "audio/aac";
    if (strcasecmp(dot, ".jpg") == 0)   return "image/jpeg";
    if (strcasecmp(dot, ".jpeg") == 0)  return "image/jpeg";
    if (strcasecmp(dot, ".png") == 0)   return "image/png";
    if (strcasecmp(dot, ".mjpeg") == 0) return "video/x-motion-jpeg";
    if (strcasecmp(dot, ".avi") == 0)   return "video/x-msvideo";
    if (strcasecmp(dot, ".mp4") == 0)   return "video/mp4";
    if (strcasecmp(dot, ".txt") == 0)   return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static const char *kind_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return "other";
    }
    if (strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".mp3") == 0 ||
        strcasecmp(dot, ".aac") == 0) {
        return "audio";
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 ||
        strcasecmp(dot, ".png") == 0) {
        return "image";
    }
    if (strcasecmp(dot, ".avi") == 0 || strcasecmp(dot, ".mjpeg") == 0 ||
        strcasecmp(dot, ".mp4") == 0) {
        return "video";
    }
    return "other";
}

static bool is_media(const char *name)
{
    const char *k = kind_of(name);
    return strcmp(k, "other") != 0;
}

/* 把 URL 路径里的 <alias>/<rel> 解析成真实文件系统路径；拒绝 .. 与绝对路径 */
static bool resolve_media_path(const char *uri, char *out, size_t cap)
{
    static const char prefix[] = "/media/";
    if (strncmp(uri, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *rest = uri + sizeof(prefix) - 1;
    const char *slash = strchr(rest, '/');
    if (slash == NULL || slash == rest) {
        return false;
    }
    char alias[16];
    size_t alias_len = (size_t)(slash - rest);
    if (alias_len >= sizeof(alias)) {
        return false;
    }
    memcpy(alias, rest, alias_len);
    alias[alias_len] = '\0';

    const char *rel = slash + 1;
    if (*rel == '\0' || strstr(rel, "..") != NULL || rel[0] == '/') {
        return false;                             /* 目录穿越防护 */
    }
    for (size_t i = 0; i < sizeof(s_roots) / sizeof(s_roots[0]); i++) {
        if (strcasecmp(alias, s_roots[i].alias) == 0) {
            snprintf(out, cap, "%s/%s", s_roots[i].root, rel);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ 列表 */

static void add_file(cJSON *arr, const char *alias, const char *root, const char *name)
{
    char full[MEDIA_PATH_MAX];
    int n = snprintf(full, sizeof(full), "%s/%s", root, name);
    if (n <= 0 || (size_t)n >= sizeof(full)) {
        return;                                   /* 路径过长：跳过（不截断） */
    }
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        return;
    }
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return;
    }
    cJSON_AddStringToObject(o, "alias", alias);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddNumberToObject(o, "size", (double)st.st_size);
    cJSON_AddNumberToObject(o, "mtime", (double)st.st_mtime);
    cJSON_AddStringToObject(o, "kind", kind_of(name));
    cJSON *item = cJSON_AddObjectToObject(o, "item");
    char url[MEDIA_URI_MAX];
    snprintf(url, sizeof(url), "media/%s/%s", alias, name);
    cJSON_AddStringToObject(item, "url", url);
    /* 板上回放（本轮）：仅 SD 卡（FATFS）上的 wav/mp3 —— SPIFFS 录不上板播，只能网页播放/下载 */
    const char *dot = strrchr(name, '.');
    bool audio = (dot != NULL) &&
                 (strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".mp3") == 0);
    cJSON_AddBoolToObject(o, "playable_on_board", audio && strcmp(alias, "sdcard") == 0);
    char dev_path[PLAYER_PATH_MAX];
    snprintf(dev_path, sizeof(dev_path), "/%s/%s", alias, name);
    cJSON_AddStringToObject(o, "device_path", dev_path);
    cJSON_AddItemToArray(arr, o);
}

static int scan_dir(cJSON *arr, const char *alias, const char *root, const char *sub, int limit)
{
    char dirpath[MEDIA_PATH_MAX];
    if (sub && sub[0]) {
        snprintf(dirpath, sizeof(dirpath), "%s/%s", root, sub);
    } else {
        snprintf(dirpath, sizeof(dirpath), "%s", root);
    }
    DIR *dir = opendir(dirpath);
    if (dir == NULL) {
        return 0;
    }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && n < limit) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char rel[MEDIA_PATH_MAX];
        if (sub && sub[0]) {
            int m = snprintf(rel, sizeof(rel), "%s/%s", sub, de->d_name);
            if (m <= 0 || (size_t)m >= sizeof(rel)) {
                continue;
            }
        } else {
            int m = snprintf(rel, sizeof(rel), "%s", de->d_name);
            if (m <= 0 || (size_t)m >= sizeof(rel)) {
                continue;
            }
        }
        char full[MEDIA_PATH_MAX];
        int k = snprintf(full, sizeof(full), "%s/%s", dirpath, de->d_name);
        if (k <= 0 || (size_t)k >= sizeof(full)) {
            continue;
        }
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (!is_media(rel)) {
            continue;
        }
        add_file(arr, alias, root, rel);
        n++;
    }
    closedir(dir);
    return n;
}

static esp_err_t h_media_list(httpd_req_t *req)
{
    (void)req;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    aec_capture_status_t st;
    aec_capture_get_status(&st);

    cJSON *files = cJSON_AddArrayToObject(root, "files");
    int total = 0;
    for (size_t i = 0; i < sizeof(s_roots) / sizeof(s_roots[0]); i++) {
        total += scan_dir(files, s_roots[i].alias, s_roots[i].root, "rec", MEDIA_LIST_MAX);
        total += scan_dir(files, s_roots[i].alias, s_roots[i].root, NULL, MEDIA_LIST_MAX);
    }
    cJSON_AddNumberToObject(root, "count", total);
    cJSON_AddStringToObject(root, "rec_root", aec_capture_root());
    cJSON_AddBoolToObject(root, "aec_enabled", st.enabled);
    cJSON_AddBoolToObject(root, "aec_recording", st.recording);

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (txt == NULL) {
        return httpd_resp_send_500(req);
    }
    esp_err_t rc = httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (rc == ESP_OK) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        rc = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    }
    free(txt);
    return rc;
}

/* ------------------------------------------------------------------ 文件（含 Range） */

static esp_err_t h_media_get(httpd_req_t *req)
{
    char path[MEDIA_PATH_MAX];
    if (!resolve_media_path(req->uri, path, sizeof(path))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad media path");
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "文件不存在：%s", path);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
    }
    int64_t total = (int64_t)st.st_size;
    int64_t start = 0;
    int64_t end = total > 0 ? total - 1 : 0;

    char range[64];
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK) {
        /* 仅支持单区间：bytes=start-end | bytes=start- | bytes=-suffix */
        const char *p = strstr(range, "bytes=");
        if (p != NULL) {
            p += 6;
            long long a = -1, b = -1;
            if (sscanf(p, "%lld-%lld", &a, &b) >= 1) {
                if (a < 0 && b >= 0) {              /* 末尾 N 字节 */
                    start = total - b;
                    end = total - 1;
                } else if (a >= 0) {
                    start = a;
                    end = (b >= 0 && b < total) ? b : total - 1;
                }
                if (start < 0) {
                    start = 0;
                }
                if (start >= total) {
                    /* 该 IDF 版本的 esp_http_server 无 416 枚举值 ⇒ 手工置状态码 */
                    httpd_resp_set_status(req, "416 Requested Range Not Satisfiable");
                    httpd_resp_set_hdr(req, "Content-Range", "bytes */0");
                    return httpd_resp_send(req, "", 0);
                }
            }
        }
    }
    int64_t len = end - start + 1;

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    }
    if (fseek(fp, (long)start, SEEK_SET) != 0) {
        fclose(fp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "seek failed");
    }

    char hdr[96];
    snprintf(hdr, sizeof(hdr), "%lld", (long long)len);
    httpd_resp_set_type(req, content_type_of(path));
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    bool partial = (start > 0 || len != total);
    if (partial) {
        char cr[96];
        snprintf(cr, sizeof(cr), "bytes %lld-%lld/%lld", (long long)start, (long long)end, (long long)total);
        httpd_resp_set_status(req, "206 Partial Content");
        httpd_resp_set_hdr(req, "Content-Range", cr);
    }

    /* 小文件（AEC WAV 等）一次发送 ⇒ 带准确 Content-Length（浏览器拖动进度最稳）；
     * 大文件（SD 卡录像）分块流式，避免一次性占用大块堆内存。 */
    if (len <= MEDIA_SINGLE_SEND_MAX) {
        char *buf = malloc((size_t)len);
        if (buf != NULL) {
            size_t got = fread(buf, 1, (size_t)len, fp);
            fclose(fp);
            esp_err_t rc = httpd_resp_send(req, buf, got);
            free(buf);
            ESP_LOGI(TAG, "媒体发送：%s（%lld/%lld 字节，Content-Length）",
                     path, (long long)got, (long long)total);
            return rc;
        }
        /* 内存不足 → 回落到分块发送 */
        (void)fseek(fp, (long)start, SEEK_SET);
    }

    char *buf = malloc(MEDIA_CHUNK);
    if (buf == NULL) {
        fclose(fp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    int64_t remain = len;
    esp_err_t rc = ESP_OK;
    while (remain > 0) {
        size_t want = (size_t)(remain > MEDIA_CHUNK ? MEDIA_CHUNK : remain);
        size_t got = fread(buf, 1, want, fp);
        if (got == 0) {
            break;
        }
        rc = httpd_resp_send_chunk(req, buf, got);
        if (rc != ESP_OK) {
            break;
        }
        remain -= (int64_t)got;
    }
    free(buf);
    fclose(fp);
    if (rc == ESP_OK) {
        rc = httpd_resp_send_chunk(req, NULL, 0);   /* 结束分块响应 */
    }
    ESP_LOGI(TAG, "媒体发送：%s（%lld/%lld 字节，分块）", path, (long long)len, (long long)total);
    return rc;
}

/* ------------------------------------------------------------------ 动作（写操作：仅台面验证） */

static esp_err_t h_action(httpd_req_t *req)
{
    char body[320];
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    body[got] = '\0';

    cJSON *doc = cJSON_Parse(body);
    if (doc == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    const cJSON *op = cJSON_GetObjectItem(doc, "op");
    const cJSON *dur = cJSON_GetObjectItem(doc, "duration_s");
    const cJSON *path = cJSON_GetObjectItem(doc, "path");
    const cJSON *vol = cJSON_GetObjectItem(doc, "volume");
    esp_err_t rc = ESP_OK;
    const char *msg = "ok";
    char file[64] = "";
    const char *op_str = cJSON_IsString(op) ? op->valuestring : "";

    if (strcmp(op_str, "aec_start") == 0) {
        uint32_t seconds = (cJSON_IsNumber(dur) && dur->valuedouble > 0)
                               ? (uint32_t)dur->valuedouble : 0;
        rc = aec_capture_start(seconds, file, sizeof(file));
        if (rc != ESP_OK) {
            msg = (rc == ESP_ERR_INVALID_STATE) ? "already recording"
                                                : (rc == ESP_ERR_NOT_SUPPORTED ? "aec disabled" : "start failed");
        }
    } else if (strcmp(op_str, "aec_stop") == 0) {
        rc = aec_capture_stop();
        msg = (rc == ESP_OK) ? "stopped" : "stop failed";
    } else if (strcmp(op_str, "play") == 0) {
        if (!cJSON_IsString(path)) {
            rc = ESP_ERR_INVALID_ARG;
            msg = "missing path";
        } else {
            rc = player_play(path->valuestring);
            if (rc != ESP_OK) {
                msg = (rc == ESP_ERR_NOT_SUPPORTED) ? "unsupported codec (wav/mp3 only)"
                      : (rc == ESP_ERR_INVALID_ARG ? "invalid path (must be /sdcard/...)"
                                                   : "play failed");
            } else {
                msg = "playing";
                snprintf(file, sizeof(file), "%s", path->valuestring);
            }
        }
    } else if (strcmp(op_str, "stop") == 0 || strcmp(op_str, "play_stop") == 0) {
        rc = player_stop();
        msg = (rc == ESP_OK) ? "stopped" : "stop failed";
    } else if (strcmp(op_str, "set_volume") == 0) {
        if (!cJSON_IsNumber(vol)) {
            rc = ESP_ERR_INVALID_ARG;
            msg = "missing volume";
        } else {
            rc = player_set_volume((int)vol->valuedouble);
            msg = (rc == ESP_OK) ? "volume set" : "volume failed";
        }
    } else {
        msg = "unknown op";
        rc = ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(doc);

    aec_capture_status_t st;
    aec_capture_get_status(&st);
    player_status_t ps;
    player_get_status(&ps);
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(resp, "ok", rc == ESP_OK);
    cJSON_AddStringToObject(resp, "msg", msg);
    cJSON_AddStringToObject(resp, "file", file[0] ? file : (ps.path[0] ? ps.path : st.last_file));
    cJSON_AddBoolToObject(resp, "recording", st.recording);
    cJSON_AddNumberToObject(resp, "last_bytes", (double)st.last_bytes);
    cJSON_AddBoolToObject(resp, "playing", ps.playing);
    cJSON_AddNumberToObject(resp, "volume", ps.volume);
    char *txt = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (txt == NULL) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t send_rc = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    free(txt);
    return send_rc;
}

/* ------------------------------------------------------------------ 注册 */

esp_err_t media_api_register(httpd_handle_t httpd)
{
    if (httpd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/media/list", .method = HTTP_GET,  .handler = h_media_list },
        { .uri = "/media/*",    .method = HTTP_GET,  .handler = h_media_get },
        { .uri = "/api/action", .method = HTTP_POST, .handler = h_action },
    };
    esp_err_t rc = ESP_OK;
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t one = httpd_register_uri_handler(httpd, &uris[i]);
        if (one != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败：%s", uris[i].uri, esp_err_to_name(one));
            rc = one;
        }
    }
    ESP_LOGI(TAG, "媒体/动作 API 已注册：/media/list、/media/<alias>/<path>（支持 Range）、POST /api/action");
    return rc;
}
