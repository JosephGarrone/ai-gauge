# Gauge Configuration Schema

**Normative.** This document defines the gauge XML format. The firmware parser and any
authoring tool (including the future web app) must agree with what is written here.

Files live on the LittleFS `storage` partition at `/storage/gauges/<id>.xml`.

## Design principles

- **Domain-specific, not a general UI language.** The schema describes *a dial gauge*, not
  arbitrary widgets. See [ADR 0002](adr/0002-custom-gauge-xml-schema.md).
- **Forward compatible.** Unknown elements and attributes are ignored, so a newer authoring
  tool can add features without bricking older firmware.
- **Strict-but-forgiving.** Out-of-range numbers clamp to valid values, missing attributes
  take the documented defaults, and an unparseable file falls back to the built-in face with
  a visible warning. A bad config must never leave the driver with a blank screen.

## Example

```xml
<gauge version="1" id="boost">
  <panel shape="round" width="466" height="466"/>
  <source channel="boost" unit="psi" min="0" max="30"/>

  <face start-angle="225" sweep="270" background="#000000">
    <band from="0"  to="18" color="#00c853"/>
    <band from="18" to="25" color="#ffab00"/>
    <band from="25" to="30" color="#d50000"/>
    <ticks major-every="5" minor-every="1"
           major-len="24" minor-len="12" color="#ffffff"/>
    <labels every="5" font="montserrat_24" color="#ffffff" radius="150"/>
  </face>

  <needle style="taper" length="170" width="14"
          color="#ff1744" pivot-radius="18"/>

  <title text="BOOST" y="150" font="montserrat_20" color="#9e9e9e"/>
  <readout y="320" font="montserrat_48" format="%.1f" suffix=" psi" color="#ffffff"/>

  <alert above="25" flash-hz="2" color="#d50000" chime="true"/>
</gauge>
```

## Elements

### `<gauge>` (root, required)

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `version` | int | *required* | Schema version. Firmware rejects a major version it does not know. |
| `id` | string | *required* | Unique identifier; matches the filename stem. |

### `<panel>` (optional)

Declares the panel the face was authored for. If it does not match the running
`board_profile`, the firmware scales geometry proportionally rather than refusing to render.

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `shape` | `round` \| `square` | `round` | Authoring shape |
| `width`, `height` | int | from board profile | Authoring resolution in pixels |

### `<source>` (required)

Binds the gauge to a data channel and sets the value range.

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `channel` | string | *required* | Channel name, e.g. `boost`, `egt` |
| `unit` | string | `""` | Display unit; also selects unit conversion |
| `min`, `max` | float | *required* | Scale endpoints. `max` must exceed `min`. |
| `damping` | float 0–1 | `0.15` | Needle smoothing. 0 = instant, higher = smoother/laggier. |

### `<face>` (optional)

The static dial artwork. Angles are **degrees clockwise from 12 o'clock**, so 90 is the 3
o'clock position and 225 is 7:30. The defaults (`start-angle="225"`, `sweep="270"`) give
the conventional gauge: starting at lower-left and sweeping clockwise over the top to
lower-right.

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `start-angle` | float | `225` | Angle of the `min` end of the scale |
| `sweep` | float | `270` | Total angular sweep, clockwise |
| `background` | colour | `#000000` | Face fill |
| `radius` | int | fits panel | Outer radius of the scale arc |

#### `<band>` (0..n, inside `<face>`)

A coloured arc segment marking a value range.

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `from`, `to` | float | *required* | Value range, clamped to the `<source>` range |
| `color` | colour | *required* | Band colour |
| `width` | int | `18` | Radial thickness in pixels |

#### `<ticks>` (0..1, inside `<face>`)

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `major-every` | float | `10` | Value interval between major ticks |
| `minor-every` | float | `0` (off) | Value interval between minor ticks |
| `major-len`, `minor-len` | int | `20`, `10` | Tick length in pixels |
| `major-width`, `minor-width` | int | `4`, `2` | Tick thickness in pixels |
| `color` | colour | `#ffffff` | Tick colour |

#### `<labels>` (0..1, inside `<face>`)

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `every` | float | matches `major-every` | Value interval between numeric labels |
| `font` | font name | `montserrat_24` | See *Fonts* below |
| `color` | colour | `#ffffff` | |
| `radius` | int | inside the ticks | Distance from centre to label centre |
| `format` | printf | `%g` | Label number format |

### `<needle>` (optional)

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `style` | `taper` \| `line` \| `arrow` | `taper` | Needle shape |
| `length` | int | 0.72 × radius | Pivot to tip, in pixels |
| `width` | int | `12` | Width at the pivot end |
| `color` | colour | `#ff1744` | |
| `pivot-radius` | int | `16` | Centre hub radius; `0` disables the hub |
| `tail` | int | `0` | Length of the counterweight behind the pivot |

### `<title>` (0..n)

Static text on the face. Baked into the pre-rendered background, so it costs nothing per
frame.

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `text` | string | *required* | |
| `x`, `y` | int | centred horizontally | Position of the text centre |
| `font` | font name | `montserrat_20` | |
| `color` | colour | `#ffffff` | |

### `<readout>` (0..1)

The live numeric value. A separate LVGL label, not part of the background — see
[display-pipeline.md](display-pipeline.md).

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `x`, `y` | int | centred horizontally | Position of the text centre |
| `font` | font name | `montserrat_48` | |
| `format` | printf | `%.1f` | Applied to the channel value |
| `prefix`, `suffix` | string | `""` | Text either side of the number |
| `color` | colour | `#ffffff` | |

### `<peak>` (0..1)

A telltale marker that stays at the highest value reached, with an optional peak readout.
**Tapping the dial clears it.** The marker is drawn over the face and under the needle, and is
repainted only when the peak rises, so it costs nothing while the value is below its peak.

The peak tracks the **raw input value, before damping**, so a brief boost spike registers even
if the damped needle never quite reaches it.

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `color` | colour | `#ffab00` | Marker colour |
| `length` | int | `28` | Marker length, from the outer edge of the scale inward |
| `width` | int | `5` | Marker thickness |
| `show-value` | bool | `true` | Show the peak value below the live readout |
| `value-y` | int | below the readout | Vertical position of the peak value |
| `font` | font name | `montserrat_16` | Peak value font |
| `format` | printf | `%.1f` | Peak value format |
| `prefix` | string | `PEAK ` | Text before the peak value |

Omit the element to disable peak-hold.

### `<alert>` (0..n)

Fires when the value crosses a threshold. Give `above`, `below`, or both.

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `above` | float | — | Trigger when value rises above this |
| `below` | float | — | Trigger when value falls below this |
| `color` | colour | `#d50000` | Flash colour applied to readout and needle |
| `flash-hz` | float | `2` | Flash rate; `0` = steady colour change, no flash |
| `chime` | bool | `false` | Audible alert via the onboard ES8311 codec |

## Types

- **colour** — `#rrggbb` or `#rgb`. Rendered at RGB565, so fine gradations are lost.
- **bool** — `true` / `false` (also accepts `1` / `0`).
- **float / int** — decimal. Values outside a documented range are clamped, not rejected.
- **font name** — one of the fonts compiled into the firmware. Currently the Montserrat
  sizes enabled in `sdkconfig.defaults`: `montserrat_12/16/18/20/22/24/26/48`. An unknown
  font name falls back to the nearest available size and logs a warning.

## Validation and error handling

| Condition | Behaviour |
|---|---|
| Unknown element or attribute | Ignored (forward compatibility) |
| Unknown major `version` | File rejected; fall back to default face |
| Missing required attribute | File rejected; fall back to default face |
| `max <= min` | File rejected; fall back to default face |
| Number out of documented range | Clamped, warning logged |
| Band range outside source range | Clamped to the source range |
| Unknown font or malformed colour | Documented default substituted, warning logged |

Every rejection or substitution is logged and surfaced on the settings screen, so a config
that "almost works" is diagnosable without a serial cable.

## Adding to the schema

1. Update this document first — it is normative.
2. Add the field to `gauge_config_t` with a default that preserves existing behaviour.
3. Add a parser unit test, including a file that omits the new field.
4. Only bump `version` for a **breaking** change. Additive changes do not need it, because
   unknown attributes are ignored by design.
