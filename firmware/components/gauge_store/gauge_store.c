/*
 * Gauge configurations on LittleFS. See gauge_store.h.
 */

#include "gauge_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "gauge_store";

#define MOUNT_POINT     "/storage"
#define PARTITION_LABEL "storage"
#define GAUGE_DIR       MOUNT_POINT "/gauges"
#define XML_SUFFIX      ".xml"

static bool s_mounted;

static void set_err(char *err, size_t err_len, const char *fmt, ...)
{
    if (err == NULL || err_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* Reject anything that could escape the gauges directory or produce a silly filename. */
static bool id_is_safe(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    if (strlen(id) >= GAUGE_CONFIG_MAX_ID_LEN) {
        return false;
    }

    for (const char *p = id; *p != '\0'; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/*
 * Everything this component allocates goes to PSRAM explicitly. Plain malloc() would put
 * anything under SPIRAM_MALLOC_ALWAYSINTERNAL (4KB) -- every config and every parsed
 * gauge_config_t -- into internal RAM, which WiFi needs. And a gauge_config_t is too large for
 * the stacks of the tasks that load one: config uploads had brought the HTTP server's
 * internal-RAM stack to within 668 bytes of overflowing (docs/performance.md).
 */
static gauge_config_t *alloc_config(void)
{
    return heap_caps_malloc(sizeof(gauge_config_t), MALLOC_CAP_SPIRAM);
}

static void path_for(const char *id, char *out, size_t out_len)
{
    snprintf(out, out_len, GAUGE_DIR "/%s" XML_SUFFIX, id);
}

esp_err_t gauge_store_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = MOUNT_POINT,
        .partition_label        = PARTITION_LABEL,
        /*
         * Format on failure: a device that shipped with a blank or corrupt filesystem should
         * come up usable rather than needing a serial cable. There is nothing here the user
         * cannot re-upload, and the built-in face covers the gap either way.
         */
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (continuing with the built-in face)",
                 esp_err_to_name(err));
        s_mounted = false;
        return err;
    }

    s_mounted = true;

    size_t total = 0, used = 0;
    if (esp_littlefs_info(PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "mounted %s: %u KB used of %u KB",
                 MOUNT_POINT, (unsigned)(used / 1024), (unsigned)(total / 1024));
    }

    /* Created on a freshly formatted filesystem so uploads have somewhere to land. */
    if (mkdir(GAUGE_DIR, 0755) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "could not create %s (errno %d)", GAUGE_DIR, errno);
    }

    return ESP_OK;
}

bool gauge_store_mounted(void)
{
    return s_mounted;
}

void gauge_store_list(gauge_store_list_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (!s_mounted) {
        return;
    }

    DIR *dir = opendir(GAUGE_DIR);
    if (dir == NULL) {
        ESP_LOGW(TAG, "cannot open %s", GAUGE_DIR);
        return;
    }

    const size_t suffix_len = strlen(XML_SUFFIX);

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && out->count < GAUGE_STORE_MAX_GAUGES) {
        size_t len = strlen(ent->d_name);
        if (len <= suffix_len ||
            strcmp(ent->d_name + len - suffix_len, XML_SUFFIX) != 0) {
            continue;
        }

        size_t stem_len = len - suffix_len;
        if (stem_len >= GAUGE_CONFIG_MAX_ID_LEN) {
            ESP_LOGW(TAG, "skipping '%s': name too long", ent->d_name);
            continue;
        }

        memcpy(out->ids[out->count], ent->d_name, stem_len);
        out->ids[out->count][stem_len] = '\0';
        out->count++;
    }

    closedir(dir);

    /* Sorted so the picker order does not depend on directory iteration order. */
    for (int i = 1; i < out->count; i++) {
        char key[GAUGE_CONFIG_MAX_ID_LEN];
        snprintf(key, sizeof(key), "%s", out->ids[i]);

        int j = i - 1;
        while (j >= 0 && strcmp(out->ids[j], key) > 0) {
            snprintf(out->ids[j + 1], GAUGE_CONFIG_MAX_ID_LEN, "%s", out->ids[j]);
            j--;
        }
        snprintf(out->ids[j + 1], GAUGE_CONFIG_MAX_ID_LEN, "%s", key);
    }
}

/* Reads a whole file into a NUL-terminated heap buffer. Caller frees. */
static char *read_file(const char *path, size_t *out_len, char *err, size_t err_len)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        set_err(err, err_len, "not found");
        return NULL;
    }

    if (st.st_size <= 0 || st.st_size > GAUGE_STORE_MAX_FILE_BYTES) {
        set_err(err, err_len, "file is %ld bytes (limit %d)",
                (long)st.st_size, GAUGE_STORE_MAX_FILE_BYTES);
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        set_err(err, err_len, "cannot open file");
        return NULL;
    }

    size_t len = (size_t)st.st_size;
    char  *buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        fclose(f);
        set_err(err, err_len, "out of memory");
        return NULL;
    }

    size_t got = fread(buf, 1, len, f);
    fclose(f);

    if (got != len) {
        free(buf);
        set_err(err, err_len, "short read");
        return NULL;
    }

    buf[len] = '\0';
    *out_len = len;
    return buf;
}

esp_err_t gauge_store_load(const char *id, gauge_config_t *cfg, char *err, size_t err_len)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");

    if (!id_is_safe(id)) {
        set_err(err, err_len, "invalid gauge name");
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        set_err(err, err_len, "storage not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    char path[128];
    path_for(id, path, sizeof(path));

    size_t len = 0;
    char  *xml = read_file(path, &len, err, err_len);
    if (xml == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Parsed into a scratch copy so a rejected file leaves *cfg untouched. */
    gauge_config_t *parsed = alloc_config();
    if (parsed == NULL) {
        free(xml);
        set_err(err, err_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    gauge_config_err_t perr = gauge_config_parse(xml, len, parsed);
    free(xml);

    if (perr != GAUGE_CONFIG_OK) {
        free(parsed);
        set_err(err, err_len, "%s", gauge_config_err_str(perr));
        ESP_LOGW(TAG, "'%s' rejected: %s", id, gauge_config_err_str(perr));
        return ESP_ERR_INVALID_ARG;
    }

    *cfg = *parsed;
    free(parsed);

    if (cfg->warning_count > 0) {
        set_err(err, err_len, "loaded with %u adjusted value(s)",
                (unsigned)cfg->warning_count);
        ESP_LOGW(TAG, "'%s' loaded with %u warning(s)", id, cfg->warning_count);
    } else {
        set_err(err, err_len, "%s", "");
        ESP_LOGI(TAG, "loaded '%s': %s %.1f..%.1f %s", id, cfg->source.channel,
                 (double)cfg->source.min, (double)cfg->source.max, cfg->source.unit);
    }

    return ESP_OK;
}

esp_err_t gauge_store_save(const char *id, const char *xml, size_t len,
                           char *err, size_t err_len)
{
    if (!id_is_safe(id) || xml == NULL) {
        set_err(err, err_len, "invalid gauge name");
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        set_err(err, err_len, "storage not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0) {
        len = strlen(xml);
    }
    if (len > GAUGE_STORE_MAX_FILE_BYTES) {
        set_err(err, err_len, "too large (limit %d bytes)", GAUGE_STORE_MAX_FILE_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * Parse before writing. An upload that does not parse must leave the existing config
     * exactly as it was -- replacing it and then discovering the problem would strand a
     * vehicle with a blank gauge.
     */
    gauge_config_t *parsed = alloc_config();
    if (parsed == NULL) {
        set_err(err, err_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    gauge_config_err_t perr = gauge_config_parse(xml, len, parsed);
    free(parsed);
    if (perr != GAUGE_CONFIG_OK) {
        set_err(err, err_len, "%s", gauge_config_err_str(perr));
        return ESP_ERR_INVALID_ARG;
    }

    char path[128];
    path_for(id, path, sizeof(path));

    /* Write to a temporary file and rename, so an interrupted write cannot truncate the
     * config that was already there. */
    char tmp[136];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        set_err(err, err_len, "cannot write file");
        return ESP_FAIL;
    }

    size_t written = fwrite(xml, 1, len, f);
    fclose(f);

    if (written != len) {
        unlink(tmp);
        set_err(err, err_len, "write failed (disk full?)");
        return ESP_FAIL;
    }

    unlink(path); /* rename() will not overwrite on some VFS backends */
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        set_err(err, err_len, "could not replace existing config");
        return ESP_FAIL;
    }

    set_err(err, err_len, "%s", "");
    ESP_LOGI(TAG, "saved '%s' (%u bytes)", id, (unsigned)len);
    return ESP_OK;
}

esp_err_t gauge_store_delete(const char *id)
{
    if (!id_is_safe(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[128];
    path_for(id, path, sizeof(path));

    if (unlink(path) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "deleted '%s'", id);
    return ESP_OK;
}
