/*
 * The face editor's files, gzipped into the application image so the gauge can serve the editor
 * at /editor/ (ADR 0008). The table is generated at build time by editor_bundle.cmake from
 * tools/config-app, so the editor always matches the firmware it ships with, OTA included.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char    *path; /* relative to /editor/, e.g. "js/app.js" */
    const uint8_t *gz;   /* gzip stream, sent as-is with Content-Encoding: gzip */
    size_t         len;
} net_svc_editor_file_t;

extern const net_svc_editor_file_t net_svc_editor_files[];
extern const size_t                net_svc_editor_file_count;
