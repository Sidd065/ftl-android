#!/usr/bin/env python3
"""Make Android launcher-icon resources from the iPad app's own icon.

    python3 -I tools/game_icon.py <FTL.app> <out-res-dir>

Used by android/build-apk.sh when GAME_APP is set, so a personal build shows
the game's icon. The icon comes from the user's copy at build time and is
never stored in this repository.

iOS ships its icons as "CgBI" PNGs (headerless deflate, BGRA, premultiplied
alpha); they are decoded here before resizing.
"""
import os
import struct
import sys
import zlib

from PIL import Image

FG_PX = 432    # 108 dp adaptive-icon canvas at xxxhdpi
ICON_PX = 320  # the icon fills 80 dp of it: launcher masks crop just the corners


def png_chunks(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    i = 8
    while i < len(data):
        n, = struct.unpack('>I', data[i:i + 4])
        yield data[i + 4:i + 8], data[i + 8:i + 8 + n]
        i += 12 + n


def decode_cgbi(data):
    """Decode an Apple CgBI PNG (8-bit RGBA only) to an RGBA image."""
    width = height = None
    idat = b''
    for kind, body in png_chunks(data):
        if kind == b'IHDR':
            width, height, depth, ctype = struct.unpack('>IIBB', body[:10])
            if depth != 8 or ctype != 6:
                raise ValueError('unsupported CgBI format')
        elif kind == b'IDAT':
            idat += body
    raw = zlib.decompress(idat, -15)  # raw deflate, no zlib header
    stride = width * 4
    out = bytearray(stride * height)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        f = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    for i in range(0, len(out), 4):  # BGRA, premultiplied -> RGBA
        b, g, r, a = out[i:i + 4]
        if 0 < a < 255:
            r, g, b = (min(255, v * 255 // a) for v in (r, g, b))
        out[i:i + 4] = bytes((r, g, b, a))
    return Image.frombytes('RGBA', (width, height), bytes(out))


def load_icon(path):
    data = open(path, 'rb').read()
    if any(kind == b'CgBI' for kind, _ in png_chunks(data)):
        return decode_cgbi(data)
    return Image.open(path).convert('RGBA')


def png_size(path):
    for kind, body in png_chunks(open(path, 'rb').read()):
        if kind == b'IHDR':
            return struct.unpack('>II', body[:8])
    return (0, 0)


def main():
    app, res = sys.argv[1], sys.argv[2]
    names = [n for n in os.listdir(app) if n.startswith('AppIcon') and n.endswith('.png')]
    if not names:
        sys.exit('no AppIcon*.png in ' + app)
    best = max(names, key=lambda n: png_size(os.path.join(app, n))[0])
    icon = load_icon(os.path.join(app, best)).resize((ICON_PX, ICON_PX), Image.LANCZOS)

    # Background: the average colour along the icon's border, so any ring the
    # launcher's mask shows outside the icon blends in.
    px = icon.load()
    edge = [px[x, y] for x in range(ICON_PX) for y in (0, ICON_PX - 1)]
    edge += [px[x, y] for y in range(ICON_PX) for x in (0, ICON_PX - 1)]
    bg = tuple(sum(c[i] for c in edge) // len(edge) for i in range(3))

    fg = Image.new('RGBA', (FG_PX, FG_PX), (0, 0, 0, 0))
    off = (FG_PX - ICON_PX) // 2
    fg.paste(icon, (off, off), icon)
    os.makedirs(os.path.join(res, 'mipmap-xxxhdpi'), exist_ok=True)
    os.makedirs(os.path.join(res, 'mipmap-anydpi-v26'), exist_ok=True)
    os.makedirs(os.path.join(res, 'values'), exist_ok=True)
    fg.save(os.path.join(res, 'mipmap-xxxhdpi', 'ic_launcher_game.png'))
    with open(os.path.join(res, 'values', 'game_icon.xml'), 'w') as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
                '    <color name="ic_launcher_game_bg">#%02X%02X%02X</color>\n</resources>\n' % bg)
    with open(os.path.join(res, 'mipmap-anydpi-v26', 'ic_launcher.xml'), 'w') as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '    <background android:drawable="@color/ic_launcher_game_bg" />\n'
                '    <foreground android:drawable="@mipmap/ic_launcher_game" />\n'
                '</adaptive-icon>\n')
    print('icon: %s (%dx%d), background #%02X%02X%02X' % ((best,) + png_size(os.path.join(app, best)) + bg))


if __name__ == '__main__':
    main()
