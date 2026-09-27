/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 局域网 Web 管理页（系统状态 + 文件管理）
 *
 * 设备拿到 IP 后自动在 80 端口起 HTTP 服务、断开自动停。页面本体是
 * assets/web/index.html（构建期用 EMBED_FILES 打进固件，不依赖文件系统、也不会被误删），
 * 数据走 /api 前缀：
 *   GET  /                              页面
 *   GET  /api/status                    系统信息 / Wi-Fi / 蓝牙 / 存储（JSON）
 *   GET  /api/logs                      最近日志（纯文本，直接取日志环）
 *   GET  /api/files?path=               目录列表 + 该存储容量（JSON，目录在前按名排序）
 *   GET  /api/job                       后台任务进度（复制 / 剪切 / 删除）
 *   GET  /api/download?path=[&dl=1]     分块下发文件（不带 dl 时给浏览器内联显示）
 *   POST /api/upload?dir=&name=         请求体就是文件内容（边收边写，不占内存）
 *   POST /api/mkdir?path=
 *   POST /api/rename?path=&name=
 *   POST /api/delete                    请求体：一行一个路径 → 提交后台任务，立刻返回
 *   POST /api/paste?to=&mode=copy|cut   请求体：一行一个来源路径 → 提交后台任务
 * 另外还有 /s 前缀：脚本运行时注册的网页路由（见 svc_web_set_custom_handler）。
 * 精确接口先注册、脚本区最后注册，按注册顺序匹配，互不干扰。
 *
 * 起停与后台任务都放在一个独立小任务（svc.web）里做：配网的 httpd 可能还占着 80 端口
 * （STA 刚连上、AP 还没停），启动失败要退避重试；事件总线回调里只投一条命令。复制 / 剪切 /
 * 删除可能涉及成百上千个文件，放在 handler 里会把整个服务堵死（esp_http_server 只有一个
 * 任务），所以改成"提交任务 + 界面轮询 /api/job"。
 *
 * 所有 handler 都跑在 esp_http_server 自己的任务里，禁止碰 LVGL；上传 / 下载必须流式
 * （整块读写有 1 MB 上限，见 svc_storage 的 write_open / chunk / close 与 stream_read）。
 * 大文件上下传期间其它请求（含状态轮询）会排队等，所以关掉了 lru_purge（别把排队的连接
 * 踢掉），页面侧也做了防堆积。
 *
 * 不做认证：只在可信的局域网里用。
 */

#include "svc_common.h"
#include "svc_web.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "svc.web";

/* 页面资源：CMakeLists 的 EMBED_FILES 生成（符号名只取文件名，
 * 见 IDF 的 data_file_embed_asm.cmake：_binary_index_html_start/_end） */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

#define WEB_PATH_MAX     320      /* 请求里的路径（含 /sdcard 前缀） */
#define WEB_NAME_MAX     200      /* 文件名 */
#define WEB_QUERY_MAX    512
#define WEB_JSON_MAX     1536     /* 状态 JSON */
#define WEB_ITEM_MAX     1024     /* 列表里单条的 JSON（名字要转义，留足） */
#define WEB_LIST_MAX     512      /* 一次列出的目录项上限 */
#define WEB_UP_CHUNK     8192     /* 上传 / 读文件的搬运块 */
#define WEB_UP_TIMEOUT   20       /* 连续这么多次收不到数据就放弃（每次 5 s） */
#define WEB_TASK_STACK   4096
#define WEB_TASK_PRIO    3

#define WEB_CMD_START    1
#define WEB_CMD_STOP     2
#define WEB_CMD_JOB      3        /* 跑后台任务（复制 / 剪切 / 删除） */

#define WEB_JOB_MAX_PATHS  512    /* 一次任务最多几个来源 */

#define WEB_CUSTOM_PATH_MAX  128  /* /s 脚本区：去掉前缀后的路径 */
#define WEB_CUSTOM_QUERY_MAX 256
#define WEB_CUSTOM_BODY_MAX  1024

/* HTTP 服务句柄与命令队列：服务在文件末尾的 svc.web 任务里起停，后台任务也由它跑 */
static httpd_handle_t s_server = NULL;
static QueueHandle_t s_cmds = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_held = false;    /* 相机在用内部内存，暂不提供网页 */

/* 各 handler 的拼串 / 转义缓冲。handler 都在 esp_http_server 的同一个任务里顺序执行，
 * 一块共享暂存就够。放 PSRAM：这些加起来十几 KB，摊在内部 RAM 会挤掉别的任务要用的
 * 连续块（内部 RAM 很紧）。服务起来时分配、停时释放。 */
typedef struct {
    char status_json[WEB_JSON_MAX];             /* /api/status */
    char log_ring[2048 + 1];                    /* /api/logs */
    char files_esc_path[WEB_PATH_MAX * 2];      /* /api/files 头部 */
    char files_head[WEB_PATH_MAX * 3 + 256];
    char files_item[WEB_ITEM_MAX];
    char job_json[WEB_NAME_MAX * 4 + 256];      /* /api/job */
    char job_esc_current[WEB_NAME_MAX * 2];
    char job_esc_failed[WEB_NAME_MAX * 2];
    char body_rbuf[1024];                       /* 按行读请求体 */
    char body_line[WEB_PATH_MAX];
    char custom_path[WEB_CUSTOM_PATH_MAX];      /* /s 脚本区 */
    char custom_query[WEB_CUSTOM_QUERY_MAX];
    char custom_body[WEB_CUSTOM_BODY_MAX];
} web_scratch_t;

static web_scratch_t *s_scratch = NULL;

/* ------------------------------- 小工具 ------------------------------- */

/* 按 JSON 字符串转义写进 dst，返回写入长度（UTF-8 原样透传，只转义必须转义的字节） */
static size_t json_escape_into(char *dst, size_t len, const char *src)
{
    size_t o = 0;

    for (const char *p = src; *p != '\0' && o + 1 < len; p++) {
        const unsigned char c = (unsigned char)*p;

        if (c == '"' || c == '\\') {
            if (o + 2 >= len) break;
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            if (o + 6 >= len) break;
            o += (size_t)snprintf(dst + o, len - o, "\\u%04x", c);
        } else {
            dst[o++] = (char)c;
        }
    }

    dst[o] = '\0';
    return o;
}

/* %XX 解码（'+' 当普通字符：路径里的 '+' 不该变成空格） */
static void url_decode(const char *in, char *out, size_t len)
{
    size_t o = 0;

    for (size_t i = 0; in[i] != '\0' && o + 1 < len; i++) {
        if (in[i] == '%' && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            const char hex[3] = { in[i + 1], in[i + 2], '\0' };
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[o++] = in[i];
        }
    }

    out[o] = '\0';
}

/* 取 query 参数（URL 解码后写进 out）；没有该参数返回 false */
static bool qparam(httpd_req_t *req, const char *key, char *out, size_t len)
{
    char query[WEB_QUERY_MAX];
    char raw[WEB_QUERY_MAX];

    if (out == NULL || len == 0) return false;
    out[0] = '\0';

    if (httpd_req_get_url_query_len(req) <= 0) return false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    if (httpd_query_key_value(query, key, raw, sizeof(raw)) != ESP_OK) return false;

    url_decode(raw, out, len);
    return true;
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* 统一结果：{"ok":true} / {"ok":false,"err":"..."} */
static esp_err_t send_result(httpd_req_t *req, bool ok, const char *err)
{
    char buf[320];

    if (ok) {
        snprintf(buf, sizeof(buf), "{\"ok\":true}");
    } else {
        char esc[200];
        json_escape_into(esc, sizeof(esc), (err != NULL) ? err : "failed");
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"err\":\"%s\"}", esc);
    }

    return send_json(req, buf);
}

/* 估个 Content-Type，让浏览器能内联显示文本 / 图片 / 音频 */
static const char *mime_of(const char *name)
{
    static const struct {
        const char *ext;
        const char *mime;
    } T[] = {
        { "txt",  "text/plain; charset=utf-8" },
        { "lua",  "text/plain; charset=utf-8" },
        { "md",   "text/plain; charset=utf-8" },
        { "log",  "text/plain; charset=utf-8" },
        { "csv",  "text/csv; charset=utf-8" },
        { "json", "application/json; charset=utf-8" },
        { "html", "text/html; charset=utf-8" },
        { "htm",  "text/html; charset=utf-8" },
        { "css",  "text/css; charset=utf-8" },
        { "js",   "text/javascript; charset=utf-8" },
        { "png",  "image/png" },
        { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "gif",  "image/gif" },
        { "bmp",  "image/bmp" },
        { "webp", "image/webp" },
        { "mp3",  "audio/mpeg" },
        { "wav",  "audio/wav" },
        { "mp4",  "video/mp4" },
    };

    const char *dot = strrchr(name, '.');
    if (dot == NULL) return "application/octet-stream";

    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        if (strcasecmp(dot + 1, T[i].ext) == 0) return T[i].mime;
    }
    return "application/octet-stream";
}

/* 路径必须在两个存储里（不做认证的页面，别让它碰到别的挂载点） */
static bool path_is_storage(const char *path)
{
    if (path == NULL || path[0] != '/') return false;

    static const char *const ROOTS[] = { "/sdcard", "/internal" };
    for (size_t i = 0; i < sizeof(ROOTS) / sizeof(ROOTS[0]); i++) {
        const size_t n = strlen(ROOTS[i]);
        if (strncmp(path, ROOTS[i], n) == 0 && (path[n] == '\0' || path[n] == '/')) return true;
    }
    return false;
}

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

/* 拆出主名与扩展名（"a.tar.gz" → "a.tar" + ".gz"） */
static void split_ext(const char *name, char *base, size_t base_len, char *ext, size_t ext_len)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name) {
        snprintf(base, base_len, "%s", name);
        ext[0] = '\0';
        return;
    }

    const size_t n = (size_t)(dot - name);
    if (n >= base_len) {
        snprintf(base, base_len, "%s", name);
        ext[0] = '\0';
        return;
    }

    memcpy(base, name, n);
    base[n] = '\0';
    snprintf(ext, ext_len, "%s", dot);
}

/* 目标重名就加 "(1)"（放在扩展名前面），和文件管理 App 的粘贴一致 */
static void unique_path(const char *dir, const char *name, char *out, size_t len)
{
    char base[WEB_NAME_MAX];
    char ext[64];
    size_t size = 0;

    snprintf(out, len, "%s/%s", dir, name);
    if (svc_storage_exists(out, &size) != ESP_OK) return;     /* 不重名，直接用 */

    split_ext(name, base, sizeof(base), ext, sizeof(ext));
    for (int i = 1; i <= 100; i++) {
        if (ext[0] != '\0') {
            snprintf(out, len, "%s/%s (%d)%s", dir, base, i, ext);
        } else {
            snprintf(out, len, "%s/%s (%d)", dir, base, i);
        }
        if (svc_storage_exists(out, &size) != ESP_OK) return;
    }
}

/* 逐行读请求体（delete / paste 都是"一行一个路径"）：不整块读进来，省内存也没有长度上限。
 * 单行超过 line 缓冲时整行作废并记一次失败 —— 宁可拒绝，也不能拿半截路径去删文件。 */
typedef bool (*line_cb_t)(const char *line, void *user);

static int foreach_body_line(httpd_req_t *req, line_cb_t cb, void *user,
                             char *failed, size_t failed_len)
{
    web_scratch_t *w = s_scratch;
    char *rbuf = w->body_rbuf;
    char *line = w->body_line;

    size_t line_len = 0;
    bool overflow = false;
    int done = 0;
    int timeouts = 0;
    size_t remain = (size_t)req->content_len;

    while (remain > 0) {
        const size_t want = (remain < sizeof(w->body_rbuf)) ? remain : sizeof(w->body_rbuf);
        const int rd = httpd_req_recv(req, rbuf, want);

        if (rd == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > WEB_UP_TIMEOUT) return -1;
            continue;
        }
        if (rd <= 0) return -1;

        timeouts = 0;
        remain -= (size_t)rd;

        for (int i = 0; i < rd; i++) {
            const char c = rbuf[i];

            if (c != '\n' && c != '\r') {
                if (line_len + 1 < sizeof(w->body_line)) {
                    line[line_len++] = c;
                } else {
                    overflow = true;
                }
                continue;
            }

            if (overflow) {
                if (failed != NULL && failed_len > 0) snprintf(failed, failed_len, "%s", "路径太长");
            } else if (line_len > 0) {
                line[line_len] = '\0';
                if (cb(line, user)) {
                    done++;
                } else if (failed != NULL && failed_len > 0) {
                    snprintf(failed, failed_len, "%s", path_basename(line));
                }
            }

            line_len = 0;
            overflow = false;
        }
    }

    /* 最后一行可能没有换行 */
    if (overflow) {
        if (failed != NULL && failed_len > 0) snprintf(failed, failed_len, "%s", "路径太长");
    } else if (line_len > 0) {
        line[line_len] = '\0';
        if (cb(line, user)) {
            done++;
        } else if (failed != NULL && failed_len > 0) {
            snprintf(failed, failed_len, "%s", path_basename(line));
        }
    }

    return done;
}

/* ------------------------------- 页面 / 状态 ------------------------------- */

static esp_err_t h_index(httpd_req_t *req)
{
    const size_t len = (size_t)(index_html_end - index_html_start);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

/* 浏览器总会来要 /favicon.ico：给个 204，别让它每次开页面都吃一个 404 */
static esp_err_t h_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* 对端蓝牙名字：连我们的那台设备可能出现在最近一次扫描结果里，查不到就用地址 */
static void bt_peer_name(const uint8_t *bda, char *out, size_t len)
{
    svc_bt_scan_result_t res[16];
    size_t n = 0;

    if (svc_bt_get_scan_results(res, sizeof(res) / sizeof(res[0]), &n) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            if (res[i].name[0] != '\0' && memcmp(res[i].bda, bda, 6) == 0) {
                snprintf(out, len, "%s", res[i].name);
                return;
            }
        }
    }

    out[0] = '\0';
}

static esp_err_t h_status(httpd_req_t *req)
{
    char *json = s_scratch->status_json;

    svc_sysinfo_t si;
    memset(&si, 0, sizeof(si));
    svc_sysinfo_get(&si);

    svc_net_status_t net;
    memset(&net, 0, sizeof(net));
    svc_net_get_status(&net);

    svc_bt_diag_t diag;
    memset(&diag, 0, sizeof(diag));
    svc_bt_get_diag(&diag);

    char peer[18] = "";
    char peer_name[SVC_BT_NAME_MAX] = "";
    uint8_t bda[6];
    const bool has_peer = svc_bt_get_peer(bda);
    if (has_peer) {
        snprintf(peer, sizeof(peer), "%02X:%02X:%02X:%02X:%02X:%02X",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        bt_peer_name(bda, peer_name, sizeof(peer_name));
    }

    svc_storage_info_t tf, in;
    memset(&tf, 0, sizeof(tf));
    memset(&in, 0, sizeof(in));
    const bool tf_ok = (svc_storage_get_info(SVC_STORAGE_TF_CARD, &tf) == ESP_OK);
    const bool in_ok = (svc_storage_get_info(SVC_STORAGE_INTERNAL_FLASH, &in) == ESP_OK);

    char esc_mac[24];
    char esc_peer[24];
    char esc_ssid[80];
    char esc_peer_name[SVC_BT_NAME_MAX * 2];
    json_escape_into(esc_mac, sizeof(esc_mac), si.mac);
    json_escape_into(esc_peer, sizeof(esc_peer), peer);
    json_escape_into(esc_ssid, sizeof(esc_ssid), net.wifi_ssid);
    json_escape_into(esc_peer_name, sizeof(esc_peer_name), peer_name);

    snprintf(json, sizeof(s_scratch->status_json),
             "{\"sys\":{\"name\":\"%s\",\"ver\":\"%s\",\"app\":\"%s\",\"idf\":\"%s\","
             "\"chip\":\"%s\",\"cores\":%u,\"rev\":%u,\"up\":%u,"
             "\"heap\":%u,\"heap_min\":%u,\"psram\":%u,\"flash\":%u,"
             "\"mac\":\"%s\",\"reset\":\"%s\"},"
             "\"wifi\":{\"on\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"web\":%s},"
             "\"bt\":{\"state\":%d,\"state_name\":\"%s\",\"adv\":%s,\"conn\":%s,"
             "\"peer\":\"%s\",\"peer_name\":\"%s\"},"
             "\"stor\":[{\"id\":\"tf\",\"ok\":%s,\"total\":%llu,\"free\":%llu},"
             "{\"id\":\"int\",\"ok\":%s,\"total\":%llu,\"free\":%llu}]}",
             SZPI_OS_NAME, SZPI_OS_VERSION,
             (si.app_version != NULL) ? si.app_version : "",
             (si.idf_version != NULL) ? si.idf_version : "",
             (si.chip_model != NULL) ? si.chip_model : "",
             (unsigned)si.chip_cores, (unsigned)si.chip_revision, (unsigned)si.uptime_s,
             (unsigned)si.heap_internal_free, (unsigned)si.heap_internal_min,
             (unsigned)si.heap_psram_free, (unsigned)si.flash_size,
             esc_mac, (si.reset_reason != NULL) ? si.reset_reason : "",
             net.wifi_connected ? "true" : "false", esc_ssid, net.ip_addr, (int)net.rssi,
             svc_web_is_running() ? "true" : "false",
             (int)diag.state, svc_bt_state_name(diag.state),
             diag.adv_active ? "true" : "false", diag.connected ? "true" : "false",
             esc_peer, esc_peer_name,
             tf_ok ? "true" : "false", (unsigned long long)tf.total_bytes,
             (unsigned long long)tf.free_bytes,
             in_ok ? "true" : "false", (unsigned long long)in.total_bytes,
             (unsigned long long)in.free_bytes);

    return send_json(req, json);
}

static esp_err_t h_logs(httpd_req_t *req)
{
    char *ring = s_scratch->log_ring;

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (svc_sysinfo_get_recent_logs(ring, sizeof(s_scratch->log_ring)) != ESP_OK) {
        return httpd_resp_send(req, "", 0);
    }

    /* 环是"最后 N 字节"，开头可能截在半行上：跳到第一个换行，网页里看着整齐 */
    const char *text = strchr(ring, '\n');
    text = (text != NULL) ? text + 1 : ring;

    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------- 列表 / 上传 / 下载 ------------------------------- */

static int list_cmp(const void *a, const void *b)
{
    const svc_storage_entry_t *x = (const svc_storage_entry_t *)a;
    const svc_storage_entry_t *y = (const svc_storage_entry_t *)b;

    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;      /* 目录在前 */
    return strcasecmp(x->name, y->name);
}

static esp_err_t h_files(httpd_req_t *req)
{
    char path[WEB_PATH_MAX];
    if (!qparam(req, "path", path, sizeof(path)) || path[0] == '\0') {
        snprintf(path, sizeof(path), "/sdcard");
    }
    if (!path_is_storage(path)) return send_result(req, false, "路径不合法");

    /* 去掉结尾多余的 '/'（"/sdcard/" → "/sdcard"） */
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = '\0';

    /* 容量取所在存储的那一份 */
    const bool is_tf = (strncmp(path, "/sdcard", 7) == 0);
    svc_storage_info_t info;
    memset(&info, 0, sizeof(info));
    const bool info_ok = (svc_storage_get_info(is_tf ? SVC_STORAGE_TF_CARD
                                                     : SVC_STORAGE_INTERNAL_FLASH,
                                               &info) == ESP_OK);

    svc_storage_entry_t *items = malloc(sizeof(*items) * WEB_LIST_MAX);
    size_t count = 0;
    bool trunc = false;

    if (items != NULL) {
        svc_storage_iter_t it = NULL;
        if (svc_storage_iter_start(path, &it) == ESP_OK) {
            svc_storage_entry_t *e;
            while ((e = svc_storage_iter_next(it)) != NULL) {
                if (count >= WEB_LIST_MAX) {
                    trunc = true;
                    break;
                }
                memcpy(&items[count++], e, sizeof(*e));
            }
            svc_storage_iter_end(it);
        }
        qsort(items, count, sizeof(*items), list_cmp);
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (items == NULL) return send_result(req, false, "内存不足");

    /* 头：当前目录 / 上级目录 / 容量。这几块拼串的缓冲走 PSRAM 暂存：handler 都在 httpd 的
     * 同一个任务里顺序执行，共用安全；内部 RAM 很紧，不摊在这儿。 */
    char *esc_path = s_scratch->files_esc_path;
    char *head = s_scratch->files_head;
    char *item = s_scratch->files_item;

    char parent[WEB_PATH_MAX] = "";
    if (strcmp(path, "/sdcard") != 0 && strcmp(path, "/internal") != 0) {
        snprintf(parent, sizeof(parent), "%s", path);
        char *slash = strrchr(parent, '/');
        if (slash != NULL) *slash = '\0';
    }

    json_escape_into(esc_path, sizeof(s_scratch->files_esc_path), path);

    snprintf(head, sizeof(s_scratch->files_head),
             "{\"ok\":true,\"path\":\"%s\",\"parent\":\"%s\",\"ok_info\":%s,"
             "\"total\":%llu,\"free\":%llu,\"trunc\":%s,\"items\":[",
             esc_path, parent, info_ok ? "true" : "false",
             (unsigned long long)info.total_bytes, (unsigned long long)info.free_bytes,
             trunc ? "true" : "false");
    httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);

    for (size_t i = 0; i < count; i++) {
        int k = snprintf(item, sizeof(s_scratch->files_item), "%s{\"name\":\"", (i == 0) ? "" : ",");
        k += (int)json_escape_into(item + k, sizeof(s_scratch->files_item) - (size_t)k, items[i].name);
        snprintf(item + k, sizeof(s_scratch->files_item) - (size_t)k, "\",\"dir\":%s,\"size\":%u}",
                 items[i].is_dir ? "true" : "false", (unsigned)items[i].size);
        if (httpd_resp_send_chunk(req, item, HTTPD_RESP_USE_STRLEN) != ESP_OK) break;
    }

    free(items);
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);        /* 结束分块 */
    return ESP_OK;
}

/* 把文件名按 RFC 5987 编码（Content-Disposition 的 filename*） */
static void encode_filename(const char *name, char *out, size_t len)
{
    static const char *HEX = "0123456789ABCDEF";
    size_t o = 0;

    for (const unsigned char *p = (const unsigned char *)name; *p != '\0' && o + 4 < len; p++) {
        if (isalnum(*p) || *p == '.' || *p == '-' || *p == '_' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = HEX[*p >> 4];
            out[o++] = HEX[*p & 0x0f];
        }
    }

    out[o] = '\0';
}

static esp_err_t dl_chunk_cb(const void *data, size_t len, void *user)
{
    return httpd_resp_send_chunk((httpd_req_t *)user, (const char *)data, len);
}

static esp_err_t h_download(httpd_req_t *req)
{
    char path[WEB_PATH_MAX];
    if (!qparam(req, "path", path, sizeof(path))) return send_result(req, false, "缺少 path");
    if (!path_is_storage(path)) return send_result(req, false, "路径不合法");

    size_t size = 0;
    if (svc_storage_exists(path, &size) != ESP_OK) return send_result(req, false, "文件不存在");

    char dl[8] = "";
    const bool as_attach = (qparam(req, "dl", dl, sizeof(dl)) && strcmp(dl, "1") == 0);

    httpd_resp_set_type(req, mime_of(path));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (as_attach) {
        char enc[WEB_NAME_MAX * 3];
        char header[WEB_NAME_MAX * 3 + 64];
        encode_filename(path_basename(path), enc, sizeof(enc));
        snprintf(header, sizeof(header), "attachment; filename*=UTF-8''%s", enc);
        httpd_resp_set_hdr(req, "Content-Disposition", header);
    }

    const esp_err_t err = svc_storage_stream_read(path, &size, dl_chunk_cb, req);
    httpd_resp_send_chunk(req, NULL, 0);        /* 结束分块（客户端断开时也算收尾） */

    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "download interrupted: %s", path);
    }
    return ESP_OK;
}

static esp_err_t h_upload(httpd_req_t *req)
{
    char dir[WEB_PATH_MAX];
    char name[WEB_NAME_MAX];

    if (!qparam(req, "dir", dir, sizeof(dir)) || dir[0] == '\0') {
        snprintf(dir, sizeof(dir), "/sdcard");
    }
    if (!path_is_storage(dir)) return send_result(req, false, "目录不合法");
    if (!qparam(req, "name", name, sizeof(name)) || name[0] == '\0') {
        return send_result(req, false, "缺少文件名");
    }
    if (strchr(name, '/') != NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return send_result(req, false, "文件名不合法");
    }

    char path[WEB_PATH_MAX];
    unique_path(dir, name, path, sizeof(path));

    svc_storage_writer_t *w = NULL;
    if (svc_storage_write_open(path, &w) != ESP_OK) {
        return send_result(req, false, "无法创建文件");
    }

    char *buf = malloc(WEB_UP_CHUNK);
    if (buf == NULL) {
        svc_storage_write_close(w);
        svc_storage_remove(path);
        return send_result(req, false, "内存不足");
    }

    size_t total = 0;
    size_t remain = (size_t)req->content_len;
    /* 浏览器给 File / Blob 当 body 时一定带 Content-Length；万一来的是分块传输
     * （curl --data-binary 之类），按"收到 0 为止"处理 */
    const bool sized = (remain > 0);
    int timeouts = 0;
    esp_err_t err = ESP_OK;

    while (true) {
        if (sized && remain == 0) break;

        const size_t want = (sized && remain < WEB_UP_CHUNK) ? remain : WEB_UP_CHUNK;
        const int rd = httpd_req_recv(req, buf, want);

        if (rd == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > WEB_UP_TIMEOUT) {
                err = ESP_FAIL;
                break;
            }
            continue;
        }
        if (rd <= 0) {
            if (!sized) break;                  /* 分块传输：收到结束 */
            err = ESP_FAIL;                     /* 0 = 连接断了 */
            break;
        }

        timeouts = 0;
        if (svc_storage_write_chunk(w, buf, (size_t)rd) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        total += (size_t)rd;
        if (sized) remain -= (size_t)rd;
    }

    free(buf);
    svc_storage_write_close(w);

    if (err != ESP_OK) {
        svc_storage_remove(path);               /* 不留半截文件 */
        ESP_LOGW(TAG, "upload interrupted: %s (%u byte(s))", path, (unsigned)total);
        return send_result(req, false, "上传中断");
    }

    ESP_LOGI(TAG, "uploaded %s (%u byte(s))", path, (unsigned)total);

    char esc_path[WEB_PATH_MAX * 2];
    char esc_name[WEB_NAME_MAX * 2];
    json_escape_into(esc_path, sizeof(esc_path), path);
    json_escape_into(esc_name, sizeof(esc_name), path_basename(path));

    char out[WEB_PATH_MAX * 2 + WEB_NAME_MAX * 2 + 96];
    snprintf(out, sizeof(out), "{\"ok\":true,\"path\":\"%s\",\"name\":\"%s\",\"size\":%u}",
             esc_path, esc_name, (unsigned)total);
    return send_json(req, out);
}

static esp_err_t h_mkdir(httpd_req_t *req)
{
    char path[WEB_PATH_MAX];
    if (!qparam(req, "path", path, sizeof(path))) return send_result(req, false, "缺少 path");
    if (!path_is_storage(path) || strcmp(path, "/sdcard") == 0 || strcmp(path, "/internal") == 0) {
        return send_result(req, false, "路径不合法");
    }
    /* SPIFFS 的 VFS 没实现 mkdir（返回 ENOTSUP），内置存储建不了文件夹 */
    if (strncmp(path, "/internal", 9) == 0) {
        return send_result(req, false, "内置存储不支持新建文件夹");
    }

    const esp_err_t err = svc_storage_mkdir(path);
    if (err != ESP_OK) return send_result(req, false, "创建失败");
    ESP_LOGI(TAG, "mkdir %s", path);
    return send_result(req, true, NULL);
}

static esp_err_t h_rename(httpd_req_t *req)
{
    char path[WEB_PATH_MAX];
    char name[WEB_NAME_MAX];

    if (!qparam(req, "path", path, sizeof(path))) return send_result(req, false, "缺少 path");
    if (!path_is_storage(path)) return send_result(req, false, "路径不合法");
    if (!qparam(req, "name", name, sizeof(name)) || name[0] == '\0') {
        return send_result(req, false, "缺少新名字");
    }
    if (strchr(name, '/') != NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return send_result(req, false, "名字不合法");
    }

    /* 同目录内改名 */
    char dir[WEB_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash == NULL) return send_result(req, false, "路径不合法");
    *slash = '\0';

    /* 拼"目录/新名字"：两边都可能接近上限，先算长度再拼，超了直接拒绝（不改名半截路径） */
    char to[WEB_PATH_MAX];
    const size_t dir_len = strlen(dir);
    const size_t name_len = strlen(name);
    if (dir_len + 1 + name_len >= sizeof(to)) return send_result(req, false, "名字太长");

    memcpy(to, dir, dir_len);
    to[dir_len] = '/';
    memcpy(to + dir_len + 1, name, name_len + 1);
    if (strcmp(to, path) == 0) return send_result(req, true, NULL);      /* 没变 */

    const esp_err_t err = svc_storage_rename(path, to);
    if (err == ESP_ERR_INVALID_STATE) return send_result(req, false, "同名文件已存在");
    if (err != ESP_OK) return send_result(req, false, "改名失败");

    ESP_LOGI(TAG, "rename %s -> %s", path, to);
    return send_result(req, true, NULL);
}

/* -------------------------- 后台任务（删除 / 复制 / 剪切） -------------------------- */
/* 这三种操作可能涉及成百上千个文件，放在 handler 里会把整个服务堵住（esp_http_server
 * 只有一个任务，一个请求不返回，别的请求就一直排队）。所以改成"提交任务 + 轮询进度"：
 * handler 只收下路径清单立刻返回，真正的活在 svc.web 任务里干。 */

typedef enum {
    WEB_JOB_NONE = 0,
    WEB_JOB_DELETE,
    WEB_JOB_COPY,
    WEB_JOB_CUT,
} web_job_kind_t;

static struct {
    volatile bool busy;             /* 正在跑；跑完置 false，结果留着给 /api/job 读 */
    web_job_kind_t kind;
    char to[WEB_PATH_MAX];          /* copy / cut 的目标目录 */
    char *paths;                    /* 每个路径一个 '\0'，末尾再多一个 '\0' 结束 */
    size_t paths_len;
    size_t total;
    size_t done;
    size_t failed;
    bool truncated;                 /* 路径清单超过上限（日志里提示一次） */
    char current[WEB_NAME_MAX];     /* 正在处理的项（界面显示用） */
    char failed_name[WEB_NAME_MAX]; /* 最近一个失败项 */
} s_job;

static void job_clear(void)
{
    free(s_job.paths);
    s_job.paths = NULL;
    s_job.paths_len = 0;
    s_job.total = 0;
    s_job.done = 0;
    s_job.failed = 0;
    s_job.truncated = false;
    s_job.current[0] = '\0';
    s_job.failed_name[0] = '\0';
    s_job.kind = WEB_JOB_NONE;
}

/* 收集一行路径（删除 / 粘贴的请求体就是一行一个路径） */
static bool job_collect(const char *line, void *user)
{
    (void)user;

    const size_t n = strlen(line);
    if (n == 0 || n >= WEB_PATH_MAX) return false;
    if (s_job.total >= WEB_JOB_MAX_PATHS) {
        if (!s_job.truncated) {
            s_job.truncated = true;
            ESP_LOGW(TAG, "job path list truncated at %d", WEB_JOB_MAX_PATHS);
        }
        return false;
    }
    if (!path_is_storage(line)) return false;
    /* 存储根目录不许删 */
    if (s_job.kind == WEB_JOB_DELETE &&
        (strcmp(line, "/sdcard") == 0 || strcmp(line, "/internal") == 0)) {
        return false;
    }

    char *p = realloc(s_job.paths, s_job.paths_len + n + 2);
    if (p == NULL) return false;

    s_job.paths = p;
    memcpy(s_job.paths + s_job.paths_len, line, n + 1);
    s_job.paths_len += n + 1;
    s_job.paths[s_job.paths_len] = '\0';        /* 结束标记 */
    s_job.total++;
    return true;
}

/* 把清单交给 svc.web 任务去做；返回发送给浏览器的 JSON */
static esp_err_t job_submit(httpd_req_t *req, web_job_kind_t kind, const char *to)
{
    if (s_job.busy) return send_result(req, false, "已有任务在进行，请等它跑完");

    job_clear();
    s_job.kind = kind;
    if (to != NULL) snprintf(s_job.to, sizeof(s_job.to), "%s", to);

    (void)foreach_body_line(req, job_collect, NULL, NULL, 0);

    if (s_job.total == 0) {
        job_clear();
        return send_result(req, false, "没有可处理的路径");
    }

    s_job.busy = true;
    const uint8_t cmd = WEB_CMD_JOB;
    if (xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        s_job.busy = false;
        job_clear();
        return send_result(req, false, "任务队列已满");
    }

    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"started\":true,\"total\":%u}",
             (unsigned)s_job.total);
    return send_json(req, out);
}

static esp_err_t h_delete(httpd_req_t *req)
{
    return job_submit(req, WEB_JOB_DELETE, NULL);
}

static esp_err_t h_paste(httpd_req_t *req)
{
    char to[WEB_PATH_MAX];
    char mode[8] = "copy";

    if (!qparam(req, "to", to, sizeof(to))) return send_result(req, false, "缺少目标目录");
    if (!path_is_storage(to)) return send_result(req, false, "目标不合法");
    qparam(req, "mode", mode, sizeof(mode));

    return job_submit(req, (strcmp(mode, "cut") == 0) ? WEB_JOB_CUT : WEB_JOB_COPY, to);
}

/* 进度查询：界面每隔几百毫秒问一次 */
static esp_err_t h_job(httpd_req_t *req)
{
    char *json = s_scratch->job_json;
    char *esc_name = s_scratch->job_esc_current;
    char *esc_fail = s_scratch->job_esc_failed;

    json_escape_into(esc_name, sizeof(s_scratch->job_esc_current), s_job.current);
    json_escape_into(esc_fail, sizeof(s_scratch->job_esc_failed), s_job.failed_name);

    snprintf(json, sizeof(s_scratch->job_json),
             "{\"ok\":true,\"busy\":%s,\"kind\":%d,\"total\":%u,\"done\":%u,\"failed\":%u,"
             "\"name\":\"%s\",\"failed_name\":\"%s\"}",
             s_job.busy ? "true" : "false", (int)s_job.kind, (unsigned)s_job.total,
             (unsigned)s_job.done, (unsigned)s_job.failed, esc_name, esc_fail);
    return send_json(req, json);
}

/* 在 svc.web 任务里跑一个任务（一次一个） */
static void job_run(void)
{
    for (const char *cur = s_job.paths; cur != NULL && *cur != '\0'; cur += strlen(cur) + 1) {
        snprintf(s_job.current, sizeof(s_job.current), "%s", path_basename(cur));

        bool ok = false;

        if (s_job.kind == WEB_JOB_DELETE) {
            ok = (svc_storage_remove_tree(cur) == ESP_OK);
            if (ok) ESP_LOGI(TAG, "delete %s", cur);
        } else {
            char dst[WEB_PATH_MAX];
            unique_path(s_job.to, path_basename(cur), dst, sizeof(dst));

            /* 目标落在来源目录里要拦住（例如把 /sdcard/a 贴进 /sdcard/a/b）：会无限递归 */
            const size_t n = strlen(cur);
            const bool inside = (strncmp(dst, cur, n) == 0 && dst[n] == '/');

            if (inside || strcmp(cur, dst) == 0) {
                ok = false;
            } else if (svc_storage_copy(cur, dst) != ESP_OK) {
                ok = false;
            } else if (s_job.kind == WEB_JOB_CUT) {
                ok = (svc_storage_remove_tree(cur) == ESP_OK);
                if (!ok) ESP_LOGW(TAG, "cut: copied but failed to remove %s", cur);
            } else {
                ok = true;
            }

            if (ok) ESP_LOGI(TAG, "%s %s -> %s", (s_job.kind == WEB_JOB_CUT) ? "move" : "copy",
                             cur, dst);
        }

        if (ok) {
            s_job.done++;
        } else {
            s_job.failed++;
            snprintf(s_job.failed_name, sizeof(s_job.failed_name), "%s", path_basename(cur));
        }
    }

    ESP_LOGI(TAG, "job done: kind=%d total=%u done=%u failed=%u", (int)s_job.kind,
             (unsigned)s_job.total, (unsigned)s_job.done, (unsigned)s_job.failed);

    s_job.current[0] = '\0';
    s_job.busy = false;                 /* 清单留着，等界面读完结果、下次任务再清 */
}

/* -------------------------- 自定义请求（脚本网页路由） -------------------------- */
/* 脚本运行时装进来的路由：路径形如 /s/xxx，去掉 /s 前缀后交给注册的回调。回调在 HTTP
 * 任务里跑，所以它绝对不能慢 —— 这一层只管转发与下发，具体等待逻辑在上层。 */

static svc_web_custom_cb_t s_custom_cb = NULL;
static void *s_custom_user = NULL;

esp_err_t svc_web_set_custom_handler(svc_web_custom_cb_t cb, void *user)
{
    if (cb == NULL) return ESP_ERR_INVALID_ARG;
    if (s_custom_cb != NULL) return ESP_ERR_INVALID_STATE;      /* 同时只允许一个 */

    s_custom_cb = cb;
    s_custom_user = user;
    return ESP_OK;
}

void svc_web_clear_custom_handler(void)
{
    s_custom_cb = NULL;
    s_custom_user = NULL;
}

static const char *method_name(httpd_method_t m)
{
    switch (m) {
    case HTTP_GET:    return "GET";
    case HTTP_POST:   return "POST";
    case HTTP_PUT:    return "PUT";
    case HTTP_DELETE: return "DELETE";
    case HTTP_PATCH:  return "PATCH";
    default:          return "GET";
    }
}

static const char *status_line(int code)
{
    switch (code) {
    case 400: return "400 Bad Request";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    case 504: return "504 Gateway Timeout";
    default:  return "200 OK";
    }
}

/* 静默回一个状态：不走 httpd_resp_send_err，免得脚本停掉后浏览器还在轮询 /s 时
 * 串口被 "httpd_resp_send_err: 404 ..." 刷屏（页面已经下线，属于正常现象） */
static esp_err_t send_quiet_status(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t h_script(httpd_req_t *req)
{
    char *path = s_scratch->custom_path;
    char *query = s_scratch->custom_query;
    char *body = s_scratch->custom_body;

    if (s_custom_cb == NULL) {
        return send_quiet_status(req, "404 Not Found", "没有脚本在提供网页");
    }

    /* req->uri 含 query（IDF 的 query 解析就是从它里找 '?'），先切开 */
    const char *full = req->uri + 2;                    /* 跳过 "/s" */
    const char *qmark = strchr(full, '?');
    const size_t plen = (qmark != NULL) ? (size_t)(qmark - full) : strlen(full);

    if (plen == 0) {
        snprintf(path, sizeof(s_scratch->custom_path), "/");
    } else if (plen >= sizeof(s_scratch->custom_path)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "路径太长");
    } else {
        memcpy(path, full, plen);
        path[plen] = '\0';
    }

    if (qmark != NULL) snprintf(query, sizeof(s_scratch->custom_query), "%s", qmark + 1);
    else query[0] = '\0';

    /* POST 体：最多收这么多，多出来的不读（IDF 会因此关掉这条连接，脚本用 query 传参就够） */
    size_t body_len = 0;
    size_t remain = (size_t)req->content_len;
    while (remain > 0 && body_len + 1 < sizeof(s_scratch->custom_body)) {
        const size_t want = (remain < sizeof(s_scratch->custom_body) - 1 - body_len)
                                ? remain
                                : sizeof(s_scratch->custom_body) - 1 - body_len;
        const int rd = httpd_req_recv(req, body + body_len, want);
        if (rd == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (rd <= 0) break;
        body_len += (size_t)rd;
        remain -= (size_t)rd;
    }
    body[body_len] = '\0';

    const svc_web_req_t sreq = {
        .method = method_name(req->method),
        .path = path,
        .query = query,
        .body = body,
        .body_len = body_len,
    };

    svc_web_resp_t resp;
    memset(&resp, 0, sizeof(resp));

    const esp_err_t err = s_custom_cb(&sreq, &resp, s_custom_user);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_quiet_status(req, "404 Not Found", "没有脚本在提供网页");
    }
    if (err == ESP_ERR_TIMEOUT) {
        /* IDF 的 httpd_err_code_t 没有 503，直接设状态行发纯文本 */
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "脚本响应超时");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "脚本没有处理这个请求");
    }

    httpd_resp_set_status(req, status_line(resp.status));
    httpd_resp_set_type(req, (resp.content_type != NULL) ? resp.content_type : "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)resp.body, (ssize_t)resp.body_len);
}

/* ------------------------------- 服务生命周期 ------------------------------- */

static const httpd_uri_t WEB_URIS[] = {
    { .uri = "/",             .method = HTTP_GET,  .handler = h_index },
    { .uri = "/favicon.ico",  .method = HTTP_GET,  .handler = h_favicon },
    { .uri = "/api/status",   .method = HTTP_GET,  .handler = h_status },
    { .uri = "/api/logs",     .method = HTTP_GET,  .handler = h_logs },
    { .uri = "/api/files",    .method = HTTP_GET,  .handler = h_files },
    { .uri = "/api/job",      .method = HTTP_GET,  .handler = h_job },
    { .uri = "/api/download", .method = HTTP_GET,  .handler = h_download },
    { .uri = "/api/upload",   .method = HTTP_POST, .handler = h_upload },
    { .uri = "/api/mkdir",    .method = HTTP_POST, .handler = h_mkdir },
    { .uri = "/api/rename",   .method = HTTP_POST, .handler = h_rename },
    { .uri = "/api/delete",   .method = HTTP_POST, .handler = h_delete },
    { .uri = "/api/paste",    .method = HTTP_POST, .handler = h_paste },
    /* 脚本区：精确的 /s 与子路径放在最后，按注册顺序匹配，不会抢前面的接口 */
    { .uri = "/s",            .method = HTTP_ANY,  .handler = h_script },
    { .uri = "/s/*",          .method = HTTP_ANY,  .handler = h_script },
};

bool svc_web_is_running(void)
{
    return s_server != NULL;
}

static esp_err_t web_server_start(void)
{
    if (s_server != NULL) return ESP_OK;

    /* 拼串暂存放 PSRAM：省下的都是内部 RAM（脚本任务的栈要从内部拿） */
    if (s_scratch == NULL) {
        s_scratch = heap_caps_malloc(sizeof(web_scratch_t), MALLOC_CAP_SPIRAM);
        if (s_scratch == NULL) {
            ESP_LOGE(TAG, "scratch alloc failed (%u byte(s))", (unsigned)sizeof(web_scratch_t));
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = sizeof(WEB_URIS) / sizeof(WEB_URIS[0]);
    /* 用通配匹配：脚本区注册的是 "/s/" 加通配，默认的精确比较匹配不上；精确路径不受影响
     * （模板里没有 '*' / '?' 时它就是精确比较）。匹配按注册顺序，先精确后通配。 */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    /* 不要踢掉最久没活动的连接：大文件上下传时状态轮询会排在后面，
     * 开了 LRU 清理会给它一个"连接被关"，页面上就闪"连接断开"；这里让请求老实等着 */
    cfg.lru_purge_enable = false;
    cfg.stack_size = 6144;              /* handler 里有几百字节的局部缓冲 */
    cfg.recv_wait_timeout = 10;         /* 上传大文件时别轻易超时 */
    cfg.send_wait_timeout = 10;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = NULL;
        return ESP_FAIL;
    }

    for (size_t i = 0; i < sizeof(WEB_URIS) / sizeof(WEB_URIS[0]); i++) {
        if (httpd_register_uri_handler(s_server, &WEB_URIS[i]) != ESP_OK) {
            ESP_LOGW(TAG, "register %s failed", WEB_URIS[i].uri);
        }
    }

    return ESP_OK;
}

static void web_server_stop(void)
{
    if (s_server == NULL) return;
    httpd_stop(s_server);
    s_server = NULL;

    /* handler 不会再跑了，暂存可以还掉（下次连上再分配） */
    free(s_scratch);
    s_scratch = NULL;
    ESP_LOGI(TAG, "stopped");
}

/* 起停与后台任务都在这个任务里做：配网的 httpd 可能还占着 80 端口，失败要退避重试 */
static void web_task(void *arg)
{
    (void)arg;
    uint8_t cmd = 0;

    while (true) {
        if (xQueueReceive(s_cmds, &cmd, portMAX_DELAY) != pdTRUE) continue;

        if (cmd == WEB_CMD_STOP) {
            web_server_stop();
            continue;
        }

        if (cmd == WEB_CMD_JOB) {
            job_run();                      /* 复制 / 剪切 / 删除：跑完再看下一条命令 */
            continue;
        }

        if (s_held) continue;               /* 相机占着内部内存，先不提供网页 */

        for (int i = 0; i < 12 && s_server == NULL; i++) {
            if (i > 0) {
                /* 退避期间只看不动：用 peek，别把队列里的 STOP / JOB 吃掉 */
                uint8_t peek = 0;
                if (xQueuePeek(s_cmds, &peek, pdMS_TO_TICKS(500)) == pdTRUE &&
                    peek == WEB_CMD_STOP) {
                    break;
                }
            }

            if (web_server_start() == ESP_OK) {
                ESP_LOGI(TAG, "started on port 80");
                break;
            }
        }

        if (s_server == NULL) ESP_LOGW(TAG, "start failed (port busy?)");
    }
}

void svc_web_set_hold(bool hold)
{
    s_held = hold;
    if (s_cmds == NULL) return;

    if (hold) {
        const uint8_t cmd = WEB_CMD_STOP;
        xQueueSend(s_cmds, &cmd, 0);
        /* 等 httpd 真正停下、把任务栈还给堆 —— 相机紧接着就要那块内部 DMA */
        for (int i = 0; i < 100 && s_server != NULL; i++) vTaskDelay(pdMS_TO_TICKS(5));
    } else {
        /* 恢复：只在真的连着（且不在配网）时才重起，免得空绑 80 端口 */
        svc_net_status_t st;
        if (!svc_net_prov_is_active() && svc_net_get_status(&st) == ESP_OK && st.wifi_connected) {
            const uint8_t cmd = WEB_CMD_START;
            xQueueSend(s_cmds, &cmd, 0);
        }
    }
}

static void net_event_cb(const svc_event_t *evt, void *user)
{
    (void)user;

    /* 事件总线任务里只投一条命令：起停 HTTP 服务交给 svc.web 任务 */
    if (evt->id == SVC_EVENT_WIFI_CONNECTED) {
        /* 配网期间端口 80 被配网页占着，而且那时报的"已连接"可能是旧的 AP：
         * 等配网结束、重新连上再来（那次会正常走 START）。相机占着内存时同理不启动。 */
        if (svc_net_prov_is_active() || s_held) return;
        const uint8_t cmd = WEB_CMD_START;
        if (s_cmds != NULL) xQueueSend(s_cmds, &cmd, 0);
    } else {
        const uint8_t cmd = WEB_CMD_STOP;
        if (s_cmds != NULL) xQueueSend(s_cmds, &cmd, 0);
    }
}

esp_err_t svc_web_init(void)
{
    if (s_task != NULL) return ESP_OK;

    s_cmds = xQueueCreate(4, sizeof(uint8_t));
    if (s_cmds == NULL) return ESP_ERR_NO_MEM;

    if (xTaskCreate(web_task, "svc.web", WEB_TASK_STACK, NULL, WEB_TASK_PRIO, &s_task) != pdPASS) {
        vQueueDelete(s_cmds);
        s_cmds = NULL;
        return ESP_ERR_NO_MEM;
    }

    svc_event_bus_subscribe(SVC_EVENT_WIFI_CONNECTED, net_event_cb, NULL);
    svc_event_bus_subscribe(SVC_EVENT_WIFI_DISCONNECTED, net_event_cb, NULL);

    /* 自动重连可能比这里早：已经连上了就先起一遍（配网中不算） */
    svc_net_status_t st;
    if (!svc_net_prov_is_active() && svc_net_get_status(&st) == ESP_OK && st.wifi_connected) {
        const uint8_t cmd = WEB_CMD_START;
        xQueueSend(s_cmds, &cmd, 0);
    }

    ESP_LOGI(TAG, "initialized (http://<ip>/ after Wi-Fi connects)");
    return ESP_OK;
}
