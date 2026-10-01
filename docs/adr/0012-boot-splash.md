# ADR 0012: Boot splash as a flash image, handed over by a brightness dip

**Status:** accepted, 2026-10-01

## Context

The gauge should open with the same branded splash as the obd-display Pi dashboard. The panel is up
~2.6 s after reset and the UI builds in ~110 ms, so there is little to cover. The splash's value is
the look, and it must not cost the dial anything once it has gone.

## Decision

- The scene, with logo and tag line baked in, is one 466×466 RGB565 image generated offline by
  `tools/splash/make_splash.py` and embedded in the app (`EMBED_FILES`). The underline and spark
  are two small ARGB8888 images. The underline is revealed by growing a clipping container.
- Everything sits on `lv_layer_top()` above the UI, which `app_ui_create()` builds underneath as
  usual.
- The hand-over is a dip through black on the panel's own brightness. The splash is deleted at black
  and the dial draws unseen.
- The splash hands over once its animation is done **and** `app_main` releases it. That happens
  after WiFi has initialised and the rear board's ADC has produced a conversion, so the dial opens
  on live values. WiFi keeps its original timing (1 s after the UI) during the splash, and the
  hand-over's full-screen redraw cannot overlap WiFi start-up.
- The panel stays dark until LVGL's first frame. A link-time wrap of `esp_lcd_panel_io_tx_param()`
  drops the BSP's display-on (`0x29`) until then (`main/panel_gate.c`). This fixes a white flash at
  every power-on that predates the splash.

## Alternatives rejected

- **Opacity fade of the scene over the dial.** Built first and measured: ~75 ms a frame (~13 fps),
  visibly stepped. Each frame blends 217k pixels over a full dial redraw.
- **Drawing the background with LVGL** (gradient plus weave) to save flash. It would cost per-frame
  render time on every dirty region, and the weave would be a large number of draw calls.
- **Indexed (I8) or compressed images.** LVGL decodes them into a full-size buffer, which costs the
  same PSRAM transiently and adds decode time before the first frame.
- **Loading the scene from LittleFS.** A ~434 KB read before the first frame delays the splash it is
  meant to show, and still needs a PSRAM buffer.
- **Holding WiFi until the splash had gone.** That was the first version: safe, but it put WiFi
  ~3 s later on every boot to protect only the hand-over frame.
- **Forking the BSP to change its init table** (for the white flash). It is a managed component; a
  fork is one more thing to keep in step with upstream. The wrap touches one command and nothing else.
- **Setting brightness to 0 straight after `bsp_display_start()`.** That is too late: the panel is
  already on, and touch and LVGL setup still run before it returns.
- **A dedicated raw flash partition, memory-mapped.** This is the only option with no PSRAM cost, but
  it changes the partition table, which an OTA update cannot do to gauges already fitted.

## Consequences

- **~440 KB of PSRAM is gone for good.** With `CONFIG_SPIRAM_XIP_FROM_PSRAM`, `.rodata` is copied
  to PSRAM at boot. Measured: 6,505 KB free before the UI is built without the splash, 6,056 KB with
  it. ~5.2 MB stays free after startup. No internal RAM is used.
- WiFi starts when it did before (initialised ~3.8 s after reset). The splash's widgets (~1.5 KB of
  internal RAM) are alive while it does; it still got all 6 static RX buffers on every boot measured.
- If WiFi initialisation ever hangs, the splash hands over on its own 5 s after the underline completes.
- The app image grows by ~450 KB. The 4 MB OTA slot is 43% free afterwards.
