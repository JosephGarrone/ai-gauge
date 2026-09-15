# Gauge XML — Authoring Interface

**Audience:** anyone building a tool that writes gauge faces for AI-Gauge, starting with the M7
configuration web app. It is meant to be read on its own, with no firmware background.

**Status:** matches firmware as of 2026-09-14 (schema `version="1"`, including custom shapes).
The normative definition is [gauge-config-schema.md](gauge-config-schema.md), and this document
must never contradict it. The two are updated together; if they ever disagree, the schema
document wins and the disagreement is a bug.

It covers:

1. [The device](#1-the-device): the canvas you are designing for
2. [File rules](#2-file-rules): what the firmware's parser accepts
3. [Element reference](#3-element-reference): every element and attribute
4. [Custom shapes](#4-custom-shapes): ticks, needle and hub outlines
5. [Rendering model](#5-rendering-model): exactly how the firmware draws a face, for an
   accurate preview
6. [Validation](#6-validation): what the firmware rejects, clamps or drops
7. [Device HTTP API](#7-device-http-api): uploading, listing and deleting faces
8. [Known gaps](#8-known-gaps-for-a-browser-app): firmware limitations a browser app will hit
9. [Reference material](#9-reference-material)

---

## 1. The device

| Property | Value |
|---|---|
| Panel | 466 × 466 px, **round** (the corners are not visible) |
| Colour depth | RGB565: 5 bits red, 6 green, 5 blue. Colours are authored as 24-bit and quantised on the device |
| Dial centre | (233, 233): integer `width / 2`, `height / 2` |
| Default scale radius | **225 px**: the shorter side minus an 8px safe inset at each edge, halved |
| Coordinates | Pixels, origin at the top-left, x to the right, y downwards |
| Angles | **Degrees clockwise from 12 o'clock**. 0 is 12 o'clock, 90 is 3 o'clock, 225 is 7:30 |

The face (bands, ticks, labels, titles) is rendered once into a background image. Only the
needle, the readouts and the peak marker change at runtime.

---

## 2. File rules

The firmware uses a small hand-written parser, not a full XML library. Emit files that stay
inside this subset:

| Rule | Detail |
|---|---|
| Encoding | UTF-8. The fonts are Latin; do not rely on other scripts or on symbols such as `°` rendering |
| Size | **16,384 bytes maximum** |
| Root | A single `<gauge>` element |
| Supported syntax | Start tags, end tags, self-closing tags, attributes, `<!-- comments -->`, `<?xml … ?>` prolog |
| Attribute quoting | `"double"` or `'single'` |
| **Entities** | **Not decoded.** `&amp;` stays as the five literal characters `&amp;`. Keep `&`, `<` and the enclosing quote character out of attribute values entirely |
| Text content | Ignored. Everything is carried in attributes |
| CDATA, DOCTYPE, namespaces | Not supported; do not emit them |
| **Element order** | **`<source>` must come before `<face>`**, because band ranges are clamped against it as the file is read. Emit elements in the order shown in [§3](#3-element-reference) |
| Unknown elements/attributes | Ignored. Do not invent them; emit only what this document defines |
| Numbers | Plain decimal. Emit at most 4 decimal places |
| `version` | Always `1` |

### Identifiers

A face's `id` is also its filename and its URL path segment:

- 1–31 characters from `A–Z a–z 0–9 _ -`
- The `id` attribute **must equal** the id used in the upload URL. The firmware does not
  check this today, but other parts of the system assume it.
- The device lists at most 12 faces.

### String lengths

Longer strings are **silently truncated**. Enforce these limits in the editor:

| Field | Max characters |
|---|---|
| `gauge@id`, `source@channel` | 31 |
| `source@unit` | 15 |
| `title@text`, `readout@prefix`, `readout@suffix`, `peak@prefix` | 31 |
| Any `format` | 15 |
| Any `font` | 23 |

### Formats

`format` attributes are C `printf` formats and are handed a **double**. Allow only a single
floating-point conversion with optional flags, width and precision: `%f`, `%g` or `%e`, for
example `%.1f`, `%.0f`, `%g`. Literal text around it is allowed, and `%%` prints a percent sign.
Anything else (`%d`, `%s`, two conversions) is undefined behaviour on the device and must be
rejected by the editor.

Rendered text buffers are also finite: labels hold 15 characters, and a readout's
prefix + number + suffix is cut at 95.

### Types

| Type | Syntax |
|---|---|
| colour | `#rrggbb` or `#rgb`. Emit `#rrggbb` |
| bool | `true` / `false` (the parser also accepts `1` / `0`) |
| int, float | Decimal. Out-of-range values clamp; see [§6](#6-validation) |
| font | One of `montserrat_12`, `montserrat_16`, `montserrat_18`, `montserrat_20`, `montserrat_22`, `montserrat_24`, `montserrat_26`, `montserrat_48` |
| point list | SVG `points` syntax: numbers separated by spaces and/or commas, in x,y pairs. Emit `x,y x,y …` |

---

## 3. Element reference

A complete face, in canonical emit order:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<gauge version="1" id="boost">
  <panel shape="round" width="466" height="466"/>
  <source channel="boost" unit="psi" min="0" max="30" damping="0.15"/>

  <face start-angle="225" sweep="270" background="#000000">
    <band from="0" to="18" color="#00c853"/>
    <band from="18" to="25" color="#ffab00"/>
    <band from="25" to="30" color="#d50000"/>
    <ticks major-every="5" minor-every="1" major-len="24" minor-len="12" color="#ffffff">
      <major-shape> … </major-shape>   <!-- optional, §4 -->
      <minor-shape> … </minor-shape>   <!-- optional, §4 -->
    </ticks>
    <labels every="5" font="montserrat_24" color="#ffffff" radius="150"/>
  </face>

  <needle style="taper" length="170" width="14" color="#ff1744" pivot-radius="18">
    <shape> … </shape>                 <!-- optional, §4 -->
    <hub> … </hub>                     <!-- optional, §4 -->
  </needle>

  <title text="BOOST" y="150" font="montserrat_20" color="#9e9e9e"/>
  <readout y="320" font="montserrat_48" format="%.1f" suffix=" psi" color="#ffffff"/>
  <peak color="#ffab00" length="28" width="5"/>
  <alert above="25" flash-hz="2" color="#d50000" chime="true"/>
</gauge>
```

An element with no children may be written self-closing (`<ticks … />`). An element that has
shape children must use a separate end tag.

### `<gauge>` — root, required

| Attribute | Type | Default | Notes |
|---|---|---|---|
| `version` | int | **required** | `1` |
| `id` | string | **required** | See *Identifiers* |

### `<panel>` — optional, at most one

| Attribute | Type | Default | Notes |
|---|---|---|---|
| `shape` | `round` \| `square` | `round` | Informational today |
| `width`, `height` | int | board's | Emit `466` / `466`. Proportional scaling to other panels is planned (M8), not implemented |

### `<source>` — required, exactly one, before `<face>`

| Attribute | Type | Default | Notes |
|---|---|---|---|
| `channel` | string | **required** | Data channel, e.g. `boost`, `egt` |
| `unit` | string | `""` | |
| `min`, `max` | float | **required** | `max` must be greater than `min`, or the file is rejected |
| `damping` | float 0–1 | `0.15` | See [§5.6](#56-motion-and-alerts) |

### `<face>` — optional, at most one

| Attribute | Type | Default | Notes |
|---|---|---|---|
| `start-angle` | float | `225` | Angle of `min`. Taken modulo 360 |
| `sweep` | float 1–360 | `270` | Clockwise span from `min` to `max` |
| `background` | colour | `#000000` | Full-panel fill |
| `radius` | int | `225` (fits panel) | Outer radius of the scale. `0` means fit |

### `<band>` — inside `<face>`, 0–8

| Attribute | Type | Default | Notes |
|---|---|---|---|
| `from`, `to` | float | **required** | Values, clamped to the source range. Needs `to > from` after clamping |
| `color` | colour | `#ffffff` | The schema lists it as required; always emit it |
| `width` | int | `18` | Radial thickness, measured **inwards** from the scale radius |

### `<ticks>` — inside `<face>`, at most one

Omit the element and there are no ticks.

| Attribute | Type | Default | Notes |
|---|---|---|---|
| `major-every` | float | `10` | Value interval. `0` = no major ticks |
| `minor-every` | float | `0` | Value interval. `0` = no minor ticks |
| `major-len`, `minor-len` | int | `20`, `10` | Ignored when the matching shape is set |
| `major-width`, `minor-width` | int | `4`, `2` | Ignored when the matching shape is set |
| `color` | colour | `#ffffff` | Inherited by tick shapes |

### `<labels>` — inside `<face>`, at most one

Omit the element and there are no numeric labels.

| Attribute | Type | Default | Notes |
|---|---|---|---|
| `every` | float | `major-every` | Value interval. `0` = follow `major-every` |
| `font` | font | `montserrat_24` | |
| `color` | colour | `#ffffff` | |
| `radius` | int | computed | Distance from the centre to the label's centre; see [§5.3](#53-labels-and-text) |
| `format` | printf | `%g` | |

### `<needle>` — optional, at most one

If omitted, the defaults below still draw a needle.

| Attribute | Type | Default | Notes |
|---|---|---|---|
| `style` | `taper` \| `line` \| `arrow` | `taper` | Ignored when `<shape>` is set |
| `length` | int | `0.72 × radius` (162 at the default radius) | Pivot to tip. Ignored when `<shape>` is set |
| `width` | int | `12` | Ignored when `<shape>` is set |
| `tail` | int | `0` | Counterweight length behind the pivot. Ignored when `<shape>` is set |
| `color` | colour | `#ff1744` | Inherited by needle and hub shapes |
| `pivot-radius` | int | `16` | Built-in hub circle; `0` = no hub. Ignored when `<hub>` is set |

### `<title>` — 0–4

| Attribute | Type | Default | Notes |
|---|---|---|---|
| `text` | string | **required** | A title without it is dropped |
| `x` | int | 233 (centre) | Horizontal **centre** of the text |
| `y` | int | `0` | Vertical **centre** of the text |
| `font` | font | `montserrat_20` | |
| `color` | colour | `#ffffff` | |

### `<readout>` — at most one

Omit it and there is no live number.

| Attribute | Type | Default | Notes |
|---|---|---|---|
| `x` | int | 233 (centre) | Horizontal **centre** of the text |
| `y` | int | `0` | **Top edge** of the text. This differs from `<title>` |
| `font` | font | `montserrat_48` | |
| `format` | printf | `%.1f` | |
| `prefix`, `suffix` | string | `""` | |
| `color` | colour | `#ffffff` | |

### `<peak>` — at most one

Omit it and peak-hold is off.

| Attribute | Type | Default | Notes |
|---|---|---|---|
| `color` | colour | `#ffab00` | Marker and peak text |
| `length` | int | `28` | Marker length, inwards from the scale radius |
| `width` | int | `5` | Marker thickness |
| `show-value` | bool | `true` | Show the peak number |
| `value-y` | int | below the readout | **Top edge** of the peak text; always horizontally centred on the panel |
| `font` | font | `montserrat_16` | |
| `format` | printf | `%.1f` | |
| `prefix` | string | `PEAK ` | Note the trailing space |

### `<alert>` — 0–4

| Attribute | Type | Default | Notes |
|---|---|---|---|
| `above` | float | — | Fires while the value is strictly greater |
| `below` | float | — | Fires while the value is strictly less |
| `color` | colour | `#d50000` | Flash colour |
| `flash-hz` | float 0–20 | `2` | `0` = steady colour, no flashing |
| `chime` | bool | `false` | Sound once on entry, if the user has chimes enabled |

At least one of `above` / `below` is needed, or the alert is dropped.

---

## 4. Custom shapes

Four optional slots replace the built-in drawing of ticks, needle and hub with filled outlines.
Omit a slot and the built-in drawing is used.

| Slot | Parent | Replaces | Origin (0,0) is… | Rotation |
|---|---|---|---|---|
| `<major-shape>` | `<ticks>` | `major-len`, `major-width` | the point on the **tick circle** at that tick's angle | rotated to each tick's angle |
| `<minor-shape>` | `<ticks>` | `minor-len`, `minor-width` | same | same |
| `<shape>` | `<needle>` | `style`, `length`, `width`, `tail` | the pivot (dial centre) | rotated to the current value's angle |
| `<hub>` | `<needle>` | the `pivot-radius` circle | the pivot | never rotated |

The tick circle's radius is defined in [§5.2](#52-ticks).

### 4.1 Coordinate frame

**Draw each shape as it looks at 12 o'clock**, in pixels, x right and y down (SVG
coordinates). The firmware rotates it clockwise about its origin.

```
 Needle <shape>                     Tick <major-shape> / <minor-shape>

          (0,-176)  tip                    outwards, towards the rim
             /\                                     ^  -y
            /  \                                    |
           /    \                        (-4,0) ----o---- (4,0)   <- origin on tick circle
          /      \                                \     /
     (-9,0)---o---(9,0)  <- origin = pivot         \   /
          \       /                                 \_/ (±1.5, 28)
           \_____/  (±6,38) counterweight           |
                                                    v  +y
                                           inwards, towards the centre
```

- A needle tip has **negative y**. A counterweight behind the pivot has positive y.
- For ticks, negative y points out towards the rim and positive y points in towards the
  centre. The built-in 4×20 major tick is equivalent to `-2,0 2,0 2,20 -2,20`.
- Precision is **1/16 px**. Round coordinates to multiples of 0.0625.

### 4.2 Slot and part elements

A slot has one optional attribute, `color`. It contains **parts**, drawn in document order with
later parts on top.

```xml
<shape color="#ff1744">                                  <!-- slot; color optional -->
  <polygon points="x,y x,y x,y …" color="#rrggbb"/>      <!-- ≥3 vertices, closed implicitly -->
  <circle cx="0" cy="0" r="10" color="#rrggbb"/>          <!-- cx, cy default 0; r > 0 -->
</shape>
```

| Element | Attribute | Type | Default |
|---|---|---|---|
| `<polygon>` | `points` | point list | **required**, at least 3 vertices |
| | `color` | colour | inherited |
| `<circle>` | `cx`, `cy` | float | `0` |
| | `r` | float | **required**, greater than 0 |
| | `color` | colour | inherited |

Slots must be **direct children** of their parent, and parts direct children of their slot.
Anything elsewhere is silently ignored, which is the most likely cause of a shape that "does
nothing". Emit at most one of each slot; a repeated slot replaces the earlier one.

### 4.3 Colour resolution and alerts

Each part takes the first of: its own `color`, then the slot's `color`, then the parent's
`color` (`<ticks color>` or `<needle color>`).

While an alert flashes, **needle parts that resolved to `<needle color>` take the alert colour**.
Parts with their own or the slot's colour keep it. **The hub never flashes.** A useful pattern:
leave the needle body uncoloured so it flashes, and give a centre stripe its own colour.

### 4.4 Limits

| Limit | Value | What the firmware does |
|---|---|---|
| Parts per slot | 6 | Extra parts dropped |
| Polygon vertices per slot, shared across its polygons | 64 | A polygon that would exceed it is dropped whole |
| Circles | Use **no** vertex budget | — |
| Coordinate / radius magnitude | 2047 px | Clamped |
| Polygon area | ≥ 0.25 px² | Smaller polygons dropped |

### 4.5 Geometry rules for the editor

- **Straight edges only.** The firmware has no curves. Flatten Béziers and arcs into polygon
  vertices before writing, to a tolerance of about **0.1 px**, then simplify (for example with
  Ramer–Douglas–Peucker) until the slot fits the 64-vertex budget. Prefer `<circle>` for round
  features: it costs nothing from the budget and the device renders it smoothly.
- **Simple polygons.** Edges must not cross. Winding direction does not matter. Where a polygon
  overlaps itself the overlap is filled, not cut out, which differs from SVG's `evenodd` rule, so
  reject self-intersections instead of previewing them differently.
- **No holes.** A ring-shaped hub must be drawn as a filled circle with a smaller circle on top
  in another colour, not as a cut-out.
- Parts may overlap each other freely; later parts cover earlier ones.
- **Keep ticks narrow.** A tick shape is stamped at every tick value and must not run into its
  neighbours. The spacing between tick origins is about
  `tickRadius × sweep × π / 180 × every / (max − min)` pixels.

### 4.6 Performance guidance

A shaped needle is re-rasterised on the device every time it visibly moves, at a cost roughly
proportional to the area of each part's bounding box. Long, wide or many-part needles cost more
per frame. Until this has been measured on hardware, **keep needle shapes to at most 3 parts
and within the built-in needle's footprint** (about 20px wide and 180px long). Tick and hub
shapes are drawn once, so their complexity has no per-frame cost.

---

## 5. Rendering model

This is what the firmware actually does, specified precisely enough for a preview to match. `cx`
and `cy` are the dial centre (233, 233), and `R` is the scale radius (`face@radius`, or 225).

### 5.1 Value to angle

```
frac  = clamp((value − min) / (max − min), 0, 1)
angle = start-angle + frac × sweep          // degrees clockwise from 12 o'clock
point at distance d:  x = cx + d·sin(angle),  y = cy − d·cos(angle)
```

### 5.2 Ticks

```
widest     = largest band width, or 0 with no bands
tickRadius = R − widest − (6 if any band else 0)
count      = floor((max − min) / every)      // skipped entirely if count > 400
values     = min + i·every,  i = 0 … count   // starts at min, not at a multiple of `every`
```

Minor ticks are drawn first, then major ticks, so majors cover minors where they coincide.

- **Built-in tick:** a straight line from `tickRadius` inwards to `tickRadius − len`, `width`
  pixels thick, with square (butt) ends, in `<ticks color>`.
- **Shaped tick:** the shape's origin sits on the tick circle at the tick's angle, and the shape
  is rotated by that angle.

### 5.3 Labels and text

```
step    = labels@every, or major-every when every is 0   // skipped if count > 100
reach   = ceil(max y of <major-shape>) if set, else major-len
radius  = labels@radius, or tickRadius − reach − lineHeight(font)
text    = printf(format, value) centred on the point at `radius`, `angle`
```

Titles and labels are centred on their point. The layout box is `8 × lineHeight` wide, so
longer text may wrap.

The **readout** is a label whose top edge is at `y` and whose horizontal centre is at `x`. The
**peak value** is horizontally centred on the panel with its top edge at `value-y`. If `value-y`
is not given, it sits at `readout.y + lineHeight(readout.font) + 2`, or at `cy + 60` when there
is no readout.

| Font | Line height (px) |
|---|---|
| `montserrat_12` | 15 |
| `montserrat_16` | 18 |
| `montserrat_18` | 21 |
| `montserrat_20` | 22 |
| `montserrat_22` | 24 |
| `montserrat_24` | 27 |
| `montserrat_26` | 29 |
| `montserrat_48` | 52 |

Montserrat is available from Google Fonts, so a preview can use the same face. Glyph metrics
will not match LVGL's rendering to the pixel.

### 5.4 Bands

An arc from `angle(from)` to `angle(to)`, both **rounded to whole degrees**. Its outer edge is at
radius `R` and it extends `width` pixels inwards. The ends are square, not rounded. Bands are
drawn in document order.

### 5.5 Needle, hub, peak marker

**Built-in needles**, with `L = length`, `w = width` and `t = tail`, pointing along the value's
angle:

| Style | Geometry |
|---|---|
| `taper` | Triangle: tip at distance `L`; base corners at the pivot, `w/2` either side, perpendicular to the needle. If `t > 0`, also a `w`-thick round-capped line from the pivot back to distance `t` behind it |
| `line` | Round-capped line, `w` thick, from the tip at `L` back to `t` behind the pivot |
| `arrow` | Shaft: round-capped line, `w/2` thick (at least 1), from `t` behind the pivot to `0.75·L`. Head: triangle from the tip at `L` to a base at `0.75·L`, `w` either side |

**Built-in hub:** a filled circle of radius `pivot-radius` in the needle colour.

**Peak marker:** a round-capped line `width` thick, at the peak value's angle, from radius `R`
inwards to `R − length`.

**Stacking order, bottom to top:**
1. Face: background, bands, minor ticks, major ticks, labels, titles
2. Peak marker
3. Needle
4. Hub
5. Readout
6. Peak value text

### 5.6 Motion and alerts

- **Damping.** At each sample: `displayed += (target − displayed) × (1 − damping)`, with the
  target clamped to `[min, max]`. `0` tracks instantly. Samples arrive at the sensor's rate, so
  the time constant is not fixed.
- **Alerts** compare the *damped* displayed value. The first alert in document order whose
  condition holds is active. On entry, the readout text and the inheriting needle parts switch
  to its colour, toggling every `500 / flash-hz` ms; with `flash-hz="0"` the colour stays
  steady. The chime sounds once on entry.
- **Peak** tracks the *raw* (undamped) value. Tapping the dial clears it.

### 5.7 Previewing shapes in SVG

A shape slot maps directly onto SVG transforms, with `deg` the angle from §5.1:

```xml
<!-- a tick at angle deg -->
<g transform="translate(233 233) rotate(deg) translate(0 -tickRadius)"> …parts… </g>
<!-- the needle at the current value -->
<g transform="translate(233 233) rotate(deg)"> …parts… </g>
<!-- the hub -->
<g transform="translate(233 233)"> …parts… </g>
```

SVG's `rotate()` is clockwise in y-down coordinates, which is the same convention as the schema.
Render `<polygon>` with `fill` set to the resolved colour and `fill-rule="nonzero"`. The device
anti-aliases edges by exact pixel coverage, so a browser's anti-aliasing is a close match.

---

## 6. Validation

The firmware is **strict-but-forgiving**. A face with a problem is still displayed where
possible, and only a few errors reject the whole file. Every forgiven problem increments a
**warning count**, which the device reports (see [§7](#7-device-http-api)).

**An editor should prevent all of the conditions below**, rather than rely on the device to
correct them.

### Rejects the file

The existing face on the device is kept, and the upload fails with a reason.

| Condition | Error text |
|---|---|
| No `<gauge>` root | `no <gauge> root element` |
| `version` missing or not `1` | `missing or unsupported schema version` |
| `id` missing, `<source>` missing, or `channel` / `min` / `max` missing | `missing required attribute` |
| `max ≤ min` | `invalid value range (max must exceed min)` |

### Forgiven, with one warning each

| Condition | Result |
|---|---|
| Number outside its range (damping 0–1, sweep 1–360, flash-hz 0–20, pixel sizes 0–4096, positions ±4096) | Clamped |
| Malformed colour | Default kept |
| Band `from` / `to` outside the source range | Clamped |
| Band missing `from` / `to`, or `to ≤ from` after clamping | Band dropped |
| More than 8 bands, 4 titles or 4 alerts | Extras dropped |
| Title without `text` | Dropped |
| Alert with neither `above` nor `below` | Dropped |
| Unknown `panel@shape` or `needle@style` | Default kept |
| Polygon: fewer than 3 vertices, malformed or odd-length list, area < 0.25 px² | Part dropped |
| Circle: `r` missing or ≤ 0 | Part dropped |
| More than 6 parts in a slot, or a polygon exceeding the 64-vertex budget | Part dropped |
| Shape coordinate or radius beyond ±2047 | Clamped |

### Forgiven silently (no warning)

| Condition | Result |
|---|---|
| Unknown element or attribute, or a shape element outside its parent | Ignored |
| String longer than its limit | Truncated |
| Unknown font name | LVGL's default `montserrat_14` used (logged on the device's serial console only) |
| Unparseable number in an optional attribute | Default kept |
| A slot with no valid parts | Built-in drawing used |

---

## 7. Device HTTP API

The device serves plain HTTP on port 80, at `http://ai-gauge-XXXX.local`. `XXXX` is the last
four hex digits of its WiFi MAC, shown on the device's settings page. It is also reachable at
its IP address. The full networking document is [networking.md](networking.md).

| Method | Path | Request | Success response |
|---|---|---|---|
| `GET` | `/api/gauges` | — | `200 {"gauges":["boost","egt"],"max":12}` |
| `GET` | `/api/config/<id>` | — | `200 {"id":"boost","channel":"boost","unit":"psi","min":0.000,"max":30.000,"warnings":0}` |
| `GET` | `/api/config/<id>.xml` | — | `200` the stored XML, `application/xml` |
| `PUT` | `/api/config/<id>` | Body: the raw XML, at most 16,384 bytes | `200 {"ok":true}` |
| `DELETE` | `/api/config/<id>` | — | `200 {"ok":true}` |
| `GET` | `/api/status` | — | Firmware version, uptime, WiFi (including `hostname`), heap and storage state, and `active_gauge`: the face on screen, `""` for the built-in face |
| `GET` | `/editor/` | — | The face editor, served by the gauge |

Errors are JSON of the form `{"error":"<reason>"}`:

| Status | When | Example reasons |
|---|---|---|
| `400` | Upload rejected; **the existing face is untouched** | `missing or oversized body`, `invalid gauge name`, `too large (limit 16384 bytes)`, `the face's id is 'boost', not 'boost2'`, `gauge is full (12 faces); delete one first`, `storage not mounted`, or any of the parser errors in [§6](#6-validation) |
| `404` | Config get or delete for an unknown id | `not found`, or the parse error of a stored file |
| `500` | Device out of memory | `out of memory` |

Behaviour worth knowing:

- **An upload is validated before it is written**, then replaces the old file atomically. If the
  uploaded id is the face currently on screen, the device re-renders it live, without
  rebooting.
- **The URL id must equal the file's `<gauge id>`**, and a gauge holds at most `max` faces (12).
  Uploading an id already stored replaces that face, even when the gauge is full.
- **Deleting the face on screen** makes the gauge show the first remaining face that loads, or its
  built-in face; `active_gauge` reports which.
- **`PUT` does not report warnings.** After a successful upload, call `GET /api/config/<id>` and
  check `warnings`. Anything other than 0 means the device corrected or dropped something.
- **`GET /api/config/<id>` returns a summary; `GET /api/config/<id>.xml` returns the file.** The
  file comes back exactly as stored, even if it no longer parses.
- **Cross-origin calls are accepted from loopback origins only** (`http://localhost`,
  `127.0.0.1`, `[::1]`, any port), with an `OPTIONS` preflight on `/api/config/<id>`. Any other
  page must be served by the gauge itself; see §8.

---

## 8. Known gaps for a browser app

**A browser page can reach a gauge only if the gauge served it, or if it is served from
`http://localhost`.** The gauge serves the face editor at `/editor/` for this reason
([ADR 0008](adr/0008-gauge-serves-face-editor.md)). Recorded here and in
[networking.md](networking.md):

| Limitation | Effect on a browser app | State |
|---|---|---|
| **HTTP only** | An `https://` page, including GitHub Pages, is blocked from calling an `http://` device by mixed-content rules | Permanent: no trusted certificate is possible for a `.local` name. Serve the app from the gauge |
| **CORS for loopback origins only** | A page from any other origin cannot read replies or pass a `PUT`/`DELETE` preflight | Deliberate: a wildcard would let any site replace faces on an unauthenticated API |
| **Private network access** | Chromium browsers increasingly restrict public sites from calling private-network addresses | Does not affect a page served by the gauge |
| **`.local` names** | mDNS resolution depends on the OS and browser; an IP address may be needed | Open |
| **No authentication** | Anyone on the network can replace faces | Open |

A web app hosted anywhere else should therefore work fully **offline**: design, preview, validate,
and import or export `.xml` files, then hand over to the gauge's copy for upload.

---

## 9. Reference material

| What | Where |
|---|---|
| Normative schema | [gauge-config-schema.md](gauge-config-schema.md) |
| Why shapes are polygons | [adr/0006-custom-shapes-as-polygons.md](adr/0006-custom-shapes-as-polygons.md) |
| Example faces (both parse with 0 warnings) | [../firmware/assets/gauges/boost.xml](../firmware/assets/gauges/boost.xml), [../firmware/assets/gauges/boost_custom.xml](../firmware/assets/gauges/boost_custom.xml) |
| Parser behaviour, as executable tests | [../tools/host-tests/test_gauge_config.c](../tools/host-tests/test_gauge_config.c) |
| Shape geometry and rasteriser tests | [../tools/host-tests/test_gauge_shape.c](../tools/host-tests/test_gauge_shape.c) |
| Parser source | [../firmware/components/gauge_config/gauge_config.c](../firmware/components/gauge_config/gauge_config.c) |
| The face editor, a reference implementation of this document (parser port, writer, validator, preview) | [config-app.md](config-app.md), [../tools/config-app/](../tools/config-app/) |
| Renderer source | [../firmware/components/gauge_render/gauge_render.c](../firmware/components/gauge_render/gauge_render.c) |
