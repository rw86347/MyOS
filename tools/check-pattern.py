#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# Is the file (dumped from the test disk by debugfs) exactly what
# user/ext2test.c section 6 wrote? Exits 0 if so.
import sys

data = open(sys.argv[1], "rb").read()


def expect(pos):
    if pos == 200000:
        return ord("Z")
    if pos >= 123457:
        return 0
    if 5000 <= pos < 14000:
        return 0xAA
    return ((pos ^ (pos >> 9)) * 31) & 0xFF


if len(data) != 200001:
    sys.exit(f"pattern.bin: {len(data)} bytes, want 200001")
for pos, b in enumerate(data):
    if b != expect(pos):
        sys.exit(f"pattern.bin: byte {pos} is {b:#x}, want {expect(pos):#x}")
