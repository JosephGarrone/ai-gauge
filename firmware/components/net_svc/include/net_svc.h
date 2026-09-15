/**
 * @file net_svc.h
 * @brief WiFi, HTTP API, telemetry ingest and OTA.
 *
 * Runs entirely on core 0 so it cannot disturb the LVGL task on core 1
 * (docs/architecture.md). Nothing in the display path waits on the network: `net_svc` starts
 * last, and a device with no WiFi shows its gauge exactly as one with WiFi does.
 *
 * See docs/networking.md for the HTTP API and the telemetry frame format.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_SVC_SSID_LEN 33
#define NET_SVC_IP_LEN   16

typedef enum {
    NET_SVC_DISABLED = 0, /**< Switched off by the user. */
    NET_SVC_CONNECTING,
    NET_SVC_CONNECTED,
    NET_SVC_AP_MODE,      /**< No credentials, or provisioning requested. */
    NET_SVC_FAILED,
} net_svc_state_t;

typedef struct {
    net_svc_state_t state;
    char            ssid[NET_SVC_SSID_LEN];
    char            ip[NET_SVC_IP_LEN];
    char            hostname[NET_SVC_SSID_LEN];
    int8_t          rssi;
} net_svc_status_t;

/**
 * @brief Called when a configuration is uploaded or deleted over HTTP.
 *
 * Invoked from the HTTP task, *not* the LVGL task, so an implementation must not touch LVGL
 * directly -- take the display lock or hand the work to the UI thread.
 *
 * @param id      Configuration that changed.
 * @param deleted True if it was removed rather than written.
 */
typedef void (*net_svc_config_changed_cb_t)(const char *id, bool deleted);

/**
 * @brief Reports a channel value received over the telemetry socket.
 *
 * Called from the telemetry task. Same threading rule as above.
 */
typedef void (*net_svc_telemetry_cb_t)(const char *channel, float value);

typedef struct {
    net_svc_config_changed_cb_t on_config_changed;
    net_svc_telemetry_cb_t      on_telemetry;
} net_svc_callbacks_t;

/**
 * @brief Start networking.
 *
 * Returns as soon as the tasks are running; connection happens in the background and is
 * retried indefinitely. Never blocks the caller waiting for an association.
 */
esp_err_t net_svc_start(const net_svc_callbacks_t *callbacks);

/** @brief Stop networking and release the radio. */
esp_err_t net_svc_stop(void);

/** @brief Current status, safe to call from any task. */
void net_svc_get_status(net_svc_status_t *out);

/**
 * @brief Store credentials and (re)connect.
 *
 * Credentials are persisted only once a connection succeeds, so a typo does not overwrite a
 * working configuration.
 */
esp_err_t net_svc_set_credentials(const char *ssid, const char *password);

/**
 * @brief Drop stored credentials and return to provisioning mode.
 *
 * Disconnects and brings the setup network up immediately, without a reboot. Blocks briefly
 * while the radio restarts.
 */
esp_err_t net_svc_forget_credentials(void);

/** @brief Whether credentials are stored. */
bool net_svc_has_credentials(void);

/** @brief Human-readable form of a state, for the settings page. */
const char *net_svc_state_str(net_svc_state_t state);

/**
 * @brief Record which stored face is on screen, reported by `GET /api/status`.
 *
 * @param id Configuration id, or "" for the built-in face. Safe to call from any task, and
 *           before net_svc_start().
 */
void net_svc_set_active_gauge(const char *id);

#ifdef __cplusplus
}
#endif
