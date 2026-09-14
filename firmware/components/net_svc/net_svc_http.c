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

static const char *TAG = "net_svc_http";

/* Bounds the per-request buffer. Comfortably above the schema's needs. */
#define MAX_BODY_BYTES GAUGE_STORE_MAX_FILE_BYTES
#define OTA_CHUNK      4096

static httpd_handle_t s_server;

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

    char body[512];
    snprintf(body, sizeof(body),
             "{\"version\":\"%s\",\"idf\":\"%s\",\"uptime_s\":%lld,"
             "\"wifi\":{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"hostname\":\"%s\"},"
             "\"heap\":{\"internal\":%u,\"psram\":%u},"
             "\"storage_mounted\":%s}",
             app ? app->version : "unknown", app ? app->idf_ver : "unknown",
             esp_timer_get_time() / 1000000,
             net_svc_state_str(net.state), net.ssid, net.ip, net.rssi, net.hostname,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             gauge_store_mounted() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t gauges_get(httpd_req_t *req)
{
    gauge_store_list_t list;
    gauge_store_list(&list);

    char   body[512];
    size_t used = (size_t)snprintf(body, sizeof(body), "{\"gauges\":[");

    for (int i = 0; i < list.count && used < sizeof(body) - 4; i++) {
        used += (size_t)snprintf(body + used, sizeof(body) - used, "%s\"%s\"",
                                 (i == 0) ? "" : ",", list.ids[i]);
    }
    snprintf(body + used, sizeof(body) - used, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t config_get(httpd_req_t *req)
{
    const char *id = last_segment(req->uri);

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

/* -------------------------------------------------------------------- lifecycle ---- */

esp_err_t net_svc_http_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn   = httpd_uri_match_wildcard; /* wildcard config routes */
    cfg.max_uri_handlers = 10;
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
