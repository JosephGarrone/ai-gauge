/*
 * Firmware updates from GitHub Releases. See net_svc.h, docs/networking.md ("Updates from GitHub")
 * and docs/adr/0010-github-release-updates.md.
 *
 * A check fetches ota.json from the latest release: a few bytes naming the version and its tag,
 * published by release.yml. It goes through the /releases/latest/download/ redirect rather than
 * the REST API, which is rate-limited per address and answers with tens of kilobytes of JSON.
 *
 * An install downloads that tag's ai-gauge.bin whole into PSRAM, checks it, and only then hands it
 * to the HTTP server task to write. Two reasons for the split: flash can only be written from a
 * task whose stack is in internal RAM, and this task's stack, like everything else here, is in
 * PSRAM; and a download that fails halfway leaves the other partition untouched.
 *
 * TLS needs mbedTLS to allocate from PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC). With internal
 * allocation a handshake cannot fit in the few KB of internal RAM left once WiFi runs.
 */

#include "net_svc.h"
#include "net_svc_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net_svc_update";

#define RELEASES_URL "https://github.com/" CONFIG_AI_GAUGE_UPDATE_REPO "/releases"
#define MANIFEST_URL RELEASES_URL "/latest/download/ota.json"
#define APP_ASSET    "ai-gauge.bin"

#define URL_LEN          256
#define MANIFEST_MAX     1024
#define HTTP_TIMEOUT_MS  20000
#define MAX_REDIRECTS    5
#define READ_CHUNK       4096
/*
 * Above the 4KB SPIRAM_MALLOC_ALWAYSINTERNAL threshold on purpose, so the client's buffers come
 * from PSRAM. The transmit side carries the request line, which after GitHub's redirect is a
 * signed URL of several hundred bytes.
 */
#define HTTP_RX_BUFFER   8192
#define HTTP_TX_BUFFER   4352

#define TASK_STACK       10240 /* PSRAM; the TLS handshake is the deep part */
#define TASK_PRIORITY    2
#define POLL_MS          10000
#define FIRST_CHECK_US   (60LL * 1000 * 1000)       /* after the image has confirmed itself */
#define RECHECK_US       (12LL * 3600 * 1000 * 1000)

#if CONFIG_AI_GAUGE_UPDATE_AUTO_CHECK
#define AUTO_CHECK true
#else
#define AUTO_CHECK false
#endif

enum {
    CMD_CHECK   = 1u << 0,
    CMD_INSTALL = 1u << 1,
};

static struct {
    portMUX_TYPE            lock;
    net_svc_update_status_t status; /* guarded by lock */
    char                    tag[NET_SVC_UPDATE_VERSION_LEN]; /* update task only */
    TaskHandle_t            task;
} u = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

#define LOCK()   taskENTER_CRITICAL(&u.lock)
#define UNLOCK() taskEXIT_CRITICAL(&u.lock)

/* ------------------------------------------------------------------- status -------- */

const char *net_svc_update_state_str(net_svc_update_state_t state)
{
    switch (state) {
    case NET_SVC_UPDATE_IDLE:        return "idle";
    case NET_SVC_UPDATE_CHECKING:    return "checking";
    case NET_SVC_UPDATE_UP_TO_DATE:  return "up_to_date";
    case NET_SVC_UPDATE_AVAILABLE:   return "available";
    case NET_SVC_UPDATE_DOWNLOADING: return "downloading";
    case NET_SVC_UPDATE_INSTALLING:  return "installing";
    case NET_SVC_UPDATE_REBOOTING:   return "rebooting";
    case NET_SVC_UPDATE_FAILED:      return "failed";
    default:                         return "unknown";
    }
}

void net_svc_update_get_status(net_svc_update_status_t *out)
{
    LOCK();
    *out = u.status;
    UNLOCK();
}

static void set_state(net_svc_update_state_t state, uint8_t progress)
{
    LOCK();
    u.status.state    = state;
    u.status.progress = progress;
    if (state != NET_SVC_UPDATE_FAILED) {
        u.status.message[0] = '\0';
    }
    UNLOCK();
}

static void set_failed(const char *why)
{
    ESP_LOGW(TAG, "update failed: %s", why);
    LOCK();
    u.status.state    = NET_SVC_UPDATE_FAILED;
    u.status.progress = 0;
    strlcpy(u.status.message, why, sizeof(u.status.message));
    UNLOCK();
}

static void set_progress(uint8_t percent)
{
    LOCK();
    u.status.progress = percent;
    UNLOCK();
}

static bool busy(net_svc_update_state_t state)
{
    return state == NET_SVC_UPDATE_CHECKING || state == NET_SVC_UPDATE_DOWNLOADING ||
           state == NET_SVC_UPDATE_INSTALLING || state == NET_SVC_UPDATE_REBOOTING;
}

static bool connected(void)
{
    net_svc_status_t net;
    net_svc_get_status(&net);
    return net.state == NET_SVC_CONNECTED;
}

/* ------------------------------------------------------------------ versions ------- */

/* "v1.2.3", "1.2.3", "1.2.3-rc1", "1.2.3+sha". False for anything else, such as a bare SHA. */
static bool parse_version(const char *v, long out[3], bool *prerelease)
{
    if (*v == 'v' || *v == 'V') {
        v++;
    }
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*v)) {
            return false;
        }
        char *end = NULL;
        out[i]    = strtol(v, &end, 10);
        v         = end;
        if (i < 2) {
            if (*v != '.') {
                return false;
            }
            v++;
        }
    }
    if (*v != '\0' && *v != '-' && *v != '+') {
        return false;
    }
    *prerelease = (*v == '-');
    return true;
}

/*
 * Whether @p latest should replace @p running. A running version that is not a release number --
 * a local build reports its commit SHA, a CI build "0.0.0-dev+<sha>" -- is older than any
 * release, so a development board can always be brought back onto a release.
 */
static bool is_newer(const char *latest, const char *running)
{
    long l[3], r[3];
    bool l_pre, r_pre;
    if (!parse_version(latest, l, &l_pre)) {
        return false;
    }
    if (!parse_version(running, r, &r_pre)) {
        return true;
    }
    for (int i = 0; i < 3; i++) {
        if (l[i] != r[i]) {
            return l[i] > r[i];
        }
    }
    return r_pre && !l_pre; /* 1.2.3 supersedes 1.2.3-rc1 */
}

/* ---------------------------------------------------------------------- HTTP ------- */

/*
 * Opens @p url and follows GitHub's redirects to the asset host by hand: the streaming API used
 * here does not follow them itself. Returns the open client, or NULL with @p why filled.
 */
static esp_http_client_handle_t open_url(const char *url, int64_t *content_len, char *why,
                                         size_t why_len)
{
    const esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .buffer_size       = HTTP_RX_BUFFER,
        .buffer_size_tx    = HTTP_TX_BUFFER,
        .user_agent        = "ai-gauge",
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        snprintf(why, why_len, "out of memory");
        return NULL;
    }

    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            snprintf(why, why_len, "cannot reach GitHub (%s)", esp_err_to_name(err));
            break;
        }
        int64_t len    = esp_http_client_fetch_headers(client);
        int     status = esp_http_client_get_status_code(client);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_flush_response(client, NULL);
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }
        if (status == 404) {
            snprintf(why, why_len, "not found");
            break;
        }
        if (status != 200) {
            snprintf(why, why_len, "GitHub answered HTTP %d", status);
            break;
        }
        *content_len = len;
        return client;
    }

    if (why[0] == '\0') {
        snprintf(why, why_len, "too many redirects");
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return NULL;
}

static void close_url(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

/* Reads exactly @p len bytes, reporting progress as it goes. */
static bool read_all(esp_http_client_handle_t client, uint8_t *buf, size_t len, bool report)
{
    size_t got = 0;
    while (got < len) {
        size_t want = len - got;
        if (want > READ_CHUNK) {
            want = READ_CHUNK;
        }
        int n = esp_http_client_read(client, (char *)buf + got, (int)want);
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
        if (report) {
            set_progress((uint8_t)(got * 100 / len));
        }
    }
    return true;
}

/* The value of "key":"value" in a flat JSON object. Enough for ota.json, which release.yml writes. */
static bool json_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') {
        p++;
    }
    if (*p++ != '"') {
        return false;
    }
    size_t i = 0;
    while (*p != '\0' && *p != '"' && i + 1 < out_len) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return *p == '"' && i > 0;
}

/* ------------------------------------------------------------------ operations ----- */

static void do_check(void)
{
    set_state(NET_SVC_UPDATE_CHECKING, 0);
    ESP_LOGI(TAG, "checking %s", MANIFEST_URL);

    char    why[NET_SVC_UPDATE_MESSAGE_LEN] = {0};
    int64_t len                             = 0;
    char   *body                            = NULL;

    esp_http_client_handle_t client = open_url(MANIFEST_URL, &len, why, sizeof(why));
    if (client == NULL) {
        if (strcmp(why, "not found") == 0) {
            /* No release carries ota.json yet: nothing to update to, which is not a failure. */
            ESP_LOGI(TAG, "no release published yet");
            LOCK();
            u.status.latest[0] = '\0';
            UNLOCK();
            set_state(NET_SVC_UPDATE_UP_TO_DATE, 0);
        } else {
            set_failed(why);
        }
        return;
    }

    if (len <= 0 || len >= MANIFEST_MAX) {
        close_url(client);
        set_failed("release manifest has a bad size");
        return;
    }
    body = heap_caps_calloc(1, (size_t)len + 1, MALLOC_CAP_SPIRAM);
    if (body == NULL || !read_all(client, (uint8_t *)body, (size_t)len, false)) {
        close_url(client);
        free(body);
        set_failed("could not read the release manifest");
        return;
    }
    close_url(client);

    char version[NET_SVC_UPDATE_VERSION_LEN];
    char tag[NET_SVC_UPDATE_VERSION_LEN];
    bool ok = json_string(body, "version", version, sizeof(version)) &&
              json_string(body, "tag", tag, sizeof(tag));
    free(body);
    if (!ok) {
        set_failed("release manifest is unreadable");
        return;
    }

    const esp_app_desc_t *app   = esp_app_get_description();
    bool                  newer = is_newer(version, app->version);

    strlcpy(u.tag, tag, sizeof(u.tag));
    LOCK();
    strlcpy(u.status.latest, version, sizeof(u.status.latest));
    UNLOCK();
    set_state(newer ? NET_SVC_UPDATE_AVAILABLE : NET_SVC_UPDATE_UP_TO_DATE, 0);

    ESP_LOGI(TAG, "latest release %s (%s); running %s: %s", version, tag, app->version,
             newer ? "update available" : "up to date");
}

static void do_install(void)
{
    char version[NET_SVC_UPDATE_VERSION_LEN];
    LOCK();
    strlcpy(version, u.status.latest, sizeof(version));
    UNLOCK();

    char url[URL_LEN];
    snprintf(url, sizeof(url), RELEASES_URL "/download/%s/" APP_ASSET, u.tag);

    set_state(NET_SVC_UPDATE_DOWNLOADING, 0);
    ESP_LOGW(TAG, "downloading %s", url);

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        set_failed("no OTA partition");
        return;
    }

    char    why[NET_SVC_UPDATE_MESSAGE_LEN] = {0};
    int64_t len                             = 0;

    esp_http_client_handle_t client = open_url(url, &len, why, sizeof(why));
    if (client == NULL) {
        set_failed(strcmp(why, "not found") == 0 ? "release has no " APP_ASSET : why);
        return;
    }
    if (len <= 0 || (uint64_t)len > target->size) {
        close_url(client);
        set_failed("firmware image has a bad size");
        return;
    }

    uint8_t *image = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
    if (image == NULL) {
        close_url(client);
        set_failed("not enough PSRAM for the image");
        return;
    }

    int64_t t0 = esp_timer_get_time();
    bool    ok = read_all(client, image, (size_t)len, true);
    close_url(client);
    if (!ok) {
        free(image);
        set_failed("download interrupted");
        return;
    }
    ESP_LOGI(TAG, "downloaded %lld bytes in %lld ms", (long long)len,
             (long long)((esp_timer_get_time() - t0) / 1000));

    /* The same checks /api/ota makes, plus: the image is the release it claims to be. */
    const esp_app_desc_t *desc = net_svc_ota_image_desc(image, (size_t)len, why, sizeof(why));
    if (desc == NULL) {
        free(image);
        set_failed(why);
        return;
    }
    if (strncmp(desc->version, version, sizeof(desc->version)) != 0) {
        snprintf(why, sizeof(why), "image is %.24s, not %.24s", desc->version, version);
        free(image);
        set_failed(why);
        return;
    }

    set_state(NET_SVC_UPDATE_INSTALLING, 0);
    esp_err_t err = net_svc_http_write_image(image, (size_t)len, set_progress, why, sizeof(why));
    free(image);
    if (err != ESP_OK) {
        set_failed(why);
        return;
    }

    set_state(NET_SVC_UPDATE_REBOOTING, 100);
    ESP_LOGW(TAG, "installed %s; restarting (stack high-water mark %u B)", version,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelay(pdMS_TO_TICKS(1500)); /* long enough for the settings page to say so */
    esp_restart();
}

static void update_task(void *arg)
{
    (void)arg;
    int64_t next_auto_us = FIRST_CHECK_US;

    for (;;) {
        uint32_t cmd = 0;
        xTaskNotifyWait(0, UINT32_MAX, &cmd, pdMS_TO_TICKS(POLL_MS));

        bool due = AUTO_CHECK && esp_timer_get_time() >= next_auto_us;

        if (cmd & CMD_INSTALL) {
            do_install();
        } else if (cmd & CMD_CHECK) {
            do_check();
            next_auto_us = esp_timer_get_time() + RECHECK_US;
        } else if (due && connected()) {
            net_svc_update_status_t st;
            net_svc_update_get_status(&st);
            /* Once one is found there is nothing new to learn until it is installed. */
            if (st.state != NET_SVC_UPDATE_AVAILABLE) {
                do_check();
            }
            next_auto_us = esp_timer_get_time() + RECHECK_US;
        }
    }
}

/* ------------------------------------------------------------------- public -------- */

esp_err_t net_svc_update_check(void)
{
    net_svc_update_status_t st;
    net_svc_update_get_status(&st);
    if (u.task == NULL || busy(st.state) || !connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(u.task, CMD_CHECK, eSetBits);
    return ESP_OK;
}

esp_err_t net_svc_update_install(void)
{
    net_svc_update_status_t st;
    net_svc_update_get_status(&st);
    if (u.task == NULL || st.state != NET_SVC_UPDATE_AVAILABLE || !connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Say so at once, so a second tap or request cannot queue another install behind this one. */
    set_state(NET_SVC_UPDATE_DOWNLOADING, 0);
    xTaskNotify(u.task, CMD_INSTALL, eSetBits);
    return ESP_OK;
}

esp_err_t net_svc_update_start(void)
{
    if (u.task != NULL) {
        return ESP_OK;
    }
    /* Stack and TCB in PSRAM: this costs WiFi no internal memory. It never writes flash itself. */
    if (xTaskCreatePinnedToCoreWithCaps(update_task, "net_update", TASK_STACK, NULL, TASK_PRIORITY,
                                        &u.task, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "could not start the update task");
        u.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "updates from github.com/%s", CONFIG_AI_GAUGE_UPDATE_REPO);
    return ESP_OK;
}
