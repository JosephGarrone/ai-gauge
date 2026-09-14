/*
 * WiFi lifecycle and orchestration. See net_svc.h and docs/networking.md.
 *
 * Everything here runs on core 0. The connection is attempted in the background and retried
 * indefinitely with backoff, so nothing the caller does ever blocks on the radio.
 */

#include "net_svc.h"
#include "net_svc_priv.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"

static const char *TAG = "net_svc";

#define NVS_NAMESPACE "net_svc"
#define KEY_SSID      "ssid"
#define KEY_PASS      "pass"

#define AP_SSID_PREFIX "ai-gauge-setup"
#define AP_CHANNEL     1
#define AP_MAX_CONN    4

#define RETRY_MIN_MS 2000
#define RETRY_MAX_MS 30000

static struct {
    bool started;
    bool netif_ready;

    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;

    net_svc_status_t status;
    net_svc_callbacks_t cb;

    esp_timer_handle_t retry_timer;
    uint32_t           retry_delay_ms;

    /* Credentials awaiting proof that they work, so a typo cannot overwrite a good one. */
    char pending_ssid[NET_SVC_SSID_LEN];
    char pending_pass[64];
    bool have_pending;
} s;

const char *net_svc_state_str(net_svc_state_t state)
{
    switch (state) {
    case NET_SVC_DISABLED:   return "off";
    case NET_SVC_CONNECTING: return "connecting";
    case NET_SVC_CONNECTED:  return "connected";
    case NET_SVC_AP_MODE:    return "setup mode";
    case NET_SVC_FAILED:     return "failed";
    default:                 return "unknown";
    }
}

void net_svc_get_status(net_svc_status_t *out)
{
    if (out != NULL) {
        *out = s.status;
    }
}

const net_svc_callbacks_t *net_svc_get_callbacks(void)
{
    return &s.cb;
}

/* ------------------------------------------------------------------ credentials ---- */

static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    size_t len = ssid_len;
    bool   ok  = (nvs_get_str(h, KEY_SSID, ssid, &len) == ESP_OK) && ssid[0] != '\0';

    if (ok) {
        len = pass_len;
        if (nvs_get_str(h, KEY_PASS, pass, &len) != ESP_OK) {
            pass[0] = '\0'; /* open network */
        }
    }

    nvs_close(h);
    return ok;
}

static esp_err_t store_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs_open");

    esp_err_t err = nvs_set_str(h, KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_PASS, pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

bool net_svc_has_credentials(void)
{
    char ssid[NET_SVC_SSID_LEN] = {0};
    char pass[64]               = {0};
    return load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
}

esp_err_t net_svc_forget_credentials(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs_open");
    nvs_erase_key(h, KEY_SSID);
    nvs_erase_key(h, KEY_PASS);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "credentials cleared");
    return err;
}

/* ------------------------------------------------------------------ connection ----- */

static void start_ap_mode(void);

static void retry_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "retrying connection");
    esp_wifi_connect();
}

static void schedule_retry(void)
{
    /* Exponential backoff so a missing network does not hammer the radio forever. */
    if (s.retry_delay_ms == 0) {
        s.retry_delay_ms = RETRY_MIN_MS;
    } else {
        s.retry_delay_ms *= 2;
        if (s.retry_delay_ms > RETRY_MAX_MS) {
            s.retry_delay_ms = RETRY_MAX_MS;
        }
    }

    esp_timer_stop(s.retry_timer);
    esp_timer_start_once(s.retry_timer, (uint64_t)s.retry_delay_ms * 1000);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            s.status.state = NET_SVC_CONNECTING;
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            ESP_LOGW(TAG, "disconnected from '%s' (reason %d)", s.status.ssid, d->reason);

            s.status.ip[0] = '\0';
            s.status.rssi  = 0;

            /*
             * Credentials only prove themselves by reaching a connection. If a set of
             * pending credentials fails, fall back to provisioning rather than silently
             * retrying something that does not work.
             */
            if (s.have_pending) {
                ESP_LOGW(TAG, "new credentials for '%s' did not connect", s.pending_ssid);
                s.have_pending = false;
                start_ap_mode();
                break;
            }

            s.status.state = NET_SVC_CONNECTING;
            schedule_retry();
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "a device joined the setup network");
            break;

        default:
            break;
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;

        snprintf(s.status.ip, sizeof(s.status.ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s.status.state   = NET_SVC_CONNECTED;
        s.retry_delay_ms = 0;

        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s.status.rssi = ap.rssi;
            snprintf(s.status.ssid, sizeof(s.status.ssid), "%s", (const char *)ap.ssid);
        }

        ESP_LOGI(TAG, "connected to '%s' as %s", s.status.ssid, s.status.ip);

        /* Now that they demonstrably work, keep them. */
        if (s.have_pending) {
            store_credentials(s.pending_ssid, s.pending_pass);
            s.have_pending = false;
            ESP_LOGI(TAG, "credentials saved");
        }
    }
}

static void set_hostname(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    /*
     * The MAC suffix keeps two gauges on one network from colliding. Documented in
     * docs/networking.md so the name is predictable rather than a surprise.
     */
    snprintf(s.status.hostname, sizeof(s.status.hostname), "ai-gauge-%02x%02x", mac[4], mac[5]);
}

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable; reach the device by IP instead");
        return;
    }

    mdns_hostname_set(s.status.hostname);
    mdns_instance_name_set("AI-Gauge");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI(TAG, "mDNS: %s.local", s.status.hostname);
}

static void start_ap_mode(void)
{
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", AP_SSID_PREFIX);
    ap.ap.ssid_len       = strlen(AP_SSID_PREFIX);
    ap.ap.channel        = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONN;
    /*
     * Open network. Provisioning has to be reachable by someone who has no credentials yet,
     * and the alternative is a shared default password that is no better. It is only up
     * while the device is unprovisioned -- see the security note in docs/networking.md.
     */
    ap.ap.authmode = WIFI_AUTH_OPEN;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();

    s.status.state = NET_SVC_AP_MODE;
    snprintf(s.status.ssid, sizeof(s.status.ssid), "%s", AP_SSID_PREFIX);
    snprintf(s.status.ip, sizeof(s.status.ip), "192.168.4.1");

    ESP_LOGW(TAG, "no working credentials: setup network '%s' is up at %s",
             AP_SSID_PREFIX, s.status.ip);
}

static esp_err_t start_sta_mode(const char *ssid, const char *pass)
{
    wifi_config_t sta = {0};
    snprintf((char *)sta.sta.ssid, sizeof(sta.sta.ssid), "%s", ssid);
    snprintf((char *)sta.sta.password, sizeof(sta.sta.password), "%s", pass ? pass : "");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");

    snprintf(s.status.ssid, sizeof(s.status.ssid), "%s", ssid);
    s.status.state = NET_SVC_CONNECTING;

    ESP_LOGI(TAG, "connecting to '%s'", ssid);
    return ESP_OK;
}

esp_err_t net_svc_set_credentials(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "empty ssid");

    snprintf(s.pending_ssid, sizeof(s.pending_ssid), "%s", ssid);
    snprintf(s.pending_pass, sizeof(s.pending_pass), "%s", password ? password : "");
    s.have_pending   = true;
    s.retry_delay_ms = 0;

    esp_timer_stop(s.retry_timer);
    esp_wifi_disconnect();
    esp_wifi_stop();

    return start_sta_mode(s.pending_ssid, s.pending_pass);
}

/* --------------------------------------------------------------------- lifecycle --- */

esp_err_t net_svc_start(const net_svc_callbacks_t *callbacks)
{
    ESP_RETURN_ON_FALSE(!s.started, ESP_ERR_INVALID_STATE, TAG, "already started");

    if (callbacks != NULL) {
        s.cb = *callbacks;
    }

    if (!s.netif_ready) {
        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif_init");

        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(err, TAG, "event_loop");
        }

        s.sta_netif = esp_netif_create_default_wifi_sta();
        s.ap_netif  = esp_netif_create_default_wifi_ap();
        s.netif_ready = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi_init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL),
                        TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL),
                        TAG, "ip events");

    /* Credentials in NVS survive a reflash, so the radio never needs flash write access. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi_set_storage");

    set_hostname();
    if (s.sta_netif != NULL) {
        esp_netif_set_hostname(s.sta_netif, s.status.hostname);
    }

    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name     = "net_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s.retry_timer), TAG, "retry timer");

    char ssid[NET_SVC_SSID_LEN] = {0};
    char pass[64]               = {0};

    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_RETURN_ON_ERROR(start_sta_mode(ssid, pass), TAG, "sta mode");
    } else {
        start_ap_mode();
    }

    start_mdns();
    net_svc_http_start();
    net_svc_telemetry_start();

    s.started = true;
    ESP_LOGI(TAG, "started (%s)", net_svc_state_str(s.status.state));
    return ESP_OK;
}

esp_err_t net_svc_stop(void)
{
    if (!s.started) {
        return ESP_OK;
    }

    net_svc_telemetry_stop();
    net_svc_http_stop();
    mdns_free();

    if (s.retry_timer != NULL) {
        esp_timer_stop(s.retry_timer);
        esp_timer_delete(s.retry_timer);
        s.retry_timer = NULL;
    }

    esp_wifi_stop();
    esp_wifi_deinit();

    s.started      = false;
    s.status.state = NET_SVC_DISABLED;
    s.status.ip[0] = '\0';

    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}
