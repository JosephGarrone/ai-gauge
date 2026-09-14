/*
 * Internal interface between the net_svc translation units. Not part of the public API.
 */
#pragma once

#include "esp_err.h"

#include "net_svc.h"

/** Accessor for the callbacks registered with net_svc_start(). Never NULL. */
const net_svc_callbacks_t *net_svc_get_callbacks(void);

esp_err_t net_svc_http_start(void);
void      net_svc_http_stop(void);

esp_err_t net_svc_telemetry_start(void);
void      net_svc_telemetry_stop(void);
