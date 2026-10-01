# Boot splash

![The boot splash, underline complete](images/boot-splash.png)

The obd-display Pi boot splash (`obd-display/scripts/pi/plymouth/`), recut for the round panel: the
Ford logo on a dark navy carbon-weave background, *Giuseppe Spec Mods* in Ford Antenna under it, and
a cyan underline that draws itself under the tag line led by a spark. Palette and weave are the Pi's.
The vignette into the panel's edge is new, so the rim reads as a bezel rather than a crop.

Decision record: [ADR 0012](adr/0012-boot-splash.md).

## Sequence

| Time from splash on | What happens |
|---|---|
| 0 | Splash created right after `bsp_display_start()` (~2.6 s after reset; panel init is ~1.5 s of that). The UI is built underneath it. The panel is still dark (below) |
| ~0.07 s | First frame flushed; the panel switches on, showing the splash |
| 0.3 s | Underline sweeps left to right, eased out, over 1.4 s, spark on its leading edge. Meanwhile `sensor_hub` starts, and WiFi initialises 1 s after the UI, as it did before the splash existed |
| 1.7 s | Spark fades (0.3 s), then the complete line holds for at least 0.5 s, and until `app_main` releases the splash |
| ≥2.5 s | Hand-over: panel brightness dips to black (0.25 s), the splash is deleted, the dial draws unseen (60 ms), brightness returns to the user's setting (0.3 s) |

**Release.** `app_main` calls `app_ui_splash_release()` once WiFi has initialised and the rear
board's ADC has produced a conversion, so the dial opens on live values. With no board answering it
does not wait for one; a board that answers but never converts is waited on for 1 s. If release
never comes, the splash hands over 5 s after the underline completes. In practice the board is
reading long before the animation ends, so the animation sets the length.

A tap anywhere skips the animation to the complete underline, but the hand-over still waits for
release. The splash takes all touch while it is up, so nothing reaches the dial underneath.

## No white flash at power-on

The BSP switches the panel on at full brightness at the end of its init, with panel memory
uninitialised, and LVGL's first screen is white in the light theme. Both showed for a moment at
every power-on, splash or not. `firmware/main/panel_gate.c` wraps
`esp_lcd_panel_io_tx_param()` at link time (`-Wl,--wrap`, `main/CMakeLists.txt`) and drops the
display-on command (`0x29`) until LVGL reports its first refresh (`LV_EVENT_REFR_READY`), then sends
it. Everything else passes through. The command encoding is in
[hardware-reference.md](hardware-reference.md#display--co5300-qspi-amoled-466466).

## Where things are

| | |
|---|---|
| `tools/splash/make_splash.py` | Generates the images. Edit this, not its outputs. Needs Pillow and numpy |
| `tools/splash/ford-logo.png`, `FordAntenna-Regular.otf` | Inputs, copied from obd-display |
| `firmware/components/app_ui/splash/` | Generated: `scene.bin` (466×466 RGB565), `bar.bin` and `spark.bin` (ARGB8888), `splash_assets.h` (positions) |
| `firmware/components/app_ui/app_ui_splash.c` | Shows and animates it. Timings are the `#define`s at the top |
| `firmware/main/panel_gate.c` | Keeps the panel dark until the first frame |
| `docs/images/boot-splash.png` | Preview, written by the generator |

To change the look, edit the generator, run `python tools/splash/make_splash.py`, rebuild, and
commit the outputs. The firmware build embeds the `.bin` files (`EMBED_FILES`), so CI does not
need Pillow.

## Rules

- **The scene is quantised to RGB565 with an ordered dither.** A dark navy gradient bands visibly in
  16-bit colour without it.
- **Only the bar and spark may animate.** The scene is blitted once; each sweep frame dirties only
  the strip the bar and spark cover (5% of the panel on average, measured).
- **Do not fade the scene with opacity.** A full-screen blend over the dial measured ~75 ms a frame.
  The panel-brightness dip costs no rendering.
- **The hand-over must not run during WiFi initialisation.** It redraws the whole screen, and
  full-screen frames during WiFi start-up cost RX buffers ([performance.md](performance.md)). That
  is why release comes after `net_svc_start()` returns.
- **The splash costs ~440 KB of PSRAM, permanently.** `CONFIG_SPIRAM_XIP_FROM_PSRAM` copies all
  `.rodata` into PSRAM at boot, so the embedded images occupy PSRAM even after the splash is gone.
  A bigger or second scene costs the same again. It costs no internal RAM.
- On a panel other than 466×466 the scene is centred on black. Regenerate at that size for a proper
  fit (`SIZE` in the generator).

## Verification

On hardware (ESP32-S3 rev v0.2, frame cap set to 45 fps, rear board attached), 2026-10-01.
Numbers are in [performance.md](performance.md#boot-splash). Over three boots: the display-on was
held and sent after the first frame, WiFi initialised all 6 static RX buffers during the splash, and
the ADC had ~100 MAP samples by release. The splash hands over to a clean dial.
**Not yet verified:** that the white flash is gone, which has to be judged by eye (screenshots show
what LVGL drew, not the panel), and tap-to-skip, which needs a hand on the screen.
