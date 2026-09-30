# Shipped Gauge Faces

The faces in `firmware/assets/gauges/`, and the compiled-in fallback, and why they look the way they
do.

| File | Look | Generated |
|---|---|---|
| `boost.xml` | PX Ranger style, 0–30 psi, redline 26 | `tools/faces/px_faces.py` |
| `egt.xml` | PX Ranger style, 0–900 °C, redline 750 | `tools/faces/px_faces.py` |
| `boost_custom.xml` | Hand-written test face for every custom-shape feature | no |
| built-in fallback (`gauge_config_default.c`) | PX boost face with the built-in needle | `px_faces.py --default` |

**Edit `px_faces.py` and regenerate; do not edit the generated XML or C string.** The script holds
the proportions below, and the three outputs have to stay one design.

## The PX Ranger style

The owner's vehicle is a PX Ranger, so the faces match its instrument cluster. Two sources:

- **The PX3 tachometer**, photographed in the car: `images/rpm-gauge-px3.jpg` in the owner's
  `obd-display` repository, with its written summary in that repository's
  `docs/px3-ranger-instrument-design-reference.md`. The markings, proportions and layout come
  from here.
- **The `obd-display` dashboard**, the owner's in-car display for the same vehicle. The palette
  (`Styles/Colors.axaml`, its `ipc-*` tokens) and the needle outline (`NeedleGaugeView.axaml`) come
  from here, so the two displays in the car speak one visual language.

What was carried over, measured on the photo as fractions of the ring's outer radius R (220 px here):

| Feature | Tacho | Here |
|---|---|---|
| Scale ring | Thick off-white ring near the rim, broken at each half-major | 7 bands, 12 px (5.5% R), 1.4° gaps |
| Redline | Ring turns red; pointers in it are red | Ring splits to `#ff3030` at the redline; `band-color` pointers |
| Major marks | Small inward triangle hanging off the ring, no line ticks | Custom major-tick shape: 13 px wide, 16 px deep |
| Minor marks | Short bars off the ring between numerals | 2.5 × 6 px bars, every 1 psi / 50 °C; red in the redline |
| Numerals | White, every major, ~0.8 R, single digits | `montserrat_32` at 172 px (boost); EGT's three-digit numbers use `montserrat_26` at 166 px to keep the same clearance |
| Caption | "RPM x 1000", ~0.5 R above centre | "BOOST  PSI", "EGT  °C", under the readout (see below) |
| Needle | Pale cyan taper with a stepped neck from a black cap | `obd-display`'s needle outline (body minus its black cover), `#17caff`, dark cap with a `#46525a` rim (darker is lost on black) |
| Background | Black | `#000000`, which is also what an AMOLED wants |

### Deliberate departures

- **The caption sits under the readout**, in `montserrat_16` and a muted `#8f9aa0`, not ~0.5 R
  above centre. Boost and EGT spend most of their time in the upper-left of the sweep, so a
  caption there was crossed by the needle constantly; below the hub it labels the number instead.
  The lower half reads top-down as value, unit, peak.

- **The needle stops at 180 px** rather than reaching the ring as on the tacho. That is the built-in
  needle's footprint, the size the 60fps budget was measured at, and the editor warns beyond it.
  The shaped `boost_custom` needle is not much bigger and costs about 6 fps (see
  [performance.md](performance.md)).
- **No glow.** The cluster and `obd-display` both put a soft halo on the needle and the hub rim. The
  renderer has no blur, and a faked halo ring looked worse than none.
- **Montserrat, not Ford's typeface.** The device fonts are LVGL's built-in Montserrat. The
  cluster's condensed numerals would need a converted font, and Ford Antenna is not licensed for
  that.
- **A digital readout and peak value** sit in the lower half, where the tacho has its coolant
  sub-dial. A single-purpose gauge needs the number.
- **The fallback face uses the built-in needle**, in the PX cyan. It is the last line of defence, so
  it stays on the longest-tested drawing path (asserted in `tools/host-tests/test_gauge_config.c`).

The alert behaviour is unchanged from the previous faces: above the redline, the readout and needle
flash red at 2 Hz.

![boost](images/faces/boost.png) ![egt](images/faces/egt.png)

Screenshots taken with `tools/remote/gauge_remote.py` while values were pushed over UDP telemetry;
the green fps counter is the display overlay, stalled by the screenshot itself.

## Measured

On hardware, 2026-09-22, needle sweeping, WiFi connected: the PX boost face runs at **61.2–61.8
fps**, 10.8% dirty, 6.5 ms mean render. On the same build, `boost_custom` measured 55.8 fps.

2026-10-01: minor graduations, the larger pointers and the caption move are all in the static
background, pre-rendered once ([ADR 0003](adr/0003-static-background-plus-needle-sprite.md)); the
needle and its dirty box are unchanged. The fps overlay read the same before and after on the
glass, but the needle sweep was not re-measured with the serial log.
