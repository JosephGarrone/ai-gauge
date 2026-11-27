# ADR 0014: A glanceable, burn-in-aware dial

**Status:** accepted, 2026-11-27. Built and host-tested; **not yet run on hardware** (the gauge was
in the car, off the network).

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
5. **The trend is rasterised into two A8 masks on the CPU** (fill and trace), rebuilt from a
   one-column-per-pixel ring of means only when a column completes, and drawn as two recoloured
   images plus a few lines. The strip is its own object, repainted only then.
6. **Burn-in:** the shipped faces' static artwork is drawn at 75% (`#b0b4b6`, redline `#cc2626`);
   **pixel shift** moves the whole dial tile around a 3 px orbit every 1/3/10 minutes (setting,
   default 3) with LVGL's translate style; and when the ignition feed stays below 12.9 V for 15 s,
   an opaque **engine-off screen** on `lv_layer_top()` covers the dial at 3% brightness with an
   "ENGINE OFF" notice that moves every 20 s. A tap shows the dial for a minute. No reading (no
   board, a fault) counts as running.

## Alternatives rejected

- **Decimals in the face XML only.** The owner wants a per-unit default and one switch for every
  face; editing each face's `format` for a preference is the wrong place for it.
- **A full 80 px Montserrat** (~200 KB of glyphs, copied to PSRAM with the rest of `.rodata`, ADR
  0012). A readout is a number.
- **`lv_font_conv`.** Needs a Node package download at generation time; the glyph set is tiny and
  Pillow is already a tool dependency.
- **Drawing the trend as 200 lines or rects per repaint.** Each is an LVGL draw task; rebuilding two
  10 KB masks a few times a second is cheaper and gives a filled area for free.
- **Autoscaling the trend to the window's own min/max.** It jumps whenever a new extreme scrolls in
  or out, which reads as movement at a glance. The face's range is stable and matches the dial.
- **A window setting in the XML.** It is a driving preference, the same for every face.
- **Shifting in the flush callback.** Would move pixels the dirty-region logic does not know about.
  Translating the tile keeps LVGL's coordinates honest; the renderer re-derives the needle and peak
  marker's cached screen positions (`gauge_render_parent_moved`).
- **Turning the panel fully off with the engine off.** The owner wanted to see that the gauge is
  alive; a 3% notice that keeps moving costs next to nothing in wear.
- **Dimming from the obd-display.** Wanted, but it depends on an in-vehicle network that is not
  designed yet.

## Consequences

- Each pixel-shift step redraws the whole dial once: one dropped frame every few minutes.
- The trend adds a ~210 × 60 px repaint up to 7 times a second at 30 s (about 3% of the screen per
  repaint). Expected to be cheap; **to be measured on hardware** (performance.md).
- The 80 px readout's box is bigger than the 48 px one, so a changing number dirties more; whole
  numbers change less often, which offsets it. **To be measured.**
- The Faces page now carries the four readout settings below the face list, and Display carries
  pixel shift and the engine-off switch. Both pages scroll. They exist only while open (ADR 0011),
  so the extra ~12 widgets cost internal RAM only then (~3.6 KB).
- The editor's parser port, serializer, preview and validation know `<trend>` and the digits font;
  the differential test covers the new element.
