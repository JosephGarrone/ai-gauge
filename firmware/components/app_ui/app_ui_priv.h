/*
 * Internal interface between app_ui.c (tiles, dial, public API) and app_ui_settings.c (the settings
 * tile). Not part of the public API. Everything here runs on the LVGL task.
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#include "app_settings.h"
#include "app_ui.h"
#include "board_profile.h"
#include "gauge_config.h"

/* ---- provided by app_ui.c ---- */

void app_ui_priv_apply_rotation(app_settings_rotation_t rotation);
void app_ui_priv_apply_fps_cap(app_settings_fps_cap_t cap);
void app_ui_priv_show_fps_badge(bool show);

/* ---- provided by app_ui_settings.c ---- */

void settings_build(lv_obj_t *tile, const board_profile_t *board, const gauge_config_t *cfg);

/** Every 500 ms: refreshes whatever live values the open page shows. */
void settings_tick(bool settings_visible);

/** The tileview settled on a tile. Leaving settings closes any open page, freeing its widgets. */
void settings_tile_changed(bool settings_visible);

void settings_set_gauge_info(const gauge_config_t *cfg);
void settings_set_gauge_list(const char (*ids)[GAUGE_CONFIG_MAX_ID_LEN], int count, const char *active);
void settings_set_gauge_selected_cb(app_ui_gauge_selected_cb_t cb);
void settings_set_network_reset_cb(app_ui_network_reset_cb_t cb);
void settings_set_firmware_tap_cb(app_ui_firmware_tap_cb_t cb);
void settings_set_network_status(const char *state, const char *detail);
void settings_set_update_status(const char *text);
void settings_set_warning(const char *text);
