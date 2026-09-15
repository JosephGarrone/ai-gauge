/*
 * HTTP API and the provisioning / management page. See docs/networking.md.
 *
 * Handlers run on the HTTP server task (core 0). They must never touch LVGL directly --
 * changes are reported through the callbacks in net_svc.h and applied by the owner.
 */

#include "net_svc.h"
#include "net_svc_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "gauge_store.h"
#include "net_svc_editor_files.h"

static const char *TAG = "net_svc_http";

/* Bounds the per-request buffer. Comfortably above the schema's needs. */
#define MAX_BODY_BYTES GAUGE_STORE_MAX_FILE_BYTES
#define XML_SUFFIX     ".xml"
#define ORIGIN_LEN     64
#define OTA_CHUNK      4096

static httpd_handle_t s_server;

/* Set by the app, read by the HTTP task. */
static char         s_active_gauge[GAUGE_CONFIG_MAX_ID_LEN];
static portMUX_TYPE s_active_lock = portMUX_INITIALIZER_UNLOCKED;

void net_svc_set_active_gauge(const char *id)
{
    taskENTER_CRITICAL(&s_active_lock);
    strlcpy(s_active_gauge, (id != NULL) ? id : "", sizeof(s_active_gauge));
    taskEXIT_CRITICAL(&s_active_lock);
}

/* ---------------------------------------------------------------------- helpers ---- */

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    char body[192];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* Reads a request body into a NUL-terminated heap buffer. Caller frees. */
static char *read_body(httpd_req_t *req, size_t *out_len)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY_BYTES) {
        return NULL;
    }

    /*
     * PSRAM explicitly. Internal memory is nearly exhausted once WiFi runs, and a body under
     * the 4KB SPIRAM_MALLOC_ALWAYSINTERNAL threshold would otherwise be taken from it.
     */
    char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return NULL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, buf + received, req->content_len - received);
        if (n <= 0) {
            free(buf);
            return NULL;
        }
        received += (size_t)n;
    }

    buf[received] = '\0';
    *out_len      = received;
    return buf;
}

/* Extracts the trailing path segment, e.g. "/api/config/boost" -> "boost". */
static const char *last_segment(const char *uri)
{
    const char *slash = strrchr(uri, '/');
    return (slash != NULL) ? slash + 1 : uri;
}

static bool origin_is_loopback(const char *origin)
{
    static const char *const allowed[] = {"http://localhost", "http://127.0.0.1", "http://[::1]"};

    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        size_t n = strlen(allowed[i]);
        if (strncmp(origin, allowed[i], n) == 0 && (origin[n] == '\0' || origin[n] == ':')) {
            return true;
        }
    }
    return false;
}

/*
 * CORS for one case only: the face editor running from its local dev server. A loopback origin
 * is echoed back; any other origin gets no CORS headers, so a page on some other site still cannot
 * read replies, or pass the preflight a PUT or DELETE needs to replace faces (ADR 0008). The
 * editor normally needs none of this, because the gauge serves it at /editor/.
 *
 * httpd_resp_set_hdr() keeps the pointer, not a copy, so @p origin must outlive the response:
 * callers pass a buffer on their own stack.
 */
static bool allow_loopback_origin(httpd_req_t *req, char *origin, size_t origin_len)
{
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, origin_len) != ESP_OK ||
        !origin_is_loopback(origin)) {
        return false;
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", origin);
    httpd_resp_set_hdr(req, "Vary", "Origin");
    return true;
}

/*
 * Minimal form-field extraction. The provisioning page posts
 * application/x-www-form-urlencoded, and pulling in a full parser for two fields is not
 * worth the flash.
 */
static bool form_field(const char *body, const char *name, char *out, size_t out_len)
{
    char key[32];
    int  key_len = snprintf(key, sizeof(key), "%s=", name);

    const char *p = strstr(body, key);
    if (p == NULL) {
        return false;
    }
    /* Only accept a match at the start or right after a separator. */
    if (p != body && p[-1] != '&') {
        return false;
    }
    p += key_len;

    size_t i = 0;
    while (*p != '\0' && *p != '&' && i + 1 < out_len) {
        if (*p == '+') {
            out[i++] = ' ';
            p++;
        } else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], '\0'};
            out[i++]    = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            out[i++] = *p++;
        }
    }

    out[i] = '\0';
    return true;
}

/* ------------------------------------------------------------------- endpoints ----- */

static esp_err_t status_get(httpd_req_t *req)
{
    net_svc_status_t net;
    net_svc_get_status(&net);

    const esp_app_desc_t *app = esp_app_get_description();

    char origin[ORIGIN_LEN];
    allow_loopback_origin(req, origin, sizeof(origin));

    char active[GAUGE_CONFIG_MAX_ID_LEN];
    taskENTER_CRITICAL(&s_active_lock);
    memcpy(active, s_active_gauge, sizeof(active));
    taskEXIT_CRITICAL(&s_active_lock);

    /*
     * The minimums are what future measurements need without a serial cable: internal RAM is the
     * board's binding constraint, and this handler runs on the HTTP server task, so its own stack
     * high-water mark is the server's (docs/performance.md).
     */
    char body[512];
    snprintf(body, sizeof(body),
             "{\"version\":\"%s\",\"idf\":\"%s\",\"uptime_s\":%lld,"
             "\"wifi\":{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"hostname\":\"%s\"},"
             "\"heap\":{\"internal\":%u,\"internal_min\":%u,\"psram\":%u},"
             "\"httpd_stack_min\":%u,"
             "\"active_gauge\":\"%s\","
             "\"storage_mounted\":%s}",
             app ? app->version : "unknown", app ? app->idf_ver : "unknown",
             esp_timer_get_time() / 1000000,
             net_svc_state_str(net.state), net.ssid, net.ip, net.rssi, net.hostname,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(NULL), active,
             gauge_store_mounted() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t gauges_get(httpd_req_t *req)
{
    char origin[ORIGIN_LEN];
    allow_loopback_origin(req, origin, sizeof(origin));

    gauge_store_list_t list;
    gauge_store_list(&list);

    char   body[512];
    size_t used = (size_t)snprintf(body, sizeof(body), "{\"gauges\":[");

    for (int i = 0; i < list.count && used < sizeof(body) - 4; i++) {
        used += (size_t)snprintf(body + used, sizeof(body) - used, "%s\"%s\"",
                                 (i == 0) ? "" : ",", list.ids[i]);
    }
    snprintf(body + used, sizeof(body) - used, "],\"max\":%d}", GAUGE_STORE_MAX_GAUGES);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* The stored file itself, so the editor can load a face back from the gauge. */
static esp_err_t config_get_xml(httpd_req_t *req, const char *id)
{
    char   err[GAUGE_STORE_ERR_LEN] = {0};
    size_t len                      = 0;
    char  *xml                      = gauge_store_read_xml(id, &len, err, sizeof(err));
    if (xml == NULL) {
        return send_json_error(req, "404 Not Found", err[0] ? err : "not found");
    }

    httpd_resp_set_type(req, "application/xml");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send(req, xml, (ssize_t)len);
    free(xml);
    return ret;
}

static esp_err_t config_get(httpd_req_t *req)
{
    char origin[ORIGIN_LEN];
    allow_loopback_origin(req, origin, sizeof(origin));

    /* "/api/config/<id>.xml" is the raw file; "/api/config/<id>" the parsed summary. */
    const char  *id         = last_segment(req->uri);
    const size_t id_len     = strlen(id);
    const size_t suffix_len = strlen(XML_SUFFIX);
    if (id_len > suffix_len && strcmp(id + id_len - suffix_len, XML_SUFFIX) == 0) {
        char stem[GAUGE_CONFIG_MAX_ID_LEN];
        if (id_len - suffix_len >= sizeof(stem)) {
            return send_json_error(req, "404 Not Found", "invalid gauge name");
        }
        memcpy(stem, id, id_len - suffix_len);
        stem[id_len - suffix_len] = '\0';
        return config_get_xml(req, stem);
    }

    /* PSRAM, not this task's internal-RAM stack -- see gauge_store.c. */
    gauge_config_t *cfg = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_SPIRAM);
    char            err[GAUGE_STORE_ERR_LEN] = {0};
    if (cfg == NULL) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }

    /*
     * Loading through the parser means this reports the same verdict the gauge would reach,
     * rather than blindly streaming bytes that may not be usable.
     */
    if (gauge_store_load(id, cfg, err, sizeof(err)) != ESP_OK) {
        free(cfg);
        return send_json_error(req, "404 Not Found", err[0] ? err : "not found");
    }

    char body[256];
    snprintf(body, sizeof(body),
             "{\"id\":\"%s\",\"channel\":\"%s\",\"unit\":\"%s\","
             "\"min\":%.3f,\"max\":%.3f,\"warnings\":%u}",
             cfg->id, cfg->source.channel, cfg->source.unit,
             (double)cfg->source.min, (double)cfg->source.max, cfg->warning_count);
    free(cfg);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t config_put(httpd_req_t *req)
{
    char origin[ORIGIN_LEN];
    allow_loopback_origin(req, origin, sizeof(origin));

    const char *id = last_segment(req->uri);

    size_t len = 0;
    char  *xml = read_body(req, &len);
    if (xml == NULL) {
        return send_json_error(req, "400 Bad Request", "missing or oversized body");
    }

    char      err[GAUGE_STORE_ERR_LEN] = {0};
    esp_err_t save_err = gauge_store_save(id, xml, len, err, sizeof(err));
    free(xml);

    if (save_err != ESP_OK) {
        /* gauge_store_save validates before replacing, so the live config is untouched. */
        ESP_LOGW(TAG, "rejected upload of '%s': %s", id, err);
        return send_json_error(req, "400 Bad Request", err[0] ? err : "rejected");
    }

    const net_svc_callbacks_t *cb = net_svc_get_callbacks();
    if (cb->on_config_changed != NULL) {
        cb->on_config_changed(id, false);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t config_delete(httpd_req_t *req)
{
    char origin[ORIGIN_LEN];
    allow_loopback_origin(req, origin, sizeof(origin));

    const char *id = last_segment(req->uri);

    if (gauge_store_delete(id) != ESP_OK) {
        return send_json_error(req, "404 Not Found", "not found");
    }

    const net_svc_callbacks_t *cb = net_svc_get_callbacks();
    if (cb->on_config_changed != NULL) {
        cb->on_config_changed(id, true);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    size_t len = 0;
    char  *body = read_body(req, &len);
    if (body == NULL) {
        return send_json_error(req, "400 Bad Request", "missing body");
    }

    char ssid[NET_SVC_SSID_LEN] = {0};
    char pass[64]               = {0};

    bool have_ssid = form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "password", pass, sizeof(pass));
    free(body);

    if (!have_ssid || ssid[0] == '\0') {
        return send_json_error(req, "400 Bad Request", "ssid is required");
    }

    /*
     * Answer before switching networks. Applying the change tears down the association this
     * response would travel over, so replying afterwards would never reach the client.
     */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
                       "<!doctype html><meta charset=utf-8>"
                       "<style>body{font-family:system-ui;background:#0b0b0d;color:#eee;"
                       "text-align:center;padding:3rem 1rem}</style>"
                       "<h2>Connecting&hellip;</h2>"
                       "<p>The gauge is joining that network. Its settings page will show "
                       "the address once it succeeds.</p>"
                       "<p>If it cannot connect, the setup network will reappear.</p>");

    ESP_LOGI(TAG, "provisioning for '%s'", ssid);
    net_svc_set_credentials(ssid, pass);
    return ESP_OK;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return send_json_error(req, "500 Internal Server Error", "no OTA partition");
    }

    ESP_LOGW(TAG, "OTA starting: %u bytes -> %s", (unsigned)req->content_len, target->label);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }

    /* Exactly at the 4KB internal threshold, so plain malloc() would take internal memory. */
    char  *chunk    = heap_caps_malloc(OTA_CHUNK, MALLOC_CAP_SPIRAM);
    size_t received = 0;

    if (chunk == NULL) {
        esp_ota_abort(handle);
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }

    while (received < req->content_len) {
        int n = httpd_req_recv(req, chunk, OTA_CHUNK);
        if (n <= 0) {
            free(chunk);
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "OTA aborted: upload ended early at %u bytes", (unsigned)received);
            return send_json_error(req, "400 Bad Request", "upload interrupted");
        }

        err = esp_ota_write(handle, chunk, (size_t)n);
        if (err != ESP_OK) {
            free(chunk);
            esp_ota_abort(handle);
            return send_json_error(req, "400 Bad Request", esp_err_to_name(err));
        }
        received += (size_t)n;
    }

    free(chunk);

    /* Validates the image; a bad one is rejected here, before it can be booted. */
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA image rejected: %s", esp_err_to_name(err));
        return send_json_error(req, "400 Bad Request", esp_err_to_name(err));
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");

    ESP_LOGW(TAG, "OTA complete, restarting into %s", target->label);
    vTaskDelay(pdMS_TO_TICKS(500)); /* let the response drain */
    esp_restart();
    return ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req)
{
    net_svc_status_t net;
    net_svc_get_status(&net);

    const esp_app_desc_t *app = esp_app_get_description();

    /* In setup mode the only useful thing to offer is the provisioning form. */
    if (net.state == NET_SVC_AP_MODE) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_sendstr(
            req,
            "<!doctype html><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>AI-Gauge setup</title>"
            "<style>body{font-family:system-ui;background:#0b0b0d;color:#eee;margin:0;"
            "display:grid;place-items:center;min-height:100vh}"
            "form{width:min(22rem,90vw)}h1{font-size:1.4rem}"
            "input{width:100%;padding:.7rem;margin:.35rem 0 1rem;border-radius:6px;"
            "border:1px solid #333;background:#17171a;color:#eee;box-sizing:border-box}"
            "button{width:100%;padding:.8rem;border:0;border-radius:6px;background:#ff1744;"
            "color:#fff;font-size:1rem}label{font-size:.85rem;color:#9e9e9e}</style>"
            "<form method=post action=/api/wifi>"
            "<h1>AI-Gauge setup</h1>"
            "<label>Network name</label><input name=ssid autofocus>"
            "<label>Password</label><input name=password type=password>"
            "<button type=submit>Connect</button></form>");
    }

    char body[1024];
    snprintf(body, sizeof(body),
             "<!doctype html><meta charset=utf-8>"
             "<meta name=viewport content='width=device-width,initial-scale=1'>"
             "<title>AI-Gauge</title>"
             "<style>body{font-family:system-ui;background:#0b0b0d;color:#eee;margin:0;"
             "padding:2rem 1rem;max-width:34rem;margin-inline:auto}"
             "code{background:#17171a;padding:.15em .4em;border-radius:4px}"
             "h1{color:#ff1744}li{margin:.4rem 0}</style>"
             "<h1>AI-Gauge</h1>"
             "<p>Firmware %s &middot; connected to <b>%s</b> as <code>%s</code></p>"
             "<p><a href=/editor/ style='color:#ff1744'>Open the face editor</a></p>"
             "<h3>API</h3><ul>"
             "<li><code>GET /api/status</code></li>"
             "<li><code>GET /api/gauges</code></li>"
             "<li><code>GET|PUT|DELETE /api/config/&lt;id&gt;</code></li>"
             "<li><code>POST /api/ota</code></li>"
             "</ul>"
             "<p style='color:#ffab00'>This interface has no authentication. Use it only on "
             "a network you trust.</p>",
             app ? app->version : "unknown", net.ssid, net.ip);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, body);
}

/* PUT and DELETE are preflighted; only a loopback origin passes (see allow_loopback_origin). */
static esp_err_t config_options(httpd_req_t *req)
{
    char origin[ORIGIN_LEN];
    if (!allow_loopback_origin(req, origin, sizeof(origin))) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, PUT, DELETE");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
    return httpd_resp_send(req, NULL, 0);
}

/* ----------------------------------------------------------------- face editor ----- */

static const char *content_type_for(const char *path)
{
    static const struct {
        const char *ext;
        const char *type;
    } types[] = {
        {".html", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".xml", "application/xml; charset=utf-8"},
    };

    const char *dot = strrchr(path, '.');
    for (size_t i = 0; dot != NULL && i < sizeof(types) / sizeof(types[0]); i++) {
        if (strcmp(dot, types[i].ext) == 0) {
            return types[i].type;
        }
    }
    return "application/octet-stream";
}

/*
 * The face editor, from the table built into the image (net_svc_editor_files.h). Served from here
 * the editor shares the API's origin, so the browser needs no CORS and there is no HTTPS page
 * calling a plain-HTTP device (ADR 0008). Files are looked up by exact name, so no request can
 * reach anything else, and they go out straight from flash-mapped memory with no buffer.
 */
static esp_err_t editor_get(httpd_req_t *req)
{
    /* Everything after "/editor", less any query string: the editor links ?example=<file>. */
    const char *rest     = req->uri + strlen("/editor");
    size_t      rest_len = strcspn(rest, "?");

    if (rest_len == 0) {
        /* The editor's relative stylesheet and module paths only resolve under the slash. */
        httpd_resp_set_status(req, "301 Moved Permanently");
        httpd_resp_set_hdr(req, "Location", "/editor/");
        return httpd_resp_send(req, NULL, 0);
    }
    if (rest[0] != '/') {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }

    const char *path     = rest + 1;
    size_t      path_len = rest_len - 1;
    if (path_len == 0) {
        path     = "index.html";
        path_len = strlen(path);
    }

    for (size_t i = 0; i < net_svc_editor_file_count; i++) {
        const net_svc_editor_file_t *f = &net_svc_editor_files[i];
        if (strlen(f->path) != path_len || strncmp(f->path, path, path_len) != 0) {
            continue;
        }

        httpd_resp_set_type(req, content_type_for(f->path));
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        /* Revalidate every time, so the editor updates with the firmware after an OTA. */
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        return httpd_resp_send(req, (const char *)f->gz, (ssize_t)f->len);
    }

    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

/* -------------------------------------------------------------------- lifecycle ---- */

esp_err_t net_svc_http_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn   = httpd_uri_match_wildcard; /* wildcard config routes */
    cfg.max_uri_handlers = 12;
    /*
     * The server reserves 3 of lwIP's sockets for itself, so this must stay well under
     * CONFIG_LWIP_MAX_SOCKETS or httpd_start() refuses to run. Four concurrent connections
     * is ample: the management page is a single document with no sub-resources.
     */
    cfg.max_open_sockets = 4;
    cfg.stack_size     = 6144; /* OTA and config handling need more than the default */
    cfg.core_id        = 0;    /* keep the HTTP task off the LVGL core */
    cfg.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd_start");

    static const httpd_uri_t routes[] = {
        {.uri = "/",                 .method = HTTP_GET,    .handler = root_get},
        {.uri = "/api/status",       .method = HTTP_GET,    .handler = status_get},
        {.uri = "/api/gauges",       .method = HTTP_GET,    .handler = gauges_get},
        {.uri = "/api/config/*",     .method = HTTP_GET,    .handler = config_get},
        {.uri = "/api/config/*",     .method = HTTP_PUT,    .handler = config_put},
        {.uri = "/api/config/*",     .method = HTTP_DELETE, .handler = config_delete},
        {.uri = "/api/config/*",     .method = HTTP_OPTIONS, .handler = config_options},
        {.uri = "/editor*",          .method = HTTP_GET,    .handler = editor_get},
        {.uri = "/api/wifi",         .method = HTTP_POST,   .handler = wifi_post},
        {.uri = "/api/ota",          .method = HTTP_POST,   .handler = ota_post},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG,
                            "register %s", routes[i].uri);
    }

    ESP_LOGI(TAG, "HTTP server listening on port %d", cfg.server_port);
    return ESP_OK;
}

void net_svc_http_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
