# ADR 0014: A glanceable, burn-in-aware dial

**Status:** accepted, 2026-11-27. Flashed and measured on the bench board: 66.4–66.9 fps
([performance.md](../performance.md)). The engine-off screen was checked on the bench, where the unpowered ignition input reads 0V: 20 repaints a second of the box only, 5.4 ms each. Not yet seen with a real ignition feed.

## Context

The gauge is mounted at the base of the A-pillar and read at a glance from the driver's seat. The
first in-car use showed three things:

- The 48 px readout with a decimal, a 16 px caption and a 16 px peak figure are small at that
  distance, and the fps badge and peak figure were clutter.
- A decimal place on a psi or EGT reading is noise the eye has to skip.
- The panel is an AMOLED. The ring, pointers, numerals and caption never move, so those pixels wear
  while the black around them does not, and the gauge may sit powered with the engine off.

The owner also asked for a running chart of the last 30/60/120 s.

## Decision

1. **Number format is a user setting, applied over the face.** *Boost decimals* (Auto / 0 / 0.0 /
   0.00; Auto is none for psi and kPa, one for bar) and *EGT rounded to* (1° / 10°). The renderer
   takes an override (`gauge_render_set_number_format`) for the readout, peak figure and min/max
   recall. Faces in other units keep their own `format`. No Fahrenheit: the owner does not use it.
2. **The peak figure is off by default** (*Peak figure* setting); the marker on the scale stays.
   **The fps badge is off by default**, and its NVS key was renamed (`showfps2`) so a gauge that had
   it on starts with it off once. **Brightness defaults to 50%.**
3. **An 80 px digits-only readout font**, `montserrat_digits_80`, generated from LVGL's own
   Montserrat Medium by `tools/fonts/make_digits_font.py` (Pillow only). 13 KB of glyphs.
4. **The sparkline is a face element, `<trend>`,** with its window as a user setting. Placement and
   colour belong to the face (the editor previews it); history length is a driving preference, so
   it applies to every face.
5. **The trend is a line, with no fill by default** (a filled area read as a solid block at a
   glance; `fill` opts in). It is rasterised into A8 masks on the CPU (trace, and fill if any), rebuilt from a
   one-column-per-pixel ring of means only when a column completes, and drawn as two recoloured
   images plus a few lines. The strip is its own object, repainted only then.
6. **Burn-in:** the shipped faces' static artwork is drawn at 75% (`#b0b4b6`, redline `#cc2626`);
   **pixel shift** moves the whole dial tile around a 3 px orbit every 1/3/10 minutes (setting,
   default 3) with LVGL's translate style; and when the ignition feed stays below 12.9 V for 15 s,
   an opaque **engine-off screen** on `lv_layer_top()` hides the dial and leaves the panel black
   apart from a small bordered box, "DISPLAY OFF / saving the screen / tap to wake" in 20 px, that
   drifts at 30 px/s and bounces off the round edge like an old DVD logo, changing border colour at
   each bounce. It runs at the user's brightness capped at 40%, so it stays readable. A tap shows the
   dial for a minute. No reading (no
   board, a fault) counts as running.

7. **What a gauge measures is an icon, not a word** (`<icon>`, a static shape baked into the face):
   a turbo for boost and Ford's *High Exhaust System Temperature* telltale for EGT, above the hub. The unit stays as text
   under the readout, since the digits-only font cannot carry it.

## Alternatives rejected

- **Decimals in the face XML only.** The owner wants a per-unit default and one switch for every
  face; editing each face's `format` for a preference is the wrong place for it.
- **A full 80 px Montserrat** (~200 KB of glyphs, copied to PSRAM with the rest of `.rodata`, ADR
  0012). A readout is a number.
- **`lv_font_conv`.** Needs a Node package download at generation time; the glyph set is tiny and
  Pillow is already a tool dependency.
- **Drawing the trend as 200 lines or rects per repaint.** Each is an LVGL draw task; rebuilding two
  10 KB masks a few times a second is cheaper.
- **Autoscaling the trend to the window's own min/max.** It jumps whenever a new extreme scrolls in
  or out, which reads as movement at a glance. The face's range is stable and matches the dial.
- **A window setting in the XML.** It is a driving preference, the same for every face.
- **Shifting in the flush callback.** Would move pixels the dirty-region logic does not know about.
  Translating the tile keeps LVGL's coordinates honest; the renderer re-derives the needle and peak
  marker's cached screen positions (`gauge_render_parent_moved`).
- **Turning the panel fully off with the engine off.** The owner wanted to see that the gauge is
  alive and why it is dark; a small box that never stops moving costs next to nothing in wear.
- **A notice that jumps every 20 s, at 3% brightness** (the first version). Unreadable, and the
  owner asked for the DVD-logo bounce instead.
- **Icons as embedded images.** An image element would need a converter and costs flash and
  PSRAM per icon; polygons reuse the anti-aliased shape rasteriser, scale cleanly, and the editor
  already previews them.
- **Dimming from the obd-display.** Wanted, but it depends on an in-vehicle network that is not
  designed yet.

## Consequences

- Each pixel-shift step redraws the whole dial once: one dropped frame every few minutes.
- The trend adds a ~210 × 60 px repaint up to 7 times a second at 30 s (about 3% of the screen per
  repaint). Measured: no change to the frame rate (performance.md).
- The 80 px readout's box is bigger than the 48 px one, so a changing number dirties more; whole
  numbers change less often, which more than offsets it: 5.2% dirty against 13.2% before.
- The Faces page now carries the four readout settings below the face list, and Display carries
  pixel shift and the engine-off switch. Both pages scroll. They exist only while open (ADR 0011),
  so the extra ~12 widgets cost internal RAM only then (~3.6 KB).
- The editor's parser port, serializer, preview and validation know `<trend>` and the digits font;
  the differential test covers the new element.
