# ADR 0013 — Selectable MAP sensor types

**Status:** Accepted (2026-10-31). Built and run on the bench. **Not yet run against a sensor**: the
in-car figures below came from the previous firmware.

## Context

`sensor_hub` assumed the JRP 7-bar sensor followed the 0.5–4.5V convention over 0–8 bar absolute
([sensor-frontend.md](../sensor-frontend.md)). The first run in the car contradicted that. At
idle on the diesel, which is close to atmospheric, MAP read **about 0.35V**, and a no-load rev
took it to about 0.45–0.5V. These are rough figures, and it is not recorded whether they came from
the connector reading or the raw pin reading. On a 0.5–4.5V absolute sensor, atmosphere is 1.0V at
the connector and 0.5V at the pin, so either way the reading is too low. A 0–5V output fits.

Other people's boost sensors differ along three independent lines: the output span (0.5–4.5V or
0–5V), the reference (absolute MAP, or gauge pressure vented to atmosphere), and the range,
which may start below zero for a compound vacuum-and-boost sensor (−30 inHg to 30 psi, −1 to +2
bar). The firmware hard-coded the first two.

## Decision

1. **Three settings describe the sensor:** *MAP output* (0.5–4.5V / 0–5V, *Custom* once trimmed),
   *MAP reads* (Absolute / Gauge), and *Pressure at min/max*. They sit in the existing calibration
   picker, before the fine trims, so no widgets are added (ADR 0009, 8).
2. **The range is set in a unit the user picks, in fixed steps of that unit.** On *Pressure at
   min/max*, the button under − and + becomes *Units: bar / psi / kPa* (it is *Zero boost now*
   on every other parameter, so no widgets are added). − and + step 0.1 / 1 / 10 at the min and
   0.5 / 5 / 50 at the max, snapping to that grid; hold repeats. The page opens in the unit whose
   grid both ends already sit on. The unit is not stored: it only chooses how the kPa value is
   entered. *Amended 2026-11-03:* the first version stepped one list that interleaved bar and psi
   ratings (1.0 bar, 15 psi, 1.5 bar, 2.0 bar, 30 psi …), which was unreadable in use.
3. **Pressure is computed in the sensor's own reference, and the zero is in that reference too.**
   For an absolute sensor the zero is atmosphere, accepted at 60–110 kPa as before. For a gauge
   sensor it is the sensor's offset at rest, accepted within ±15 kPa (about 2 psi). `boost` is
   reading − zero either way. `map` for a gauge sensor is `boost + 101.325`, because nothing on the
   board measures the barometer.
4. **"No MAP signal" is detected only where it can be.** The divider pulls an unplugged input to
   0V. The threshold is half the sensor's output at the lowest pressure an engine can pull (10 kPa
   abs), capped at the old 0.25V. If that comes out under 20mV, the check is off. A 0–5V gauge
   sensor outputs 0V at rest, so unplugged and at rest look the same, and it is better to read
   zero than to show a fault every time the engine is off. The high limit becomes the sensor's top
   output + 0.25V, or 4.85V if that is higher.
5. **Changing the transfer function re-runs the auto-zero.** A zero taken through the old function
   means nothing in the new one. Switching the reference resets the stored zero to that reference's
   default (101.325 or 0).
6. **The calibration blob went to version 2, and version 1 is discarded, not migrated.** Version 1
   held the 0.5–4.5V assumption that this ADR overturns. Its other values (divider trims,
   smoothing) were never set on the only board in existence.
7. **The default is 0–5V, 0–8 bar absolute**: the fitted sensor's output as measured, over JRP's
   stated range.

## Alternatives rejected

- **One list of complete sensor presets** ("0.5–4.5V 0–100 psi gauge", …). The combinations
  multiply, and any sensor not on the list leaves the user with nothing.
- **Free numeric entry for the range.** That needs a keypad page, and a keypad costs internal RAM
  in widgets. The output trims cover what the unit steps miss.
- **A list of nominal ratings across units** (the first version). Mixed units in one sequence
  made the next value unpredictable.
- **A separate units button.** Two more widgets on a page that already fills the circle.
- **Keeping the open-circuit check on 0–5V gauge sensors** with a lower threshold. ADC noise around
  0V is the same either way, so it would trip on a healthy sensor at rest.

## Consequences

- An unplugged 0–5V gauge sensor reads 0 boost rather than dashes. This is documented in the fault
  table.
- Whether the fitted sensor spans 0–8 or 0–7 bar is still open. If the 0.35V was the pin reading,
  the figures fit 0–7 bar slightly better (atmosphere 0.71V at the connector, against 0.63V for
  8 bar). A reading at rest with the engine off settles it: see
  [sensor-frontend.md](../sensor-frontend.md#verification).
