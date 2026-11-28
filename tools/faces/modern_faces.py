"""
Generate the Modern styled faces: firmware/assets/gauges/boost_modern.xml and egt_modern.xml.

    python tools/faces/modern_faces.py

The look is the obd-display Modern dashboard's arcs (src/desktop/ObdDisplay.Desktop/Views/Pages/
Gauges/ArcGauge.cs, laid out in ModernDashboardView.axaml, palette from Styles/Colors.axaml): a thick
lit band in place of a needle, fading up from Ford blue to the reading's colour, over a soft glow,
with a white head bar, the warning and danger stretches as a slim stripe on the band's outer edge,
ticks outside the band and numerals inside it, both lit as the fill passes them. Both arcs run
clockwise: the dashboard mirrors its boost arc only because it sits on the right of the page.

The icons come from px_faces.py, so the two styles share them. Design notes: docs/gauge-faces.md.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import px_faces  # noqa: E402  (the icons and the shared palette)

ROOT = Path(__file__).resolve().parents[2]

# ---------------------------------------------------------------- palette (obd-display)
BG = "#05090b"           # IpcBg / ArcGauge FadeBase: near-black, nothing boxed
TRACK = "#0b2031"        # ArcGauge TrackColor: the band where the fill has not reached
FILL = "#00d2f0"         # IpcCyanBright / NormalFill
FILL_START = "#0065b0"   # IpcBlue / StartColor: where the fade starts
TICK = "#596267"         # IpcGreyDark: ticks the fill has not reached
NUMERAL = "#969ea2"      # IpcGrey: numerals the fill has not reached
LIT = "#ebf0f2"          # IpcWhite: lit major ticks and numerals, and the readout
UNIT = px_faces.MUTED    # the unit under the readout, as on the PX faces
HEAD = "#ffffff"         # IpcWhiteBright: the head bar
AMBER = "#ffb000"        # IpcAmber: peak marker


def over(fg, alpha, bg):
    """fg at alpha over bg, as '#rrggbb'. The stripes and guide are translucent on the dashboard."""
    f = [int(fg[i:i + 2], 16) for i in (1, 3, 5)]
    b = [int(bg[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{round(x * alpha + y * (1 - alpha)):02x}" for x, y in zip(f, b))


# The hairline guide just inside the band, the full sweep (ArcGauge GuideColor #3300bee1), and the
# zone stripes over the track (WarnTint #b0ffb000, DangerTint #d0e62323). Pre-mixed, because the
# face's bands are opaque.
GUIDE = over("#00bee1", 0x33 / 255, BG)
STRIPE_ALPHA = 0xb0 / 255   # the dashboard's warning tint; danger is a little stronger
DANGER_ALPHA = 0xd0 / 255

# ---------------------------------------------------------------- geometry
# ArcGauge's band and tick proportions (16 px band and 0.55x ticks on a 276 px control), scaled to
# the 466 px round panel. The numerals sit inside the band (owner's call, 2026-11-28), so the band
# moves out to the rim: its outer edge at 204, the ticks outside it to 222, within the 225 px safe
# area. The numerals ring the inside of the glow; at 3 and 9 o'clock, where a numeral is set
# sideways to the radius, its box has to stay inside the glow (r 168), which sets their radius.
SWEEP = 250              # as on the dashboard, but centred: 125 degrees either side of 12
BAND_OUT = 204           # the face radius: outer edge of the band
BAND_W = 26
GLOW = 10                # ArcGauge's five glow passes reach 2..10 px past the band
STRIPE_W = 7             # ArcGauge: max(3, thickness * 0.28)
GUIDE_R = BAND_OUT - BAND_W - 4   # 5 px inside the band, 2 px thick (a 1 px arc breaks up)
TICK_BASE = BAND_OUT + 4          # ticks start 4 px outside the band and point outward
MAJOR_LEN = 14                    # ArcGauge: thickness * 0.55
MINOR_LEN = 7
# Ticks as shapes hanging outward from TICK_BASE (negative y is outward in the tick frame).
MAJOR = f"-1.25,0 1.25,0 1.25,{-MAJOR_LEN} -1.25,{-MAJOR_LEN}"
MINOR = f"-0.75,0 0.75,0 0.75,{-MINOR_LEN} -0.75,{-MINOR_LEN}"

# Inside the band, top to bottom: the icon, the 80 px readout, its unit, the optional peak figure,
# and the sparkline in the gap between the band's ends.
ICON_Y = 150             # centre
READOUT_Y = 194          # top edge of the number (58 px tall)
CAPTION_Y = 274          # centre of the unit
PEAK_Y = 290             # top edge of the peak figure, when the user turns it on
TREND_Y = 330
TREND_W, TREND_H = 180, 50


def num(v):
    return f"{v:.2f}".rstrip("0").rstrip(".")


def face(gid, comment, channel, unit, vmax, major, minor, zones, alerts, caption, icon,
         start, sweep, label_font, label_r, damping="0.88"):
    """zones: [(from, colour)] ascending; each runs to the next, the last to vmax."""
    stripes = []
    for k, (z, colour) in enumerate(zones):
        to = zones[k + 1][0] if k + 1 < len(zones) else vmax
        alpha = DANGER_ALPHA if k == len(zones) - 1 else STRIPE_ALPHA
        stripes.append(
            f'    <band from="{num(z)}" to="{num(to)}" color="{over(colour, alpha, TRACK)}" width="{STRIPE_W}"/>')
    alert_xml = "\n".join(
        f'  <alert above="{num(above)}" flash-hz="{hz}" color="{c}"/>'
        for above, c, hz in sorted(alerts, key=lambda a: -a[0]))
    stripe_xml = "\n".join(stripes)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!--
  {comment}
  Modern style, after the obd-display Modern dashboard's arcs: see docs/gauge-faces.md.
  Generated by tools/faces/modern_faces.py; edit that, not this. Schema: docs/gauge-config-schema.md
-->
<gauge version="1" id="{gid}">
  <panel shape="round" width="466" height="466"/>
  <source channel="{channel}" unit="{unit}" min="0" max="{vmax}" damping="{damping}"/>

  <face start-angle="{num(start)}" sweep="{num(sweep)}" background="{BG}" radius="{BAND_OUT}">
    <band from="0" to="{vmax}" color="{GUIDE}" width="2" radius="{GUIDE_R}"/>
    <band from="0" to="{vmax}" color="{TRACK}" width="{BAND_W}"/>
{stripe_xml}
    <ticks major-every="{major}" minor-every="{num(minor)}" color="{TICK}" radius="{TICK_BASE}">
      <major-shape>
        <polygon points="{MAJOR}"/>
      </major-shape>
      <minor-shape>
        <polygon points="{MINOR}"/>
      </minor-shape>
    </ticks>
    <labels every="{major}" font="{label_font}" color="{NUMERAL}" radius="{label_r}"/>
  </face>

  <arc width="{BAND_W}" color="{FILL}" start-color="{FILL_START}" glow="{GLOW}" head-color="{HEAD}" head-width="4" lit-ticks="true" lit-color="{LIT}"/>

  <icon y="{ICON_Y}" color="{px_faces.TELLTALE_GREEN}">
{px_faces.scaled(icon).replace("#000000", BG)}
  </icon>

  <needle style="none" pivot-radius="0"/>

  <readout y="{READOUT_Y}" font="{px_faces.READOUT_FONT}" format="%.0f" color="{LIT}"/>
  <title text="{caption}" y="{CAPTION_Y}" font="montserrat_22" color="{UNIT}"/>

  <peak color="{AMBER}" length="{BAND_W - 8}" width="5" value-y="{PEAK_Y}" font="montserrat_20" format="%.0f"/>
  <trend y="{TREND_Y}" width="{TREND_W}" height="{TREND_H}" line-width="3" color="{FILL}"/>
{alert_xml}
</gauge>
"""


BOOST = dict(gid="boost_modern", comment="Boost gauge, 0-30 PSI, redline from 26: after the Modern dashboard's boost arc.",
             channel="boost", unit="psi", vmax=30, major=5, minor=1,
             zones=[(26, "#e62323")], alerts=[(26, px_faces.RED, 2)], caption="psi", icon=px_faces.TURBO_ICON,
             start=360 - SWEEP / 2, sweep=SWEEP,
             label_font="montserrat_32", label_r=142)
EGT = dict(gid="egt_modern", comment="Exhaust gas temperature gauge, 0-900 C: yellow from 380, orange from 440, red above 500.",
           channel="egt", unit="C", vmax=900, major=150, minor=50,
           zones=[(380, px_faces.YELLOW), (440, px_faces.ORANGE), (500, "#e62323")],
           alerts=[(380, px_faces.YELLOW, 0), (440, px_faces.ORANGE, 0), (500, px_faces.RED, 2)],
           caption="°C", icon=px_faces.EGT_ICON,
           start=360 - SWEEP / 2, sweep=SWEEP,
           # Three-digit numerals: a size down, and pulled in so the widest stay clear of the glow.
           label_font="montserrat_26", label_r=138)


if __name__ == "__main__":
    out = ROOT / "firmware" / "assets" / "gauges"
    for spec in (BOOST, EGT):
        (out / f"{spec['gid']}.xml").write_text(face(**spec), encoding="utf-8", newline="\n")
        print("wrote", out / f"{spec['gid']}.xml")
