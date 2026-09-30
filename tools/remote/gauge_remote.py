#!/usr/bin/env python3
"""Drive and inspect a gauge over HTTP: screenshots and touch gestures. Standard library only.

    gauge_remote.py HOST shot out.png          screen as PNG, masked to the round panel
                                               (--square for the full buffer, .bmp as sent)
    gauge_remote.py HOST tap X Y
    gauge_remote.py HOST long X Y [MS]
    gauge_remote.py HOST swipe up|down|left|right
    gauge_remote.py HOST swipe X1 Y1 X2 Y2 [MS]

HOST is an IP or ai-gauge-XXXX.local. Coordinates are screenshot pixels. Needs a firmware built with
CONFIG_AI_GAUGE_REMOTE_CONTROL (docs/networking.md, "Remote control").
"""
import json
import struct
import sys
import urllib.request
import zlib


def bmp_to_png(d: bytes, round_panel: bool = True) -> bytes:
    """The gauge's BMP is RGB565 with bitfields, top-down. round_panel makes the corners the
    panel physically lacks transparent, so a screenshot shows what the glass can show."""
    off, = struct.unpack_from('<I', d, 10)
    w, h = struct.unpack_from('<ii', d, 18)
    top_down, h = h < 0, abs(h)
    row = (w * 2 + 3) & ~3
    cx, cy, r2 = (w - 1) / 2, (h - 1) / 2, (min(w, h) / 2) ** 2
    raw = bytearray()
    for y in range(h):
        base = off + (y if top_down else h - 1 - y) * row
        raw.append(0)
        for x, p in enumerate(struct.unpack_from('<%dH' % w, d, base)):
            inside = not round_panel or (x - cx) ** 2 + (y - cy) ** 2 <= r2
            raw += bytes((((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63,
                          (p & 31) * 255 // 31, 255 if inside else 0))

    def chunk(t, data):
        c = struct.pack('>I', len(data)) + t + data
        return c + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))


def gesture(host: str, body: dict) -> None:
    req = urllib.request.Request(f'http://{host}/api/input', data=json.dumps(body).encode(),
                                 method='POST', headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=15) as r:
        print(r.read().decode())


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    host, cmd, a = argv[1], argv[2], argv[3:]
    if cmd == 'shot':
        with urllib.request.urlopen(f'http://{host}/api/screenshot', timeout=15) as r:
            bmp = r.read()
        out = a[0] if a else 'screenshot.png'
        square = '--square' in a
        open(out, 'wb').write(bmp if out.lower().endswith('.bmp') else bmp_to_png(bmp, not square))
        print(out)
    elif cmd == 'tap':
        gesture(host, {'type': 'tap', 'x': int(a[0]), 'y': int(a[1])})
    elif cmd == 'long':
        g = {'type': 'long_press', 'x': int(a[0]), 'y': int(a[1])}
        if len(a) > 2:
            g['ms'] = int(a[2])
        gesture(host, g)
    elif cmd == 'swipe' and len(a) == 1:
        gesture(host, {'type': 'swipe', 'dir': a[0][0]})
    elif cmd == 'swipe':
        g = {'type': 'swipe', 'x1': int(a[0]), 'y1': int(a[1]), 'x2': int(a[2]), 'y2': int(a[3])}
        if len(a) > 4:
            g['ms'] = int(a[4])
        gesture(host, g)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
