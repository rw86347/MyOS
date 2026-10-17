#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""mkfont — the 8x16 VGA font the UEFI loader hands the kernel console.

  tools/mkfont.py OUT

A BIOS boot copies the BIOS's own font (stage2.asm, int 0x10 AX=0x1130).
Under UEFI there is none, so the loader reads EFI/MYOS/FONT.BIN: 256
characters (code page 437), 16 bytes each, one byte per row, bit 7 the
leftmost pixel. It comes from QEMU's VGA BIOS (SeaVGABIOS, LGPL) on the
build machine, which every build here has (QEMU runs the tests), rather
than being kept in this repo under another licence.

The table is found by its character 1, the white smiley, whose 16 rows are
the same in every VGA font since IBM's: 00 00 7E 81 A5 81 81 BD 99 81 81
7E 00 00 00 00. Character 0 before it must be blank.
"""
import os, sys

SMILEY = bytes([0x00, 0x00, 0x7E, 0x81, 0xA5, 0x81, 0x81, 0xBD,
                0x99, 0x81, 0x81, 0x7E, 0x00, 0x00, 0x00, 0x00])
PLACES = [os.environ.get("QEMU_SHARE", ""), "/opt/homebrew/share/qemu", "/usr/local/share/qemu",
          "/usr/share/qemu", "/usr/share/seabios"]
ROMS = ["vgabios-stdvga.bin", "vgabios.bin"]


def font_in(rom):
    at = rom.find(SMILEY)
    while at >= 0:
        start = at - 16
        if start >= 0 and start + 4096 <= len(rom) and rom[start:at] == bytes(16):
            return rom[start:start + 4096]
        at = rom.find(SMILEY, at + 1)
    return None


def main():
    out = sys.argv[1]
    for place in filter(None, PLACES):
        for name in ROMS:
            path = os.path.join(place, name)
            if os.path.exists(path):
                font = font_in(open(path, "rb").read())
                if font:
                    with open(out, "wb") as f:
                        f.write(font)
                    print("mkfont: %s from %s" % (out, path))
                    return
    sys.exit("mkfont: no QEMU VGA BIOS with an 8x16 font found (set QEMU_SHARE to "
             "the folder with vgabios-stdvga.bin)")


if __name__ == "__main__":
    main()
