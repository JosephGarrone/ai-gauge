# Performance

60fps+ is a hard requirement. This document records **how it is measured** and **what has
actually been measured on hardware**.

> **Rule:** nothing in the *Results* section may be filled in from theory, estimation or a
> desktop simulator. Only numbers observed on a real board go there. Predictions live in the
> *Budget* section and are labelled as such.

## Budget (theoretical)

| Quantity | Value |
|---|---|
| Frame period at 60fps | 16.6 ms |
| Full frame, 466×466 RGB565 | 434,312 bytes |
| QSPI throughput (80MHz × 4 lanes, theoretical) | ~40 MB/s |
| **Full-frame blit** | **~11 ms** (~66% of budget) |
| Needle sprite (~32×200 ARGB8888) rotation | ~6,400 px |
| Expected steady-state dirty region | <15% of screen |

Derivation and the design that follows from it: [display-pipeline.md](display-pipeline.md).

## Method

### On-screen overlay

`CONFIG_LV_USE_PERF_MONITOR` shows FPS and CPU load, toggleable from the settings screen.
Convenient, but it measures LVGL's own view of the world — treat it as an indicator, not
evidence.

### Instrumented flush

The two numbers that actually predict affordability are accumulated in the flush callback:

- **Bytes transferred per frame** — the QSPI cost.
- **Dirty area per frame** (px², and as a % of the screen) — the render cost.

Both are reported over the serial log and via `GET /api/status`.

### Standard test scenarios

Run each for at least 30 seconds and record min / mean / max.

| # | Scenario | What it isolates |
|---|---|---|
| 1 | Static needle, no input | Baseline idle cost |
| 2 | Needle sweeping min→max at 1Hz (simulated source) | Steady-state gauge cost — **the number that matters** |
| 3 | Needle at realistic boost rate-of-change | Representative driving |
| 4 | Swipe-up transition, repeated | The known worst case |
| 5 | Scenario 2 with WiFi connected and telemetry streaming | Cross-core interference |
| 6 | Scenario 2 with the FPS overlay enabled | Cost of the instrumentation itself |

Scenario 5 matters more than it looks: WiFi runs on core 0, LVGL on core 1, and the claim that
they do not interfere is an assumption until it is measured.

## Results

Measured on hardware: ESP32-S3 rev v0.2, 240MHz, 8MB octal PSRAM @80MHz, QIO flash @80MHz,
466x466 CO5300 panel. Scenario 2 (needle sweeping full scale continuously), simulated source
ticking every 5ms so the needle differs on every frame.

| Date | Firmware | Scenario | FPS | Dirty % | Bytes/frame | Render ms | Notes |
|---|---|---|---|---|---|---|---|
| 2026-09-10 | needle as rotated `lv_image` | 2 | **45.4** | 28.8% (62,632 px) | 125,264 | 11.65 | **Failed the gate.** See below. |
| 2026-09-10 | needle custom-drawn, tight invalidation | 2 | **66.2** | 13.3% (28,900 px) | 57,900 | 5.80 | **Passes.** |
| 2026-09-10 | + tileview screens | 2 | **66.6** | 13.2% (28,700 px) | 57,400 | 5.86 | No regression from adding the screens. |
| 2026-09-10 | + tileview screens | 4 | **33.4** | 46.2% (100,200 px) | 200,500 | 15.76 | Transition. Accepted worst case — see below. |

**The gate passes at 66.2 fps.** That figure is the LVGL refresh-period ceiling
(`CONFIG_LV_DEF_REFR_PERIOD=15` gives 1000/15 = 66.7 fps), not a rendering limit: rendering
consumes 5.8ms of each 15ms period, about 39% utilisation, so there is real headroom above
the 60fps requirement rather than a result that only just scrapes in.

Other measurements from the same run:

- Face pre-render: **36 ms**, once at startup. Zero per-frame cost thereafter, as designed.
- Face buffer: 424KB in PSRAM. Free after startup: 6,874KB PSRAM, 165KB internal.
- First measurement window shows a ~35ms max render: that is the initial full-screen paint,
  not steady state.

### What the first attempt got wrong

ADR 0003 originally specified the needle as a small ARGB8888 sprite rotated with
`lv_image_set_rotation()`, predicting under 15% dirty area. Measured, it produced **28.8%**
and only 45 fps.

The cause: LVGL grows a transformed object's invalidation area using `ext_draw_size`, which
is a **single scalar applied on all four sides**. A 14x170 needle pivoting about its end
therefore invalidates a square roughly 354x354 -- essentially the whole dial -- no matter how
few pixels the needle itself covers. The sprite was small; its rotation envelope was not.

The fix was to drop image rotation and draw the rotated triangle directly from a
`LV_EVENT_DRAW_MAIN` callback, with the needle object left stationary and covering the whole
sweep, and only two tight rectangles invalidated per update: where the needle was, and where
it now is. Dirty area fell to 13.3% and render time roughly halved.

Two smaller fixes in the same change:

- The readout label was full panel width (466px), so every value change dirtied a
  466 x 56 strip. It is now sized to its content.
- The instrumentation itself was wrong at first: it summed `LV_EVENT_INVALIDATE_AREA`
  events, which are per-request and overlap freely, and reported over 100% of the screen per
  frame. It now measures `LV_EVENT_FLUSH_START` areas, which are what actually crosses the
  QSPI bus. **A metric that can report 103% is not measuring a real quantity** -- worth
  remembering before trusting any number here.

### Scenario 4: the swipe transition

Measured with `CONFIG_AI_GAUGE_BENCH_TRANSITION=y`, which flips tiles every 1.2s so the
transition is a repeatable input rather than a human swiping.

**~33 fps** across the measurement window, which mixes animating frames with settled ones.
The animating frames themselves are worse: max render sits at **34.5ms** with the full
217,156 px invalidated, so during the slide the display runs at roughly **29 fps**.

This is the compromise [display-pipeline.md](display-pipeline.md) anticipated and accepted.
Sliding two full-screen tiles past each other cannot avoid redrawing everything, and a dip
during a deliberate user gesture is far less costly than any dip in the needle's steady-state
motion. The needle is unaffected: scenario 2 still measures 66.6 fps with the tileview in
place.

If the transition ever needs to be smoother, the fallbacks are listed in
[display-pipeline.md](display-pipeline.md) -- snapshotting both tiles at gesture start and
sliding the snapshots, so the transition becomes a pure blit. That work has **not** been done.

### Not yet measured

- Scenario 5 (WiFi active) -- `net_svc` does not exist yet.
- Scenarios 1, 3 and 6.

## The gate

**Scenario 2 must hold ≥60fps with CPU headroom before sensor and networking work is layered
on.** If the render strategy cannot meet budget, that must be discovered while it is still
cheap to change the approach — not after everything else has been built on top of it.

**Status: passed** at 66.2 fps with 39% render utilisation — on the second attempt. The
first attempt failed at 45 fps, which is precisely why this gate exists before the rest of
the system is built on top of the renderer.

If a future change fails the gate, the fallbacks are in
[display-pipeline.md](display-pipeline.md).

## When something gets slower

1. Check whether dirty area or bytes/frame changed — that distinguishes a rendering
   regression from a transfer one.
2. Check whether a new UI element broke one of the rules in
   [display-pipeline.md](display-pipeline.md) — full-screen gradients and large translucent
   objects are the usual culprits.
3. Confirm the performance-critical config is still set: `CONFIG_COMPILER_OPTIMIZATION_PERF`,
   `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2`, `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM`, the octal
   PSRAM settings, and 240MHz CPU.
4. Confirm the flush buffers are still in internal SRAM, not PSRAM. This one is easy to break
   accidentally and expensive when broken.
