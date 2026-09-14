# Gauge Configuration Schema

**Normative.** This document defines the gauge XML format. The firmware parser and any
authoring tool must agree with what is written here. The authoring guide for the web app,
[gauge-xml-interface.md](gauge-xml-interface.md), restates this format together with the
rendering model and upload API. It must be updated in the same change as this document.

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

A face using custom tick, needle and hub shapes is in
[../firmware/assets/gauges/boost_custom.xml](../firmware/assets/gauges/boost_custom.xml); see
[Shapes](#shapes).

## Elements

### `<gauge>` (root, required)

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `version` | int | *required* | Schema version. Firmware rejects a major version it does not know. |
| `id` | string | *required* | Unique identifier; matches the filename stem. |

### `<panel>` (optional)

Declares the panel the face was authored for. If it does not match the running
`board_profile`, the firmware scales geometry proportionally rather than refusing to render.
(Scaling is not yet implemented; it is scheduled for M8.)

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `shape` | `round` \| `square` | `round` | Authoring shape |
| `width`, `height` | int | from board profile | Authoring resolution in pixels |

### `<source>` (required)

Binds the gauge to a data channel and sets the value range. **It must come before `<face>`**,
because band ranges are clamped against it as they are read.

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

#### `<band>` (0..8, inside `<face>`)

A coloured arc segment marking a value range.

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `from`, `to` | float | *required* | Value range, clamped to the `<source>` range |
| `color` | colour | *required* | Band colour |
| `width` | int | `18` | Radial thickness in pixels, measured inwards from the scale radius |

#### `<ticks>` (0..1, inside `<face>`)

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `major-every` | float | `10` | Value interval between major ticks |
| `minor-every` | float | `0` (off) | Value interval between minor ticks |
| `major-len`, `minor-len` | int | `20`, `10` | Tick length in pixels |
| `major-width`, `minor-width` | int | `4`, `2` | Tick thickness in pixels |
| `color` | colour | `#ffffff` | Tick colour |

Optional children: `<major-shape>` and `<minor-shape>` replace the built-in rectangular ticks
with a custom outline, and make the matching `-len` and `-width` attributes irrelevant. See
[Shapes](#shapes).

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

Optional children: `<shape>` replaces the needle drawn by `style`, `length`, `width` and
`tail`. `<hub>` replaces the `pivot-radius` circle. See [Shapes](#shapes).

### `<title>` (0..4)

Static text on the face. Baked into the pre-rendered background, so it costs nothing per
frame.

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `text` | string | *required* | |
| `x` | int | panel centre | Horizontal centre of the text |
| `y` | int | `0` | **Vertical centre** of the text |
| `font` | font name | `montserrat_20` | |
| `color` | colour | `#ffffff` | |

### `<readout>` (0..1)

The live numeric value. A separate LVGL label, not part of the background — see
[display-pipeline.md](display-pipeline.md).

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `x` | int | panel centre | Horizontal centre of the text |
| `y` | int | `0` | **Top edge** of the text (unlike `<title>`, whose `y` is its centre) |
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
| `value-y` | int | below the readout | **Top edge** of the peak value, which is always horizontally centred on the panel |
| `font` | font name | `montserrat_16` | Peak value font |
| `format` | printf | `%.1f` | Peak value format |
| `prefix` | string | `PEAK ` | Text before the peak value |

Omit the element to disable peak-hold.

### `<alert>` (0..4)

Fires when the value crosses a threshold. Give `above`, `below`, or both.

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `above` | float | — | Trigger when value rises above this |
| `below` | float | — | Trigger when value falls below this |
| `color` | colour | `#d50000` | Flash colour applied to readout and needle |
| `flash-hz` | float | `2` | Flash rate; `0` = steady colour change, no flash |
| `chime` | bool | `false` | Audible alert via the onboard ES8311 codec |

## Shapes

Custom outlines for the major ticks, minor ticks, needle and hub. Leave a slot out and the
built-in drawing is used, so a file without shapes renders exactly as it always did. The design
rationale is [ADR 0006](adr/0006-custom-shapes-as-polygons.md).

### Slots

| Slot | Parent | Replaces | Origin `(0,0)` | Rotated? |
|---|---|---|---|---|
| `<major-shape>` | `<ticks>` | `major-len`, `major-width` | The point on the **tick circle** at the tick's angle | Yes, to each tick's angle |
| `<minor-shape>` | `<ticks>` | `minor-len`, `minor-width` | As above | Yes |
| `<shape>` | `<needle>` | `style`, `length`, `width`, `tail` | The pivot, at the dial centre | Yes, to the value's angle |
| `<hub>` | `<needle>` | the `pivot-radius` circle | The pivot | No |

The **tick circle** is the scale radius minus the widest band's width minus a 6px gap, or the
scale radius itself when there are no bands.

Each slot takes one optional attribute:

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `color` | colour | parent's `color` | Colour for parts that do not set their own |

A slot is only recognised as a direct child of its parent. At most one of each is meaningful;
a repeated slot replaces the earlier one.

### Coordinate frame

Draw every shape **as it would appear at 12 o'clock**, in pixels, with **x to the right and y
downwards**: the SVG convention. The firmware rotates it clockwise by the relevant schema angle.

- **Needle:** the tip points up, so tip coordinates have **negative y**. A counterweight
  behind the pivot has positive y.
- **Ticks:** negative y points outwards towards the rim, and positive y points in towards the
  dial centre. The built-in major tick is equivalent to the rectangle
  `-2,0 2,0 2,20 -2,20`.
- **Hub:** drawn as authored, centred on the pivot.

Coordinates may be fractional. They are stored to 1/16 px and edges are anti-aliased.

### Parts

A slot contains one or more parts, drawn in document order (later parts on top):

#### `<polygon>`

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `points` | point list | *required* | At least three vertices, closed implicitly |
| `color` | colour | slot, then parent | Fill colour |

#### `<circle>`

| Attribute | Type | Default | Meaning |
|---|---|---|---|
| `cx`, `cy` | float | `0` | Centre, in the slot's frame |
| `r` | float | *required* | Radius, greater than 0 |
| `color` | colour | slot, then parent | Fill colour |

Polygons should be simple (not self-intersecting). Winding direction does not matter. Where a
polygon overlaps itself, the overlap is simply filled, never cut out. **Holes are not
supported.** **Curves are not supported:** an authoring tool must flatten Béziers and arcs into
polygon vertices before writing the file.

### Colour and alerts

A part's colour is its own `color`, else the slot's `color`, else the parent element's `color`
(`<ticks color>` or `<needle color>`). While an alert flashes, the needle parts that fell through
to `<needle color>` take the alert colour; parts with their own or the slot's colour keep it.
The hub never flashes, just as the built-in hub never has.

### Limits

| Limit | Value |
|---|---|
| Parts per slot | 6 |
| Polygon vertices per slot, shared by all its polygons (circles use none) | 64 |
| Coordinate and radius magnitude | 2047 px |
| Precision | 1/16 px |

### Effect on other elements

- With a custom major tick and no explicit `<labels radius>`, labels are placed inside the
  tick's largest y instead of inside `major-len`.

### Example

```xml
<ticks major-every="5" minor-every="1" color="#ffffff">
  <major-shape>
    <polygon points="-4,0 4,0 1.5,28 -1.5,28"/>
  </major-shape>
  <minor-shape color="#9e9e9e">
    <circle cy="5" r="2.5"/>
  </minor-shape>
</ticks>

<needle color="#ff1744">
  <shape>
    <polygon points="0,-176 3,-150 7,-20 9,0 6,38 -6,38 -9,0 -7,-20 -3,-150"/>
    <polygon points="-1,-150 1,-150 1.5,-30 -1.5,-30" color="#ffffff"/>
  </shape>
  <hub color="#303030">
    <circle r="22"/>
    <circle r="8" color="#ff1744"/>
  </hub>
</needle>
```

## Types

- **colour** — `#rrggbb` or `#rgb`. Rendered at RGB565, so fine gradations are lost.
- **bool** — `true` / `false` (also accepts `1` / `0`).
- **float / int** — decimal. Values outside a documented range are clamped, not rejected.
- **point list** — SVG `points` syntax: numbers separated by whitespace and/or commas, taken
  in x,y pairs. Accepts signs, decimals and exponents (`1e1`), and SVG's run-together forms
  (`3-4` is `3, -4`; `.5.5` is `0.5, 0.5`).
- **font name** — one of the fonts compiled into the firmware. Currently the Montserrat
  sizes enabled in `sdkconfig.defaults`: `montserrat_12/16/18/20/22/24/26/48`. An unknown
  font name falls back to LVGL's default font (`montserrat_14`, which is otherwise not
  selectable) and logs a warning on the serial console. It does not count as a parse warning.

## Validation and error handling

| Condition | Behaviour |
|---|---|
| Unknown element or attribute | Ignored (forward compatibility) |
| Unknown major `version` | File rejected; fall back to default face |
| Missing required attribute | File rejected; fall back to default face |
| `max <= min` | File rejected; fall back to default face |
| Number out of documented range | Clamped, warning logged |
| Band range outside source range | Clamped to the source range |
| Malformed colour | Documented default substituted, warning counted |
| Unknown font | LVGL default font (`montserrat_14`) used; logged on the serial console only |
| More than 8 bands, 4 titles or 4 alerts | Extras dropped, warning each |
| Shape slot or part outside its parent | Ignored (as an unknown element) |
| Polygon with fewer than 3 points, a malformed or odd-length list, or under 0.25 px² of area | Part dropped, warning |
| Circle with a missing or non-positive `r` | Part dropped, warning |
| Part beyond 6, or a polygon that would take the slot past 64 vertices | Part dropped whole, warning |
| Shape coordinate or radius beyond ±2047 px | Clamped, warning |
| Slot left with no valid parts | Built-in drawing used |

Every rejection or substitution is logged and surfaced on the settings screen, so a config
that "almost works" is diagnosable without a serial cable.

## Adding to the schema

1. Update this document first — it is normative — and
   [gauge-xml-interface.md](gauge-xml-interface.md) in the same change.
2. Add the field to `gauge_config_t` with a default that preserves existing behaviour.
3. Add a parser unit test, including a file that omits the new field.
4. Update the face editor in the same change: the parser port and the rest of `tools/config-app/js`,
   `tools/host-tests/gauge_config_dump.c`, and cases in `tools/config-app/test/corpus.mjs`. CI's
   differential test fails if the two parsers disagree. See [config-app.md](config-app.md).
4. Only bump `version` for a **breaking** change. Additive changes do not need it, because
   unknown attributes are ignored by design.
