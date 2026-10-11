#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""Turn assets/myos-logo.png into a 320x200 VGA mode-13h bitmap.

Output is exactly 64000 bytes, one byte per pixel, row by row. Each byte is
a grey level 0..15. Stage 2 programs the VGA palette so those levels are an
even black-to-white ramp. The picture is scaled to fit and centered; the
bars around it stay black.
"""
import struct
import sys
import zlib

W, H = 320, 200


def unfilter(typ, row, prev, bpp):
    out = bytearray(len(row))
    for i, v in enumerate(row):
        a = out[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        if typ == 0:
            x = v
        elif typ == 1:
            x = v + a
        elif typ == 2:
            x = v + b
        elif typ == 3:
            x = v + ((a + b) // 2)
        elif typ == 4:
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            x = v + (a if pa <= pb and pa <= pc else b if pb <= pc else c)
        else:
            raise SystemExit(f"PNG filter {typ} is not supported")
        out[i] = x & 255
    return out


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path} is not a PNG")
    pos, w, h, depth, color, interlace = 8, 0, 0, 0, 0, 0
    idat = b""
    while pos + 8 <= len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, color, comp, filt, interlace = struct.unpack(">IIBBBBB", chunk)
            if comp or filt or interlace or depth != 8 or color not in (2, 6):
                raise SystemExit("logo PNG must be 8-bit RGB or RGBA, non-interlaced")
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    bpp = 4 if color == 6 else 3
    stride = w * bpp
    rows, i, prev = [], 0, bytearray(stride)
    for _ in range(h):
        prev = unfilter(raw[i], raw[i + 1:i + 1 + stride], prev, bpp)
        rows.append(prev)
        i += 1 + stride
    return w, h, bpp, rows


def lum_at(row, x, bpp):
    px = row[x * bpp:(x + 1) * bpp]
    r, g, b = px[0], px[1], px[2]
    a = px[3] if bpp == 4 else 255
    return ((r * 3 + g * 6 + b) // 10) * a // 255


def content_box(rows, sw, sh, bpp):
    """The mark, not the black margin around it. A little padding stays."""
    top = left = 10**9
    bot = right = -1
    for y, row in enumerate(rows):
        for x in range(sw):
            if lum_at(row, x, bpp) > 16:
                if y < top:
                    top = y
                if y > bot:
                    bot = y
                if x < left:
                    left = x
                if x > right:
                    right = x
    if bot < 0:
        return 0, 0, sw, sh
    pad_x = max(1, (right - left) // 10)
    pad_y = max(1, (bot - top) // 5)
    return (max(0, left - pad_x), max(0, top - pad_y),
            min(sw, right + 1 + pad_x), min(sh, bot + 1 + pad_y))


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: mksplash.py logo.png splash.bin")
    sw, sh, bpp, rows = read_png(sys.argv[1])
    x0, y0, x1, y1 = content_box(rows, sw, sh, bpp)
    cw, ch = x1 - x0, y1 - y0
    # Fit the mark inside 320x200. Each destination pixel averages the
    # source box it covers, so a thin line survives the shrink.
    scale = min(W / cw, H / ch)
    dw, dh = max(1, int(cw * scale)), max(1, int(ch * scale))
    ox, oy = (W - dw) // 2, (H - dh) // 2
    out = bytearray(W * H)
    for y in range(dh):
        sy0 = y0 + int(y / scale)
        sy1 = min(y1, y0 + int((y + 1) / scale) + 1)
        for x in range(dw):
            sx0 = x0 + int(x / scale)
            sx1 = min(x1, x0 + int((x + 1) / scale) + 1)
            acc = n = 0
            for sy in range(sy0, sy1):
                row = rows[sy]
                for sx in range(sx0, sx1):
                    acc += lum_at(row, sx, bpp)
                    n += 1
            out[(oy + y) * W + ox + x] = ((acc // n) * 16) // 256
    open(sys.argv[2], "wb").write(out)


if __name__ == "__main__":
    main()
