/*
 * Internal interface between the net_svc translation units. Not part of the public API.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_err.h"

#include "net_svc.h"

/** Accessor for the callbacks registered with net_svc_start(). Never NULL. */
const net_svc_callbacks_t *net_svc_get_callbacks(void);

esp_err_t net_svc_http_start(void);
void      net_svc_http_stop(void);

esp_err_t net_svc_telemetry_start(void);
void      net_svc_telemetry_stop(void);

esp_err_t net_svc_update_start(void);

/**
 * The app description inside an image's first bytes, or NULL (with @p why filled) if the image is
 * not an app built from this project. Shared by both OTA paths.
 */
const esp_app_desc_t *net_svc_ota_image_desc(const uint8_t *head, size_t len, char *why,
                                             size_t why_len);

/** Receives install progress, 0-100. Called on the HTTP server task. */
typedef void (*net_svc_ota_progress_cb_t)(uint8_t percent);

/**
 * Write a complete app image to the next OTA partition and make it the boot partition.
 *
 * Flash can only be written from a task whose stack is in internal RAM, so the write runs on the
 * HTTP server task (whose stack is) through httpd_queue_work(); the caller blocks until it is done
 * and may have a PSRAM stack. @p image may be in PSRAM. Does not restart.
 */
esp_err_t net_svc_http_write_image(const uint8_t *image, size_t len,
                                   net_svc_ota_progress_cb_t progress, char *why, size_t why_len);
