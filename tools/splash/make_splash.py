#!/usr/bin/env python3
"""Generate the boot splash: the scene, the underline and the spark, as raw LVGL image data.

    python tools/splash/make_splash.py

The look is the obd-display Pi boot splash (obd-display/scripts/pi/plymouth/make-splash.py), recut
for the round panel: the Ford logo on a dark navy carbon-weave background, the tag line under it in
Ford Antenna, and an underline that draws itself under the tag line led by a spark. The animation is
app_ui_splash.c; this script only makes the pictures. Design notes: docs/boot-splash.md.

Writes, into firmware/components/app_ui/splash/:
    scene.bin      SIZE x SIZE RGB565, little-endian; the logo and tag line baked in
    bar.bin        the underline, ARGB8888; every column identical, revealed left to right
    spark.bin      the spark, ARGB8888
    splash_assets.h  sizes and the underline's position, in scene pixels
and docs/images/boot-splash.png, a preview with the underline complete.

The scene is dithered as it is quantised to RGB565: a dark navy gradient bands visibly in 16-bit
colour otherwise. Re-run this after changing anything here and commit the outputs; the firmware
build embeds the .bin files and does not need Pillow. Needs Pillow and numpy.
"""
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "firmware/components/app_ui/splash"
PREVIEW = ROOT / "docs/images/boot-splash.png"
LOGO = HERE / "ford-logo.png"          # from obd-display/scripts/pi/plymouth, transparent background
FONT = HERE / "FordAntenna-Regular.otf"

SIZE = 466                             # the panel; the firmware centres the scene on other sizes
CX = CY = SIZE / 2

TAGLINE = "Giuseppe Spec Mods"
TAGLINE_SIZE = 32
TAGLINE_TRACKING = 1.5                 # extra px between letters
LOGO_WIDTH = 290
LOGO_CENTRE_Y = 198                    # a little above centre, as on the Pi, so the pair sits centred
TAGLINE_CENTRE_Y = 302
UNDERLINE_GAP = 9                      # tag line's descent to the underline's centre
UNDERLINE_OVERHANG = 5                 # underline runs this far past each end of the text
BAR_H = 13
SPARK = 24

# The obd-display palette (make-splash.py, Styles/Colors.axaml).
NAVY_DEEP = np.array((4, 14, 30), float)
NAVY = np.array((8, 34, 72), float)
PATTERN = np.array((40, 90, 150), float)
TEXT = (235, 240, 242)                 # IpcWhite
ACCENT = (0, 190, 225)                 # IpcCyan


def background() -> np.ndarray:
    """Radial navy gradient, faint carbon weave, and a vignette into the panel's edge. Float RGB."""
    y, x = np.mgrid[0:SIZE, 0:SIZE].astype(float) + 0.5
    # Lit from just above centre, as on the Pi; the radius is the panel's, so the rim is deepest.
    t = np.clip(np.hypot(x - CX, (y - SIZE * 0.42) * 1.1) / (SIZE * 0.62), 0, 1) ** 2
    img = NAVY + (NAVY_DEEP - NAVY) * t[..., None]

    # Carbon weave: alternating short diagonal strokes in a 12 px cell, drawn faintly.
    weave = Image.new("L", (SIZE, SIZE), 0)
    d = ImageDraw.Draw(weave)
    cell = 12
    for gy in range(-1, SIZE // cell + 2):
        for gx in range(-1, SIZE // cell + 2):
            x0, y0 = gx * cell, gy * cell
            for k in range(0, cell, 3):
                if (gx + gy) % 2 == 0:
                    d.line([(x0 + k, y0), (x0 + k + cell // 2, y0 + cell // 2)], fill=255)
                else:
                    d.line([(x0 + cell // 2, y0 + k), (x0, y0 + k + cell // 2)], fill=255)
    w = np.asarray(weave.filter(ImageFilter.GaussianBlur(0.6)), float)[..., None] / 255 * 0.07
    img = img * (1 - w) + PATTERN * w

    # Vignette over the last fifth of the radius: the panel's edge is a bezel, not a crop.
    r = np.hypot(x - CX, y - CY) / (SIZE / 2)
    img *= (1 - 0.55 * np.clip((r - 0.8) / 0.2, 0, 1) ** 2)[..., None]
    return img


def logo() -> Image.Image:
    img = Image.open(LOGO).convert("RGBA")
    return img.resize((LOGO_WIDTH, round(img.height * LOGO_WIDTH / img.width)), Image.LANCZOS)


def tracked_width(text, font, tracking):
    return sum(font.getlength(ch) for ch in text) + tracking * (len(text) - 1)


def draw_tracked(draw, xy, text, font, fill, tracking):
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=font, fill=fill)
        x += font.getlength(ch) + tracking


def to_rgb565(rgb: np.ndarray) -> np.ndarray:
    """Quantise float RGB to RGB565 with a 4x4 ordered dither, so gradients do not band."""
    bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], float) / 16 - 0.5
    d = np.tile(bayer, (SIZE // 4 + 1, SIZE // 4 + 1))[:SIZE, :SIZE]
    r = np.clip(np.floor(rgb[..., 0] / 255 * 31 + 0.5 + d), 0, 31).astype(np.uint16)
    g = np.clip(np.floor(rgb[..., 1] / 255 * 63 + 0.5 + d), 0, 63).astype(np.uint16)
    b = np.clip(np.floor(rgb[..., 2] / 255 * 31 + 0.5 + d), 0, 31).astype(np.uint16)
    return (r << 11) | (g << 5) | b


def from_rgb565(px: np.ndarray) -> np.ndarray:
    r = ((px >> 11) & 31) * 255 // 31
    g = ((px >> 5) & 63) * 255 // 63
    b = (px & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.uint8)


def argb8888(img: Image.Image) -> bytes:
    """LVGL's ARGB8888 is B, G, R, A in memory."""
    a = np.asarray(img.convert("RGBA"), np.uint8)
    return a[..., [2, 1, 0, 3]].tobytes()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    scene = Image.fromarray(np.clip(background(), 0, 255).astype(np.uint8)).convert("RGBA")

    # Logo, with a soft dark shadow so it lifts off the pattern.
    mark = logo()
    lx, ly = (SIZE - mark.width) // 2, LOGO_CENTRE_Y - mark.height // 2
    shadow = Image.new("RGBA", scene.size, (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 140), (lx, ly + 5), mark.split()[3])
    scene = Image.alpha_composite(scene, shadow.filter(ImageFilter.GaussianBlur(8)))
    scene.alpha_composite(mark, (lx, ly))

    # Tag line.
    font = ImageFont.truetype(str(FONT), TAGLINE_SIZE)
    text_w = tracked_width(TAGLINE, font, TAGLINE_TRACKING)
    ascent, descent = font.getmetrics()
    tx = round((SIZE - text_w) / 2)
    ty = TAGLINE_CENTRE_Y - (ascent + descent) // 2
    draw_tracked(ImageDraw.Draw(scene), (tx, ty), TAGLINE, font, TEXT + (255,), TAGLINE_TRACKING)

    # Black outside the circle: the panel cannot show it, and a hard edge there would only matter
    # on a square panel, where the firmware's black surround meets it.
    y, x = np.mgrid[0:SIZE, 0:SIZE] + 0.5
    inside = np.clip(SIZE / 2 - np.hypot(x - CX, y - CY) + 0.5, 0, 1)[..., None]
    rgb = np.asarray(scene.convert("RGB"), float) * inside
    px565 = to_rgb565(rgb)
    (OUT / "scene.bin").write_bytes(px565.astype("<u2").tobytes())

    # Underline: a 3 px accent core with a vertical glow, every column identical.
    bar_w = round(text_w) + 2 * UNDERLINE_OVERHANG
    mid = BAR_H // 2
    column = Image.new("RGBA", (1, BAR_H), (0, 0, 0, 0))
    for yy in range(BAR_H):
        dist = abs(yy - mid)
        a = 255 if dist <= 1 else int(110 * math.exp(-((dist - 1) ** 2) / 5))
        column.putpixel((0, yy), ACCENT + (a,))
    bar = column.resize((bar_w, BAR_H), Image.NEAREST)
    (OUT / "bar.bin").write_bytes(argb8888(bar))
    bar_x = tx - UNDERLINE_OVERHANG
    bar_y = ty + ascent + descent + UNDERLINE_GAP - mid

    # Spark: a small white-hot radial glow riding the leading edge.
    spark = Image.new("RGBA", (SPARK, SPARK), (0, 0, 0, 0))
    for yy in range(SPARK):
        for xx in range(SPARK):
            r = math.hypot(xx - SPARK / 2 + 0.5, yy - SPARK / 2 + 0.5) / (SPARK / 2)
            a = max(0.0, 1 - r) ** 2.2
            c = tuple(int(ACCENT[i] + (255 - ACCENT[i]) * a) for i in range(3))
            spark.putpixel((xx, yy), c + (int(255 * a),))
    (OUT / "spark.bin").write_bytes(argb8888(spark))

    (OUT / "splash_assets.h").write_text(HEADER.format(
        size=SIZE, bar_x=bar_x, bar_y=bar_y, bar_w=bar_w, bar_h=BAR_H, spark=SPARK), newline="\n")

    # Preview: what the panel shows once the underline is complete, through a round mask.
    prev = Image.fromarray(from_rgb565(px565)).convert("RGBA")
    prev.alpha_composite(bar, (bar_x, bar_y))
    mask = Image.fromarray((inside[..., 0] * 255).astype(np.uint8))
    out = Image.new("RGBA", prev.size, (0, 0, 0, 0))
    out.paste(prev, (0, 0), mask)
    PREVIEW.parent.mkdir(parents=True, exist_ok=True)
    out.save(PREVIEW, optimize=True)
    print(f"wrote scene.bin ({SIZE}x{SIZE}), bar.bin ({bar_w}x{BAR_H} at {bar_x},{bar_y}), "
          f"spark.bin ({SPARK}x{SPARK}), splash_assets.h, {PREVIEW.relative_to(ROOT)}")


HEADER = """\
/* GENERATED by tools/splash/make_splash.py. Edit that, not this. */
#pragma once

/* All in scene pixels, from the scene's top-left corner. */
#define SPLASH_SCENE_SIZE {size}
#define SPLASH_BAR_X      {bar_x}
#define SPLASH_BAR_Y      {bar_y}
#define SPLASH_BAR_W      {bar_w}
#define SPLASH_BAR_H      {bar_h}
#define SPLASH_SPARK_SIZE {spark}
"""

if __name__ == "__main__":
    main()
