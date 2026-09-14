# ADR 0006 — Custom tick, needle and hub shapes as polygons

**Status:** Accepted (2026-09-14). Implemented and host-tested; frame cost not yet measured on
hardware — see [../performance.md](../performance.md).

## Context

Users want to draw their own minor ticks, major ticks, needle and needle hub, not just pick
from the built-in `taper` / `line` / `arrow` styles. The M7 web app will be the authoring
tool, so the format has to suit a browser editor as well as the firmware.

Two constraints shape the design:

- **The needle's frame cost.** [ADR 0003](0003-static-background-plus-needle-sprite.md) holds
  60fps by repainting only the needle's exact bounding box, and records that rotating an image
  sprite broke this (28.8% dirty, 45fps). A custom needle must keep the tight box.
- **Internal RAM.** `gauge_config_t` is copied by value through the loader, the renderer and
  the reload path, and ~3KB of internal heap is left once WiFi runs. The shape data must be
  bounded, compact and kept out of internal memory.

## Decision

1. **Shapes are filled polygons and circles in the XML**, inside four slot elements:
   `<ticks>` → `<major-shape>`, `<minor-shape>`; `<needle>` → `<shape>`, `<hub>`. The syntax is
   SVG's (`<polygon points="x,y …">`, `<circle cx cy r>`), so a web editor can render a part
   directly as SVG. Each shape is drawn pointing at 12 o'clock around its own origin and
   rotated into place.
2. **Curves are flattened by the authoring tool**, not the firmware. The firmware accepts only
   straight-edged polygons plus circles.
3. **Bounded, fixed-point storage.** Up to 6 parts and 64 shared points per shape, stored as
   int16 in 1/16 px (~340 bytes a shape, ~1.4KB for all four). Over-budget parts are dropped
   with a warning, following the schema's strict-but-forgiving rule.
4. **An anti-aliased coverage rasteriser of our own** (`gauge_shape`, a host-testable leaf
   with no LVGL dependency), using the signed-area accumulation technique from font renderers.
   - The needle is rasterised into A8 masks sized to its current bounding box and blended
     untransformed, so each update still invalidates only the old and new boxes.
   - Ticks are blended straight into the pre-rendered face.
   - The hub is rasterised once.
5. **Every copy of `gauge_config_t` moves to PSRAM**: the active config in `app_main`, the one
   loaded when switching gauges, the built-in default, and the renderer struct that embeds one.
   Before this change several of these sat in internal RAM, so the net effect should *free*
   internal memory despite the larger struct.

## Alternatives considered

**Split polygons into triangles and draw them with `lv_draw_triangle()`.** Rejected. LVGL
anti-aliases each triangle's edges independently, so adjacent triangles leave faint seams
along every internal edge — visible on a needle that is the focal point of the display.

**Bitmap images (PNG) for each shape.** Rejected for the needle: a rotated image brings back
the whole-dial invalidation envelope that ADR 0003 measured at 45fps. It would also need an
asset upload path and storage management, and images do not scale with `<panel>` the way
coordinates do.

**SVG `<path>` parsed in the firmware.** Rejected for now. Béziers and arcs need a path parser
and curve flattening on the device, adding code and stack on the HTTP task for no visible
gain: the web app can flatten to 0.1px before upload. Adding `<path>` later is backwards
compatible, because unknown elements are ignored.

**LVGL's vector graphics (ThorVG).** Rejected. It is C++, currently disabled, adds substantial
flash and RAM, and its per-frame cost on this panel is unmeasured. The rasteriser we need is
about 150 lines of C that is testable on a host.

**Polygon holes (multiple rings per part).** Deferred. Most hub and needle designs do not need
them, and a part can be layered over another in a contrasting colour. The rasteriser already
supports holes, so adding a ring syntax later is cheap.

## Consequences

**Good:**
- Arbitrary needle, hub and tick designs, with no change to the dirty-region model.
- Built-in styles are unchanged, and a file with no shape elements renders exactly as before.
- The format maps one-to-one onto SVG, which makes a live browser preview straightforward.
- The rasteriser's correctness (coverage, seams, winding, worst-case buffer size) is covered
  by host tests.

**Bad:**
- A shaped needle is rasterised in software on the LVGL task at every visible move. The cost
  scales with the needle's bounding-box area and **must be measured on hardware** against
  the frame budget before it is relied on.
- Each shaped needle part holds a worst-case-sized mask in PSRAM (tens of KB for a long
  needle).
- Curved designs spend their point budget on flattening; 64 points per shape is a hard cap.

**Neutral:**
- The web app now owns curve flattening and must respect the point budget. That contract is
  written down in [../gauge-xml-interface.md](../gauge-xml-interface.md).
