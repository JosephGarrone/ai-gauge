"""
Generate the PX Ranger styled faces: firmware/assets/gauges/boost.xml and egt.xml, and the
built-in fallback face's XML (printed, for firmware/components/gauge_config/gauge_config_default.c).

Regenerate rather than editing coordinates by hand:

    python tools/faces/px_faces.py            # writes both .xml files
    python tools/faces/px_faces.py --default  # prints the fallback face as a C string

The look is the PX2/PX3 Ranger tachometer (reference photo: obd-display/images/rpm-gauge-px3.jpg),
with the needle outline and palette from the obd-display dashboard (NeedleGaugeView.axaml,
Styles/Colors.axaml). The design notes are in docs/gauge-faces.md.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# ---------------------------------------------------------------- palette (obd-display ipc-*)
OFF_WHITE = "#ebf0f2"  # IpcWhite: the readout, the one thing that changes
# The static artwork (ring, pointers, numerals) at 75% of IpcWhite, and the redline ring at 80% of
# IpcRedBright. These pixels never move, so on an AMOLED they wear while the black around them does
# not; dimming them cuts that wear by about a quarter, and the moving needle and readout stay at full
# brightness so they still lead the eye. Glare at night drops too.
ART = "#b0b4b6"
RED_ART = "#cc2626"
MUTED = "#8f9aa0"      # caption: secondary text, a cool grey beside the off-white
RED = "#ff3030"        # IpcRedBright: redline ring and pointers, alert
AMBER = "#ffb000"      # IpcAmber: peak marker
NEEDLE = "#17caff"     # NeedleGaugeViewModel's default needle colour
CAP = "#07090b"        # pivot cap
CAP_RIM = "#46525a"    # the rim the cap shows under cluster lighting; any darker is lost on black

# ---------------------------------------------------------------- geometry, from the photo
# Proportions are fractions of the ring's outer radius, measured on rpm-gauge-px3.jpg.
RADIUS = 220                        # scale radius on the 466 px panel (safe area allows 225)
BAND_W = 12                         # ring thickness, ~5.5% of radius
GAP_DEG = 1.4                       # break in the ring at each half-major
# The caption labels the readout from under it, not ~0.5 R above centre as on the tacho: the needle
# spends most of its time sweeping the upper face, and a caption there is crossed constantly.
# Sized for a glance from the driver's seat on the A-pillar: an 80 px whole-number readout and a 22 px
# caption, with the sparkline in the gap under the dial.
READOUT_FONT = "montserrat_digits_80"  # digits only: tools/fonts/make_digits_font.py
READOUT_Y = 258                     # top edge of the number (58 px tall)
CAPTION_Y = 342                     # centre of the unit, 14 px clear of the number
# What the gauge measures is an icon above the hub rather than a word, as on the cluster's own
# telltales; the readout keeps only its unit, under the number.
ICON_Y = 162                        # centre of the icon, ~70 px above the hub
PEAK_Y = 356                        # top edge of the peak value, when the user turns it on
TREND_Y = 378                       # top edge of the sparkline strip
TREND_W, TREND_H = 200, 50
TICK_GAP = 6                        # renderer: tick circle = band inner edge - 6 px

# Pointer: an inward triangle hanging off the ring's inner edge (y = -TICK_GAP in the tick frame).
# Narrow and deep, so it reads as an arrow and overhangs the ring's ends at min and max by little.
POINTER = f"-6.5,{-TICK_GAP - 1} 6.5,{-TICK_GAP - 1} 0,{-TICK_GAP + 15}"
# Minor graduation: a short bar hanging off the ring's inner edge, a third the pointer's depth.
MINOR = f"-1.25,{-TICK_GAP - 1} 1.25,{-TICK_GAP - 1} 1.25,{-TICK_GAP + 5} -1.25,{-TICK_GAP + 5}"


def needle_outline():
    """
    The visible part of obd-display's needle: its body minus the black cover drawn over it (both
    from NeedleGaugeView.axaml, in needle-local units with the tip at +y). The cover's channel is
    narrower than the body up to y=198, so the channel's edge is what shows there; beyond it the
    body's own taper does. The tail is cut just behind the pivot, where the cap hides it.
    """
    right = [(13.5, -20), (15, 98), (23, 123), (19, 198), (21.12, 198), (6, 494), (4, 496)]
    # On the tacho the tip reaches the ring. It stops at 180 px instead: that is the built-in
    # needle's footprint, the size the 60fps budget was measured with, and the editor warns past it.
    tip_px = 180
    k = tip_px / 496.0
    pts = right + [(-x, y) for x, y in reversed(right)]
    # Needle-local +y is the tip; the schema's needle points up (-y).
    return " ".join(f"{x * k:.2f},{-y * k:.2f}".replace(".00", "") for x, y in pts)


# Icons, about 40 px across, centred on (0, 0), x right and y down. Polygons and circles only: the
# schema's <icon> takes the same parts as a custom needle (docs/gauge-config-schema.md, Shapes).
#
# Boost: a turbocharger's compressor housing seen end-on, the symbol aftermarket boost gauges and
# Ford's own turbo warnings use. The volute's outlet leaves tangentially at the top right; two vanes
# on the wheel suggest it turning.
TURBO_ICON = """    <polygon points="2,-17 25,-17 25,-8.5 2,-8.5"/>
    <circle r="17"/>
    <circle r="12.5" color="#000000"/>
    <circle r="4.5"/>
    <polygon points="-1.6,-3.6 1.6,-3.6 6,-11 2.6,-12"/>
    <polygon points="1.6,3.6 -1.6,3.6 -6,11 -2.6,12"/>"""

# EGT: Ford's "High Exhaust System Temperature" telltale, as on the owner's cluster: a thermometer
# with three graduations standing in an exhaust stream (two dashes above a sloping line) that
# leaves as three puffs of gas. Laid out in the telltale's own proportions (EGT_SRC, measured on a
# reference image, ~460 x 375 units), then fitted to ~52 px.
#
# A slot holds 6 parts and 64 points, and this symbol has 9 pieces, so pieces that share a part
# are joined by a zero-width bridge: out along one edge and straight back. The rasteriser
# accumulates signed area, so the two bridge edges cancel exactly and leave no hairline.
EGT_STROKE = 25          # line weight, source units
EGT_FIT = 52 / 460       # source units to px, before ICON_SCALE
EGT_CENTRE = (570, 442)  # middle of the symbol's bounding box, source units


def _egt_pt(x, y):
    return ((x - EGT_CENTRE[0]) * EGT_FIT, (y - EGT_CENTRE[1]) * EGT_FIT)


def _bar(x1, y1, x2, y2, w=EGT_STROKE):
    """A straight stroke from (x1, y1) to (x2, y2), as a 4-point outline."""
    import math
    dx, dy = x2 - x1, y2 - y1
    n = math.hypot(dx, dy)
    ox, oy = -dy / n * w / 2, dx / n * w / 2
    return [(x1 + ox, y1 + oy), (x2 + ox, y2 + oy), (x2 - ox, y2 - oy), (x1 - ox, y1 - oy)]


def _puff(cx, cy, r=45, w=EGT_STROKE, a0=-115, a1=115, n=5):
    """A ')'-shaped arc opening to the left, as a band n points along each edge."""
    import math
    angs = [math.radians(a0 + (a1 - a0) * k / (n - 1)) for k in range(n)]
    outer = [(cx + (r + w / 2) * math.cos(a), cy + (r + w / 2) * math.sin(a)) for a in angs]
    inner = [(cx + (r - w / 2) * math.cos(a), cy + (r - w / 2) * math.sin(a)) for a in reversed(angs)]
    return outer + inner


def _join(*pieces):
    """Several outlines as one polygon, joined by zero-width bridges back to the first point."""
    out = list(pieces[0])
    for piece in pieces[1:]:
        out += [pieces[0][0], piece[0]] + list(piece[1:]) + [piece[0]]
    return out


def _poly(pts):
    return " ".join(f"{x:.2f},{y:.2f}".replace(".00", "") for x, y in (_egt_pt(*q) for q in pts))


# Thermometer: stem 255..470 at x 480, graduations at y 300/335/370 out to x 535.
_W2 = EGT_STROKE / 2
_STEM = [(480 - _W2, 255), (480 + _W2, 255),
         (480 + _W2, 300 - 7), (538, 300 - 7), (538, 300 + 7), (480 + _W2, 300 + 7),
         (480 + _W2, 335 - 7), (538, 335 - 7), (538, 335 + 7), (480 + _W2, 335 + 7),
         (480 + _W2, 370 - 7), (538, 370 - 7), (538, 370 + 7), (480 + _W2, 370 + 7),
         (480 + _W2, 470), (480 - _W2, 470)]
_BULB_C, _BULB_R = _egt_pt(480, 475), 35 * EGT_FIT

EGT_ICON = f"""    <polygon points="{_poly(_STEM)}"/>
    <circle cx="{_BULB_C[0]:.2f}" cy="{_BULB_C[1]:.2f}" r="{_BULB_R:.2f}"/>
    <polygon points="{_poly(_join(_bar(348, 458, 410, 443), _bar(552, 410, 614, 395)))}"/>
    <polygon points="{_poly(_bar(345, 503, 615, 563))}"/>
    <polygon points="{_poly(_join(_puff(655, 385), _puff(660, 575)))}"/>
    <polygon points="{_poly(_puff(745, 485))}"/>"""


# Drawn at ~40 px, then scaled up so they read at a glance from the driver's seat.
ICON_SCALE = 1.45


def scaled(icon):
    """The geometry attributes (points, cx, cy, r) times ICON_SCALE; colours untouched."""
    num = re.compile(r"-?\d+(?:\.\d+)?")
    fmt = lambda m: f"{float(m.group()) * ICON_SCALE:.2f}".rstrip("0").rstrip(".")
    return re.sub(r'\b(points|cx|cy|r)="([^"]*)"', lambda a: f'{a.group(1)}="{num.sub(fmt, a.group(2))}"', icon)


def needle_xml(builtin):
    if builtin:
        # The fallback face keeps the built-in needle: it is the last line of defence, so it stays
        # on the longest-tested drawing path (tools/host-tests/test_gauge_config.c asserts this).
        return f'  <needle style="taper" length="180" width="12" color="{NEEDLE}" pivot-radius="16"/>'
    return f"""  <needle color="{NEEDLE}">
    <shape>
      <polygon points="{needle_outline()}"/>
    </shape>
    <hub>
      <circle r="17.5" color="{CAP_RIM}"/>
      <circle r="16" color="{CAP}"/>
    </hub>
  </needle>"""


def bands(vmin, vmax, major, red_from):
    """Ring segments between half-majors, parted by GAP_DEG, split where the redline starts."""
    sweep = 270.0
    gap = GAP_DEG / 2 * (vmax - vmin) / sweep   # half the gap, in value units
    edges = [vmin] + [vmin + major * (i + 0.5) for i in range(int((vmax - vmin) / major))] + [vmax]
    out = []
    for a, b in zip(edges, edges[1:]):
        lo = a if a == vmin else a + gap
        hi = b if b == vmax else b - gap
        if lo < red_from < hi:
            out += [(lo, red_from, ART), (red_from, hi, RED_ART)]
        else:
            out.append((lo, hi, RED_ART if lo >= red_from else ART))
    assert len(out) <= 8, "the schema allows 8 bands"
    return out


def num(v):
    return f"{v:.2f}".rstrip("0").rstrip(".")


def face(gid, comment, channel, unit, vmax, major, minor, red_from, caption, icon, damping, fmt, label_font,
         label_r, builtin_needle=False):
    band_xml = "\n".join(
        f'    <band from="{num(a)}" to="{num(b)}" color="{c}" width="{BAND_W}"/>'
        for a, b, c in bands(0, vmax, major, red_from))
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!--
  {comment}
  PX Ranger style: see docs/gauge-faces.md. Generated by tools/faces/px_faces.py; edit that, not this.
  Schema: docs/gauge-config-schema.md
-->
<gauge version="1" id="{gid}">
  <panel shape="round" width="466" height="466"/>
  <source channel="{channel}" unit="{unit}" min="0" max="{vmax}" damping="{damping}"/>

  <face start-angle="225" sweep="270" background="#000000" radius="{RADIUS}">
{band_xml}
    <ticks major-every="{major}" minor-every="{num(minor)}" color="{ART}" band-color="true">
      <major-shape>
        <polygon points="{POINTER}"/>
      </major-shape>
      <minor-shape>
        <polygon points="{MINOR}"/>
      </minor-shape>
    </ticks>
    <labels every="{major}" font="{label_font}" color="{ART}" radius="{label_r}"/>
  </face>

  <icon y="{ICON_Y}" color="{MUTED}">
{scaled(icon)}
  </icon>

{needle_xml(builtin_needle)}

  <readout y="{READOUT_Y}" font="{READOUT_FONT}" format="{fmt}" color="{OFF_WHITE}"/>
  <title text="{caption}" y="{CAPTION_Y}" font="montserrat_22" color="{MUTED}"/>

  <peak color="{AMBER}" length="{BAND_W + 4}" width="5" value-y="{PEAK_Y}" font="montserrat_20" format="{fmt}"/>
  <trend y="{TREND_Y}" width="{TREND_W}" height="{TREND_H}" line-width="3" color="{NEEDLE}"/>
  <alert above="{num(red_from)}" flash-hz="2" color="{RED}"/>
</gauge>
"""


BOOST = dict(gid="boost", comment="Boost gauge, 0-30 PSI, redline from 26 as on the obd-display boost dial.",
             channel="boost", unit="psi", vmax=30, major=5, minor=1, red_from=26, caption="PSI", icon=TURBO_ICON,
             damping="0.15", fmt="%.0f",
             # Numerals at ~0.8 R, as on the tacho, pulled in far enough to clear the pointers at 3
             # and 9 o'clock, where a number is set sideways to the radius.
             label_font="montserrat_32", label_r=172)
EGT = dict(gid="egt", comment="Exhaust gas temperature gauge, 0-900 C, redline from 750.",
           channel="egt", unit="C", vmax=900, major=150, minor=50, red_from=750, caption="°C", icon=EGT_ICON,
           damping="0.25", fmt="%.0f",
           # Three-digit numbers: the tacho only ever has one, so a size down to keep the same clearance.
           label_font="montserrat_26", label_r=166)


def c_string(xml):
    """The face as C string literal lines, without the XML declaration and comment."""
    body = xml[xml.index("<gauge"):].strip().splitlines()
    return "\n".join('    "' + line.replace('\\', '\\\\').replace('"', '\\"') + '"'
                     for line in body if line.strip())


if __name__ == "__main__":
    if "--default" in sys.argv:
        print(c_string(face(**{**BOOST, "gid": "default", "builtin_needle": True})))
        sys.exit(0)
    out = ROOT / "firmware" / "assets" / "gauges"
    for spec in (BOOST, EGT):
        (out / f"{spec['gid']}.xml").write_text(face(**spec), encoding="utf-8", newline="\n")
        print("wrote", out / f"{spec['gid']}.xml")
