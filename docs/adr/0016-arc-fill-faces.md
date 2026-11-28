# ADR 0016: Arc-fill faces, drawn into the face buffer

**Status:** accepted, 2026-11-28. Built, flashed over OTA and measured on the bench board with
values fed over UDP telemetry: 66.6 fps at 2.4% dirty for a 1 Hz full-scale sweep
([performance.md](../performance.md#modern-arc-faces-2026-11-28)). Not yet seen in the car.

## Context

The owner asked for faces matching the Modern dashboard of the in-car OBD Display (the obd-display
repository: `ArcGauge.cs`, `ModernDashboardView.axaml`). Its dials have no needle. The reading is a
thick lit band whose length is the value, fading from Ford blue at its start to the reading's
colour at its head, over a soft glow, with a white head bar, the warning and danger stretches as a
slim stripe on the band's outer edge, and the ticks and numerals it has passed lit. The boost arc
is mirrored and runs anticlockwise, but only because of where it sits on the page.

The renderer was built around a pre-rendered face and a small moving needle
([ADR 0003](0003-static-background-plus-needle-sprite.md)). A filled arc is the opposite case: the
moving part is as big as the scale. Drawing it as LVGL arcs each frame would redraw a box the size
of the whole dial, and the dashboard's fade, a conic gradient scaled to the fill, changes every
pixel of the fill whenever the value moves.

## Decision

1. **A new `<arc>` element** (radius, width, colour, start colour, glow, head, lit ticks), with
   `<needle style="none">`, a negative `sweep` for a mirrored dial, and a `radius` on `<band>` and
   `<ticks>` so that a hairline guide can sit inside the band and the ticks outside it. All
   additive: old faces parse and render as before ([gauge-config-schema.md](../gauge-config-schema.md#arc-01)).
2. **The fill is drawn into the face buffer itself.** At build time every pixel of the annulus the
   fill can light gets its unlit colour, the point along the scale where it lights, its band or
   glow coverage and its lit-tick coverage (~6 B per pixel, 326 KB in PSRAM for the shipped faces).
   When the value moves, only the pixels whose key lies between the old and the new value are
   rewritten, and only their box is invalidated. The head bar is a separate small object, drawn
   like the needle.
3. **The fade is fixed to the scale, not to the fill.** A pixel's colour depends only on where it
   is on the scale, so a moving value never recolours the rest of the fill. The head bar keeps the
   head the brightest thing on the scale, which is what the dashboard's head-relative fade gave.
4. **Zones recolour the whole fill, steadily, at most four times a second.** An active alert's
   colour replaces the fill's, fading up from a dark shade of it, as on the dashboard. A recolour
   repaints every lit pixel (one long frame); the rate limit bounds what a value sitting on a zone
   edge can cost. The fill does not flash; the readout, icon and head do.
5. **Ticks and labels light whole.** A tick's pixels take its own value as their key, and each
   label has a lit and an unlit tile swapped as the fill passes it, so nothing is ever half lit.
6. **The faces are generated** by `tools/faces/modern_faces.py`, a sibling of `px_faces.py` that
   shares its icons. Both arcs run clockwise: the dashboard mirrors its boost arc only for its
   place on the page (owner, 2026-11-28). The numerals sit inside the band and the ticks outside,
   with the band at the rim. Both are 250° arcs, centred on the round panel rather than leaning
   inward as the dashboard's do in their frames.

## Alternatives rejected

- **LVGL arcs drawn every frame**, or an `lv_arc` widget. The fill's box is the whole dial, so every
  frame would redraw ~60% of the screen; the full-frame blit alone is ~11 ms
  ([display-pipeline.md](../display-pipeline.md)).
- **`lv_draw_arc` with an image source** (the lit face as a texture, masked by an arc). LVGL's arc
  angles are whole degrees in this build (`LV_USE_FLOAT` off), which steps 3–4 px at the band, and
  each zone colour would need its own full-frame lit image (434 KB each; EGT has four zones).
- **The dashboard's head-relative fade.** Every lit pixel changes colour whenever the value moves:
  the whole fill every frame.
- **Pre-rendered PNG or RGB565 assets on LittleFS.** The faces would stop being editable in the face
  editor, OTA does not update LittleFS, and the per-pixel data still has to be built for each
  zone colour.
- **The boost-target marker.** The dashboard draws the PCM's boost target as a wedge and bar. The
  gauge asks the display for one channel, its face's ([ADR 0015](0015-obd-display-link.md)), so it
  has no target to draw. It needs a second channel in the link, which is a change on both ends.

## Consequences

- An arc face holds 66.6 fps where a needle face holds ~61–66, because the dirty slice is smaller
  than a needle's box. A zone crossing costs one long frame (~100k px, ~20 ms).
- 326 KB of PSRAM per arc face on top of the 424 KB face buffer. Internal RAM is unchanged: the
  planes, rows and label tiles are all PSRAM.
- **Burn-in:** the fill is lit wherever the reading sits. An EGT that cruises at 300 °C keeps the
  first third of its arc lit for hours, where a needle lit a 20 px strip. The fade puts the
  dimmest colours there and pixel shift still applies, but the trade is real; the PX faces remain
  for a driver who prefers less wear.
- Firmware older than this change cannot show an arc face properly: it ignores `<arc>`, takes
  `style="none"` as an unknown style (a taper needle, one warning) and clamps the boost face's
  negative sweep to 1° (one more warning). Upload arc faces to updated gauges only. A gauge
  updated over the air also does not get them on its storage: OTA writes the app, not LittleFS,
  so they are uploaded from the editor's examples (they ship in the app image) or over HTTP.
