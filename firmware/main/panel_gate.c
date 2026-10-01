/*
 * Keeps the panel dark until LVGL has drawn a real frame (docs/boot-splash.md).
 *
 * The BSP's CO5300 init table ends with DISPON (0x29) at full brightness, and bsp_display_new()
 * sends DISPON again through esp_lcd_panel_disp_on_off(). From then until app_main has built
 * anything, the panel shows its uninitialised memory and then LVGL's default screen, which is
 * white in the light theme: a white flash at every power-on. Neither the BSP nor the driver offers
 * a hook, so the link wraps esp_lcd_panel_io_tx_param() (main/CMakeLists.txt) and drops DISPON
 * until panel_gate_open(). Every other command, and every pixel write, passes straight through, so
 * the rest of the init sequence and LVGL's first frame land in panel memory as usual.
 *
 * Over QSPI the CO5300 driver sends a command as opcode 0x02 in bits 31..24 and the DCS command in
 * bits 15..8 (tx_param() in esp_lcd_co5300_spi.c).
 */
#include "panel_gate.h"

#include <stdatomic.h>

#include "esp_lcd_panel_io.h"
#include "esp_log.h"

static const char *TAG = "panel_gate";

#define QSPI_CMD(dcs) ((int)((0x02u << 24) | ((unsigned)(dcs) << 8)))
#define DCS_DISPON    0x29

static atomic_bool                      s_open;
static esp_lcd_panel_io_handle_t _Atomic s_io;

esp_err_t __real_esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param,
                                           size_t param_size);

esp_err_t __wrap_esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param,
                                           size_t param_size)
{
    if (lcd_cmd == QSPI_CMD(DCS_DISPON) && !atomic_load(&s_open)) {
        atomic_store(&s_io, io);
        return ESP_OK;
    }
    return __real_esp_lcd_panel_io_tx_param(io, lcd_cmd, param, param_size);
}

void panel_gate_open(void)
{
    if (atomic_exchange(&s_open, true)) {
        return;
    }
    esp_lcd_panel_io_handle_t io = atomic_load(&s_io);
    if (io == NULL) {
        /* Nothing was held back, so the panel is already on (or never was): nothing to undo. */
        return;
    }
    esp_err_t err = __real_esp_lcd_panel_io_tx_param(io, QSPI_CMD(DCS_DISPON), NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display on failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "display on, first frame drawn");
    }
}
