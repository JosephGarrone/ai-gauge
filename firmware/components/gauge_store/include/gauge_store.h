/**
 * @file gauge_store.h
 * @brief Gauge configurations on the LittleFS `storage` partition.
 *
 * Keeps filesystem concerns out of `gauge_config`, which stays a pure parser with no
 * ESP-IDF dependency so it can be tested on a host (docs/architecture.md).
 *
 * Every failure path here degrades rather than propagating: an unmountable filesystem or an
 * unparseable file must never be the reason a gauge does not display.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "gauge_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAUGE_STORE_MAX_GAUGES 12

/** Largest configuration file accepted. Generous for the schema; bounds the read buffer. */
#define GAUGE_STORE_MAX_FILE_BYTES 16384

/** Length of the diagnostic message gauge_store_load() writes. */
#define GAUGE_STORE_ERR_LEN 96

typedef struct {
    char ids[GAUGE_STORE_MAX_GAUGES][GAUGE_CONFIG_MAX_ID_LEN];
    int  count;
} gauge_store_list_t;

/**
 * @brief Mount the storage partition.
 *
 * Returns an error if the filesystem is unusable, but the caller is expected to carry on
 * with the built-in face rather than abort. Check gauge_store_mounted() afterwards.
 */
esp_err_t gauge_store_init(void);

/** @brief Whether the filesystem is available. False means only the built-in face exists. */
bool gauge_store_mounted(void);

/**
 * @brief List the configurations present, by id.
 *
 * Ids are the filename stems of `*.xml` in the gauges directory, sorted so the picker order
 * is stable across boots. Yields an empty list when the filesystem is unavailable.
 */
void gauge_store_list(gauge_store_list_t *out);

/**
 * @brief Load and parse one configuration.
 *
 * @param id       Configuration id (filename stem).
 * @param[out] cfg Populated on success; untouched on failure.
 * @param[out] err Optional buffer of at least GAUGE_STORE_ERR_LEN for a message suitable to
 *                 show a user. A parse failure names the reason, so a config that "almost
 *                 works" is diagnosable without a serial cable.
 * @param err_len  Size of @p err.
 */
esp_err_t gauge_store_load(const char *id, gauge_config_t *cfg, char *err, size_t err_len);

/**
 * @brief Read one configuration's XML exactly as stored, for loading back into the face editor.
 *
 * Not parsed: a file the gauge rejects is still returned, so it can be fixed.
 *
 * @param id       Configuration id (filename stem).
 * @param[out] len Length of the file in bytes.
 * @param[out] err Optional buffer for a message, as for gauge_store_load().
 * @param err_len  Size of @p err.
 * @return A NUL-terminated buffer in PSRAM that the caller frees, or NULL on failure.
 */
char *gauge_store_read_xml(const char *id, size_t *len, char *err, size_t err_len);

/**
 * @brief Write a configuration, replacing any existing one with the same id.
 *
 * Validates before replacing: the XML is parsed first, and an existing file is left untouched
 * if it does not parse. That is what stops a bad upload leaving a vehicle with a blank gauge.
 *
 * Also refused, so that every stored face is listed and addressed by its own id:
 * - a file whose `<gauge id>` differs from @p id
 * - a new id once GAUGE_STORE_MAX_GAUGES faces are stored (replacing one is always allowed)
 */
esp_err_t gauge_store_save(const char *id, const char *xml, size_t len,
                           char *err, size_t err_len);

/** @brief Remove a configuration. */
esp_err_t gauge_store_delete(const char *id);

#ifdef __cplusplus
}
#endif
