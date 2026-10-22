#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""fakelid.py — an ACPI table with a laptop's lid, for QEMU

QEMU has no lid, so `make test-sleep` gives it one: an SSDT
(`qemu ... -acpitable file=fakelid.aml`) declaring what a laptop's
firmware would, the way the kernel's acpi_button.c reads it:

    Device (\\_SB.LID0)    _HID "PNP0C0D", _LID: LIDS (1: open, at first)
                          _PRW: wakes from S3 by GPE 0x0D (no AML handles
                          it: acpi_button.c turns it on to notify the lid)
                          TOGL: LIDS ^= 1, Notify (LID0, 0x80) -- what a
                          real lid's EC query does when it moves

Nothing on the host can run TOGL by itself; a SLEEPTEST=1 kernel runs it
when a CPU is added in QEMU's monitor (kernel/fakelid_test.c).

There is no iasl on the Mac, so the AML is written byte by byte, as in
fakebat.py (ACPI 6.5, chapter 20). Usage: fakelid.py OUT.aml
"""
import struct
import sys

OUT = sys.argv[1]


def pkglen(body):
    """PkgLength + body: the length counts its own bytes (1 to 4)."""
    n = len(body)
    if n + 1 < 64:
        return bytes([n + 1]) + body
    for extra in (1, 2, 3):
        total = n + 1 + extra
        if total < 1 << (4 + 8 * extra):
            lead = extra << 6 | (total & 0x0F)
            rest = (total >> 4).to_bytes(extra, 'little')
            return bytes([lead]) + rest + body
    raise ValueError('too long')


def name(s):
    return s.encode().ljust(4, b'_')


def integer(v):
    if v == 0:
        return b'\x00'                            # ZeroOp
    if v == 1:
        return b'\x01'                            # OneOp
    return b'\x0A' + bytes([v])                   # BytePrefix (all ours are < 256)


def string(s):
    return b'\x0D' + s.encode() + b'\x00'


def package(*items):
    return b'\x12' + pkglen(bytes([len(items)]) + b''.join(items))


def name_obj(n, value):                            # Name (n, value)
    return b'\x08' + name(n) + value


def method(n, body, args=0):                       # Method (n, args) { body }
    return b'\x14' + pkglen(name(n) + bytes([args]) + body)


def device(n, body):                               # Device (n) { body }
    return b'\x5B\x82' + pkglen(name(n) + body)


lid = device('LID0',
             name_obj('_HID', string('PNP0C0D')) +
             name_obj('LIDS', integer(1)) +
             name_obj('_PRW', package(integer(0x0D), integer(3))) +
             method('_LID', b'\xA4' + name('LIDS')) +                  # Return (LIDS)
             method('TOGL',
                    b'\x7F' + name('LIDS') + integer(1) + name('LIDS') +   # XOr (LIDS, 1, LIDS)
                    b'\x86' + name('LID0') + integer(0x80)))              # Notify (LID0, 0x80)

body = b'\x10' + pkglen(b'\x5C' + name('_SB') + lid)                   # Scope (\_SB)
header = struct.pack('<4sIBB6s8sI4sI', b'SSDT', 36 + len(body), 2, 0,
                     b'MYOS  ', b'FAKELID ', 1, b'MYOS', 1)
table = bytearray(header + body)
table[9] = (-sum(table)) & 0xFF                   # the checksum makes the sum 0
with open(OUT, 'wb') as f:
    f.write(table)
