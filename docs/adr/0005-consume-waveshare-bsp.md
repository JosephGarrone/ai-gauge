# ADR 0005 — Consume the upstream Waveshare BSP

**Status:** Accepted

## Context

The board needs a CO5300 QSPI AMOLED driver, a CST9217 touch driver, AXP2101 power
management, and LVGL wiring. Waveshare publishes a maintained board support package on the
Espressif component registry — `waveshare/esp32_s3_touch_amoled_1_75` (v3.0.1, ESP-IDF ≥5.5)
— exposing the standard `esp-bsp` API (`bsp_display_start()`, `bsp_display_lock()`).

## Decision

Depend on the upstream BSP plus `espressif/esp_lvgl_port`, and spend our effort on the
rendering strategy and application instead of on panel bring-up.

```yaml
dependencies:
  waveshare/esp32_s3_touch_amoled_1_75: "^3.0.1"
  espressif/esp_lvgl_port: "^2.9.0"
  lvgl/lvgl: { version: "9.4.*", public: true }
```

This sets the **ESP-IDF floor at v5.5**. The project pins **v5.5.1** locally and in CI.

## Alternatives considered

**Write our own CO5300 and CST9217 drivers.** Rejected. Panel initialisation sequences are
fiddly, poorly documented and easy to get subtly wrong; the display is not where this project
adds value. Revisit only if the BSP proves to block a specific optimisation we need.

**Vendor a copy of the BSP into the repository.** Rejected for now. It would pin behaviour
exactly and allow local patches, at the cost of losing upstream fixes. Worth reconsidering if
upstream makes a breaking change or if we need modifications they will not take.

**Arduino / GFX stack.** Rejected. ESP-IDF gives direct control over DMA, task placement, core
affinity and cache configuration — all of which the 60fps requirement depends on.

## Consequences

**Good:**
- Panel, touch and power work from day one; effort goes into the gauge itself.
- Upstream fixes arrive with a version bump.
- The standard `esp-bsp` API is what other boards implement too, so a future square or
  differently-sized panel is largely a BSP swap plus a new `board_profile`.

**Bad:**
- We depend on Waveshare's release cadence and their choice of LVGL integration.
- If a display optimisation needs changes below the BSP API, we must fork or vendor.
- The IDF ≥5.5 floor is inherited, not chosen.

**Neutral:**
- The BSP initialises SD and audio support we use only incidentally
  ([ADR 0004](0004-retain-sd-and-audio.md)).
