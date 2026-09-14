# Display Pipeline and the Frame Budget

The 60fps+ target is the hardest constraint in this project. This document explains where the
budget goes and the rules that protect it.

## The budget

At 60fps a frame is **16.6ms**. Two costs compete for it: rendering pixels on the CPU, and
pushing them over QSPI to the panel.

**Transfer cost.** A full 466×466 RGB565 frame is `466 × 466 × 2 = 434,312 bytes`. QSPI at
80MHz across 4 data lanes is ~40MB/s theoretical, so a full-frame blit is **~11ms** — about
two-thirds of the budget, before a single pixel has been rendered. Real-world overhead makes
it worse.

**The conclusion that shapes the whole renderer:** a design that redraws the full screen every
frame cannot hit 60fps on this hardware. Frame rate is therefore governed by *dirty region
size*, and the renderer's job is to keep that region small.

## How the gauge stays cheap

### 1. The dial face is pre-rendered once

The bezel, colour bands, tick marks and numeric labels are static for a given configuration.
At boot — and again only when the config changes — they are rasterised from the gauge XML
into a full-frame RGB565 canvas held in PSRAM, which becomes the screen background.

Steady-state cost of all that artwork: **zero**. This is the single biggest win, and it is why
an expensive-looking gauge face costs no more per frame than a plain one.

### 2. Only the needle moves, and only its exact footprint is repainted

The needle is drawn by a custom `LV_EVENT_DRAW_MAIN` callback that renders the rotated
triangle directly. The needle object itself stays put and covers the whole sweep; each
update invalidates exactly two tight rectangles — where the needle was, and where it now
is. Measured: **13.3% of the screen per frame**.

**Do not be tempted back to `lv_image_set_rotation()`.** It is the obvious approach and it
was tried first. LVGL grows a transformed object's invalidation area using `ext_draw_size`,
a single scalar applied on all four sides, so a 14×170 needle pivoting about its end
invalidates a ~354×354 square — essentially the whole dial. That measured 28.8% dirty and
held only 45 fps. The sprite was small; its rotation envelope was not.
See [performance.md](performance.md).

### 3. Readouts are separate objects

Digital readouts are their own small labels with their own background, so a changing number
dirties only its own box rather than the dial behind it.

### 4. Flush buffers in internal DMA memory, not PSRAM

LVGL runs in **partial mode** with two flush buffers of `466 x 20 x 2B = 18,640 bytes` each,
allocated in **internal** DMA-capable SRAM by `retarget_draw_buffers()` in `app_main.c`.

This is not what the BSP does by default, and the difference is not a tuning detail. The
Waveshare BSP allocates its flush buffers in **PSRAM** (`use_psram = true`, 50 lines). On
ESP32-S3, `esp_ptr_dma_capable()` is false for PSRAM addresses, so the SPI master driver
silently allocates an internal DMA **bounce buffer the size of the whole transfer and memcpys
the frame into it, on every flush**. That only works while a contiguous ~46KB internal block
happens to be free. The moment WiFi starts, the largest free DMA block drops to ~20KB, every
transfer fails with `ESP_ERR_NO_MEM`, and the display stops outright.

The BSP exposes no way to change this, so the buffers are swapped afterwards through LVGL's
public `lv_display_set_buffers()`, before the UI is built and before WiFi claims its share of
internal RAM. Internal buffers eliminate both the bounce allocation and the extra copy.

**Keep them at 20 lines.** Measured: 12 lines hangs the LVGL task outright with no error
logged, and larger buffers do not fit alongside the network stack. The reason 12 fails is not
yet understood, so treat this value as load-bearing and re-measure before changing it.

Rendering into one buffer overlaps with DMA-ing the other out.

### 5. TE sync

The panel's tearing-effect line (GPIO13) gates the flush so a frame is not swapped mid-scan.
Configured through `esp_lvgl_port`'s tear-avoidance handling.

### 6. Both cores render

`CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2` gives LVGL two software draw units so rendering
parallelises across both Xtensa cores. `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` puts LVGL's
hot blend paths in IRAM.

## Rules for anyone adding UI

These exist to protect the budget. Breaking one is allowed only with a measurement in
[performance.md](performance.md) showing it is affordable.

1. **No full-screen gradients, blurs, or shadows on the gauge screen.** They force a
   full-frame draw layer and blow the entire budget in one object.
2. **Avoid large translucent objects.** Alpha blending over a big area is expensive; LVGL
   cannot skip what is underneath.
3. **Animate position and rotation, not size.** Resizing invalidates larger regions and
   defeats caching.
4. **Keep animated objects small and separate.** Two small dirty boxes beat one large one.
5. **Never render inside the flush callback**, and never block the LVGL task on I/O.
6. **Measure anything on the gauge screen.** The settings screen has more latitude — it is
   not in the driver's field of view while moving — but the dial screen does not.

## The known worst case: the swipe-up transition

Sliding between the dial and settings tiles necessarily redraws the full screen, every frame,
for the duration of the animation. This is the one place the design deliberately spends the
full budget.

It is implemented as an `lv_tileview` with two vertical tiles, which gives momentum-tracked
swipe for free. Its cost is measured separately from steady state in
[performance.md](performance.md).

**If it cannot hold an acceptable frame rate**, the fallback — in preference order — is:

1. Render both tiles to snapshot layers once at gesture start and slide the snapshots, so the
   transition becomes a pure blit with no re-rendering.
2. Replace the slide with a cheaper transition (cross-fade of two snapshots, or an instant
   switch) on lower-capability board profiles.

A brief dip during a deliberate user gesture is far more acceptable than any dip in the
needle's steady-state motion, so this is the right place to compromise if a compromise is
needed.

## Measuring

`gauge_perf` accumulates **bytes transferred per frame** and **dirty area per frame** — the
two numbers that actually predict whether a change is affordable. They are logged
periodically, shown live on the settings page, and available as an optional badge on the
dial. Method and results in [performance.md](performance.md).

**LVGL's own `CONFIG_LV_USE_PERF_MONITOR` is deliberately disabled.** It is pinned to a
screen corner, and this panel is round — the corners are not there, so the overlay renders
outside the visible area. It also reports LVGL's internal view of its own work rather than
what reaches the panel.
