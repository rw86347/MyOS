#!/usr/bin/env python3
"""check-screen.py — is the console really on the screen? (make test)

QEMU's monitor command `screendump FILE` saves what the virtual screen
shows as a PPM image. We check that it is the size the display driver set
(1920x1200 by default) and that it shows console text: the kernel prints in
light grey (0xAAAAAA), a VGA palette color fbcon draws with — and nothing
else on a test screen is that exact color. A black or wrongly sized screen
fails.

usage: check-screen.py screen.ppm [WIDTHxHEIGHT]
"""
import sys


def read_ppm(path):
    data = open(path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:                      # magic, width, height, maxval
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":           # a comment line
            pos = data.index(b"\n", pos)
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    if fields[0] != b"P6" or fields[3] != b"255":
        sys.exit("check-screen: not an 8-bit P6 PPM")
    return int(fields[1]), int(fields[2]), data[pos + 1:]


def main():
    path = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 else "1920x1200"
    w, h, px = read_ppm(path)
    if f"{w}x{h}" != want:
        sys.exit(f"check-screen: screen is {w}x{h}, expected {want}")
    # Runs of 0xAA bytes: about 3 per grey pixel. (A few lines of text are
    # tens of thousands of pixels; a black screen has none.)
    grey = px.count(b"\xaa\xaa\xaa")
    if grey < 5000:
        sys.exit(f"check-screen: no console text ({grey} grey pixels)")
    print(f"check-screen: {w}x{h}, console text on screen ({grey} grey pixels)")


main()
