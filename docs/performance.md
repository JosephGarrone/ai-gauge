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

**None recorded yet — no measurements have been taken on hardware.**

The design is complete and the theory is in the *Budget* section above, but no board has been
run. Do not cite the budget figures as achieved performance.

Fill in as measured:

| Date | Firmware | Scenario | FPS (min/mean) | Dirty % | Bytes/frame | Notes |
|---|---|---|---|---|---|---|
| | | | | | | |

## The gate

**Scenario 2 must hold ≥60fps with CPU headroom before sensor and networking work is layered
on.** If the render strategy cannot meet budget, that must be discovered while it is still
cheap to change the approach — not after everything else has been built on top of it.

If the gate fails, the fallbacks are in [display-pipeline.md](display-pipeline.md).

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
