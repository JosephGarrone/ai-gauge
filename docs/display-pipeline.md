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

### 2. Only a small needle sprite moves

The needle is an ARGB8888 sprite (~32×200) rotated about its pivot with
`lv_image_set_rotation()`. LVGL invalidates the union of the needle's previous and current
bounding boxes — for typical needle motion that is well under 15% of the screen.

Rotating ~6,400 pixels per frame is cheap, and the resulting blit is a fraction of the 11ms
full-frame figure. This is what leaves real headroom at 60fps.

### 3. Readouts are separate objects

Digital readouts are their own small labels with their own background, so a changing number
dirties only its own box rather than the dial behind it.

### 4. Two DMA buffers in internal SRAM

LVGL runs in **partial mode** with two flush buffers of roughly `466 × 60 × 2B ≈ 56KB` each,
allocated in **internal** SRAM — deliberately not PSRAM. SPI DMA out of internal SRAM runs at
full rate; out of PSRAM it contends for the same cache and bus as instruction fetch (this
board runs `SPIRAM_XIP_FROM_PSRAM`), which shows up directly as reduced throughput.

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

`CONFIG_LV_USE_PERF_MONITOR` gives an on-screen FPS and CPU-load overlay, toggleable from the
settings screen. Beyond that, the flush callback accumulates **bytes transferred per frame**
and **dirty area per frame** — the two numbers that actually predict whether a change is
affordable. Method and results in [performance.md](performance.md).
