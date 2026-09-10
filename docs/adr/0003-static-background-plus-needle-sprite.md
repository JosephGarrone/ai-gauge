# ADR 0003 — Pre-rendered face plus needle sprite

**Status:** Accepted

## Context

60fps+ is a hard requirement: a laggy needle is worse than the analogue gauge being replaced.

At 60fps a frame is 16.6ms. A full 466×466 RGB565 frame is 434,312 bytes; QSPI at 80MHz over
4 lanes gives roughly 40MB/s, so a **full-frame blit alone costs ~11ms** — two-thirds of the
budget before any pixel is rendered. On top of that, software-rendering 217,000 pixels on a
240MHz Xtensa core is not free either.

Redrawing the whole screen every frame therefore cannot hit 60fps on this hardware. Frame rate
is governed by **dirty region size**, so that is what the design must control.

## Decision

Split the gauge into a static part rendered once and a moving part kept deliberately small:

1. **Pre-render the dial face to PSRAM.** Bezel, colour bands, ticks, numeric labels and
   static titles are rasterised from the gauge XML into a full-frame RGB565 canvas at boot and
   on config change. It becomes the screen background. Per-frame cost: zero.
2. **The needle is a small rotated sprite.** ARGB8888, roughly 32×200, rotated about its pivot
   with `lv_image_set_rotation()`. LVGL invalidates only the union of its previous and current
   bounding boxes — typically under 15% of the screen.
3. **Readouts are separate small labels**, so a changing number dirties only its own box.
4. **Two DMA flush buffers in internal SRAM** (~56KB each), not PSRAM, so DMA does not contend
   with instruction fetch through the same cache.

Detail and the rules this imposes on UI code are in
[../display-pipeline.md](../display-pipeline.md).

## Alternatives considered

**Redraw the face every frame with LVGL vector/arc drawing.** Rejected: exceeds the budget by
a wide margin, and the cost scales with how elaborate the face is — meaning prettier gauges
would be slower, exactly the wrong incentive for a customisable product.

**Pre-render every needle angle as a sprite atlas.** Rejected as unnecessary. Rotating ~6,400
pixels per frame is already cheap; caching 360 sprites would consume megabytes of PSRAM to
optimise something that is not the bottleneck.

**Full-frame double buffering in PSRAM.** Rejected. It does not reduce the transfer cost,
which is the dominant term, and adds ~868KB of PSRAM traffic per frame competing with XIP
instruction fetch.

**Hardware 2D acceleration.** Not available — the ESP32-S3 has no PPA/2D-DMA blitter. (The
ESP32-P4 does, which is worth remembering if a future board is ever considered.)

## Consequences

**Good:**
- Steady-state per-frame cost is small and, crucially, **independent of how elaborate the
  face is** — an ornate gauge costs the same per frame as a plain one.
- Leaves substantial CPU headroom on core 1 for smooth needle interpolation.
- Config changes are cheap at runtime: re-render the background once, no per-frame penalty.

**Bad:**
- A full-frame RGB565 canvas costs ~434KB of PSRAM per gauge face. Acceptable against 8MB, but
  it caps how many faces can be held pre-rendered simultaneously.
- Anything that must animate *behind* the needle breaks the model and forces a face re-render.
  The schema deliberately does not offer animated backgrounds.
- Screen transitions still cost a full redraw — the accepted worst case, analysed with its
  fallbacks in [../display-pipeline.md](../display-pipeline.md).

**Neutral:**
- Imposes real constraints on UI authoring (no full-screen gradients, no large translucent
  overlays on the gauge screen). Documented as rules rather than left to be rediscovered.
