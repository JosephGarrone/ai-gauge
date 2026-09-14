/*
 * UDP telemetry ingest. See docs/networking.md for the frame format.
 *
 * UDP is deliberate: telemetry is a stream of perishable values, so dropping a datagram is
 * strictly better than delaying the stream to retransmit one. A late value on a gauge is
 * worse than a missing one.
 */

#include "net_svc.h"
#include "net_svc_priv.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "net_svc_telem";

#define TELEMETRY_PORT 5005
#define DATAGRAM_MAX   256

static TaskHandle_t s_task;
static int          s_sock = -1;
static volatile bool s_running;

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
    if (end == v + 1) {
        return false;
    }

    *value = (float)d;
    return true;
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
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        buf[n] = '\0';

        char  channel[32];
        float value;
        if (!parse_frame(buf, channel, sizeof(channel), &value)) {
            ESP_LOGD(TAG, "dropped malformed datagram (%d bytes)", n);
            continue;
        }

        const net_svc_callbacks_t *cb = net_svc_get_callbacks();
        if (cb->on_telemetry != NULL) {
            cb->on_telemetry(channel, value);
        }
    }

    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t net_svc_telemetry_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    ESP_RETURN_ON_FALSE(s_sock >= 0, ESP_FAIL, TAG, "socket failed (errno %d)", errno);

    /* A timeout lets the task notice s_running going false instead of blocking forever. */
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
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

    /* Core 0, so it cannot compete with LVGL on core 1 (docs/architecture.md). */
    BaseType_t ok = xTaskCreatePinnedToCore(telemetry_task, "telemetry", 3072, NULL, 4,
                                            &s_task, 0);
    if (ok != pdPASS) {
        s_running = false;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "listening for telemetry on UDP %d", TELEMETRY_PORT);
    return ESP_OK;
}

void net_svc_telemetry_stop(void)
{
    s_running = false;
    /* The task closes the socket and deletes itself once its receive times out. */
}
