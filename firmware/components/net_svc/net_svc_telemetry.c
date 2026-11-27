/*
 * UDP telemetry ingest, and the OBD Display link. See docs/networking.md for both frame formats.
 *
 * UDP is deliberate: telemetry is a stream of perishable values, so dropping a datagram is
 * strictly better than delaying the stream to retransmit one. A late value on a gauge is
 * worse than a missing one.
 *
 * The link (docs/networking.md, "The OBD Display link"; ADR 0015): the display broadcasts a beacon
 * once a second to this port. Each beacon names its sender, so the gauge answers it, from this
 * socket, ten times a second while beacons keep arriving: who it is, the channel its face shows
 * and its own sensor readings (built by the owner's link_report callback). The display then sends
 * that channel here as ordinary telemetry frames. Nothing is configured on either side.
 */

#include "net_svc.h"
#include "net_svc_priv.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "net_svc_telem";

#define TELEMETRY_PORT 5005
#define DATAGRAM_MAX   256
#define REPORT_MAX     384
#define CHANNEL_MAX    32

/* How often the gauge reports to the display, and how long a beacon keeps it reporting. */
#define LINK_REPORT_US (100 * 1000)
#define LINK_FRESH_US  (5 * 1000 * 1000)

/*
 * The receive timeout is also the report clock's resolution: short enough that reports go out
 * close to every LINK_REPORT_US, long enough that an idle gauge wakes only ten times a second.
 */
#define RECV_TIMEOUT_US (100 * 1000)

/*
 * Stack and TCB in PSRAM, like the update task's: this task never writes flash, and internal RAM is
 * the board's scarcest resource (docs/performance.md). Formatting the report's floats wants more
 * than the 3KB of internal RAM this task had when it only parsed.
 */
#define TASK_STACK 4096

static TaskHandle_t  s_task;
static int           s_sock = -1;
static volatile bool s_running;

/* The display that last sent a beacon. Only the telemetry task touches these. */
static struct sockaddr_in s_peer;
static int64_t            s_peer_at;
static int64_t            s_report_at;

/*
 * Where a key's value starts in a small flat JSON object: just past the colon and any spaces, or
 * NULL. Good enough for the frames here, which have no nesting and no key inside a string value.
 */
static const char *json_value(const char *json, const char *key)
{
    char quoted[24];
    int  n = snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(quoted)) {
        return NULL;
    }
    const char *p = strstr(json, quoted);
    if (p == NULL) {
        return NULL;
    }
    p += n;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != ':') {
        return NULL;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

/* True if the object's "t" is the given string. */
static bool json_type_is(const char *json, const char *type)
{
    const char *v = json_value(json, "t");
    size_t      n = strlen(type);
    return v != NULL && v[0] == '"' && strncmp(v + 1, type, n) == 0 && v[1 + n] == '"';
}

/*
 * Pulls "ch" and "v" out of a small flat JSON object. A full JSON parser is available in
 * ESP-IDF, but this runs per datagram at telemetry rates and the frame is fixed and tiny --
 * see docs/networking.md. Anything malformed is dropped rather than guessed at.
 */
static bool parse_frame(const char *json, char *channel, size_t channel_len, float *value)
{
    const char *ch = strstr(json, "\"ch\"");
    const char *v  = strstr(json, "\"v\"");
    if (ch == NULL || v == NULL) {
        return false;
    }

    ch = strchr(ch + 4, '"');
    if (ch == NULL) {
        return false;
    }
    ch++;

    size_t i = 0;
    while (*ch != '\0' && *ch != '"' && i + 1 < channel_len) {
        channel[i++] = *ch++;
    }
    channel[i] = '\0';
    if (i == 0 || *ch != '"') {
        return false;
    }

    v = strchr(v + 3, ':');
    if (v == NULL) {
        return false;
    }

    char  *end = NULL;
    double d   = strtod(v + 1, &end);
    /* strtod() accepts "nan" and "inf", which JSON does not and no gauge can show. */
    if (end == v + 1 || !isfinite(d)) {
        return false;
    }

    *value = (float)d;
    return true;
}

/* A beacon: note who sent it, and pass on its brightness (-1 when it carries none). */
static void on_beacon(const char *json, const struct sockaddr_in *src)
{
    s_peer    = *src;
    s_peer_at = esp_timer_get_time();

    int         bright = -1;
    const char *b      = json_value(json, "bright");
    if (b != NULL) {
        char *end = NULL;
        long  v   = strtol(b, &end, 10);
        if (end != b && v >= 0 && v <= 100) {
            bright = (int)v;
        }
    }

    const net_svc_callbacks_t *cb = net_svc_get_callbacks();
    if (cb->on_link_beacon != NULL) {
        cb->on_link_beacon(bright);
    }
}

/* One report to the display that sent the last beacon, if it is still beaconing and one is due. */
static void maybe_report(void)
{
    int64_t now = esp_timer_get_time();
    if (s_peer_at == 0 || now - s_peer_at > LINK_FRESH_US || now - s_report_at < LINK_REPORT_US) {
        return;
    }
    s_report_at = now;

    net_svc_status_t st;
    net_svc_get_status(&st);

    /* On this task's PSRAM stack; never a static, which would be internal RAM. */
    char buf[REPORT_MAX];
    int  len = snprintf(buf, sizeof(buf), "{\"t\":\"gauge\",\"ver\":1,\"host\":\"%s\",\"fw\":\"%s\"",
                        st.hostname, esp_app_get_description()->version);
    if (len <= 0 || len >= (int)sizeof(buf) - 2) {
        return;
    }

    const net_svc_callbacks_t *cb = net_svc_get_callbacks();
    if (cb->link_report != NULL) {
        /* Room is kept for the closing brace and terminator. */
        size_t room  = sizeof(buf) - (size_t)len - 2;
        size_t added = cb->link_report(buf + len, room);
        if (added < room) {
            len += (int)added;
        }
    }
    buf[len++] = '}';
    buf[len]   = '\0';

    if (sendto(s_sock, buf, (size_t)len, 0, (struct sockaddr *)&s_peer, sizeof(s_peer)) < 0) {
        ESP_LOGD(TAG, "report not sent (errno %d)", errno);
    }
}

static void telemetry_task(void *arg)
{
    (void)arg;

    char buf[DATAGRAM_MAX];

    while (s_running) {
        struct sockaddr_in src;
        socklen_t          src_len = sizeof(src);

        int n = recvfrom(s_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &src_len);
        if (n < 0) {
            if (!s_running) {
                break;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            maybe_report();
            continue;
        }

        buf[n] = '\0';

        if (json_type_is(buf, "obd")) {
            on_beacon(buf, &src);
        } else {
            char  channel[CHANNEL_MAX];
            float value;
            if (parse_frame(buf, channel, sizeof(channel), &value)) {
                const net_svc_callbacks_t *cb = net_svc_get_callbacks();
                if (cb->on_telemetry != NULL) {
                    cb->on_telemetry(channel, value);
                }
            } else {
                ESP_LOGD(TAG, "dropped malformed datagram (%d bytes)", n);
            }
        }
        maybe_report();
    }

    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }

    s_task = NULL;
    /* A PSRAM-stack task deleting itself: IDF hands the freeing to a short-lived helper task. */
    vTaskDeleteWithCaps(NULL);
}

esp_err_t net_svc_telemetry_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    ESP_RETURN_ON_FALSE(s_sock >= 0, ESP_FAIL, TAG, "socket failed (errno %d)", errno);

    /* A timeout lets the task notice s_running going false, and keeps the report clock running. */
    struct timeval tv = {.tv_sec = 0, .tv_usec = RECV_TIMEOUT_US};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(TELEMETRY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind to port %d failed (errno %d)", TELEMETRY_PORT, errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    s_running = true;
    s_peer_at = 0;

    /* Core 0, so it cannot compete with LVGL on core 1 (docs/architecture.md). */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(telemetry_task, "telemetry", TASK_STACK, NULL, 4,
                                                    &s_task, 0, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        s_running = false;
        s_task    = NULL;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "listening for telemetry and the display's beacon on UDP %d", TELEMETRY_PORT);
    return ESP_OK;
}

void net_svc_telemetry_stop(void)
{
    s_running = false;
    /* The task closes the socket and deletes itself once its receive times out. */
}
