#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""fakebat.py — an ACPI table with a battery and a charger, for QEMU

QEMU has no battery, so `make test-battery` gives it one: an SSDT
(`qemu ... -acpitable file=fakebat.aml`) declaring what a laptop's
firmware would, the way the kernel's battery.c reads it:

    Device (\\_SB.BAT0)    _HID "PNP0C0A", _STA 0x1F (a battery is in),
                          _BIX: 50 Wh new, 45 Wh now (90% health),
                          127 cycles, "MyOS FAKE-BAT", Li-ion
                          _BST: discharging at 9 W, 30 Wh left at
                          first, 0.1 Wh less each time it is read
    Device (\\_SB.AC)      _HID "ACPI0003", _PSR 0: on battery
    Device (\\_SB.GFX0.LCD) the panel: _BCL levels 5..100, _BCM, _BQC

There is no iasl on the Mac, so the AML is written byte by byte here
(ACPI 6.5, chapter 20, "AML Specification"). Usage: fakebat.py [--charging] OUT.aml
(--charging: plugged in, the battery charging at 9 W, for the tray's bolt)
"""
import struct
import sys

CHARGING = '--charging' in sys.argv
OUT = [a for a in sys.argv[1:] if a != '--charging'][0]


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
    if v < 0x100:
        return b'\x0A' + bytes([v])               # BytePrefix
    if v < 0x10000:
        return b'\x0B' + struct.pack('<H', v)     # WordPrefix
    return b'\x0C' + struct.pack('<I', v)         # DWordPrefix


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


def ret(x):                                        # Return (x)
    return b'\xA4' + x


LOCAL0 = b'\x60'
UNKNOWN = 0xFFFFFFFF

bix = package(
    integer(1),                 # revision
    integer(0),                 # unit: mWh
    integer(50000),             # design capacity
    integer(45000),             # last full charge
    integer(1),                 # rechargeable
    integer(11400),             # design voltage, mV
    integer(5000),              # warning level
    integer(2000),              # low level
    integer(127),               # cycle count
    integer(100000),            # accuracy
    integer(UNKNOWN), integer(UNKNOWN), integer(UNKNOWN), integer(UNKNOWN),
    integer(100), integer(100), # granularity
    string('FAKE-BAT'), string('0042'), string('LION'), string('MyOS'),
)

bst = (
    # Subtract (LEFT, 100, LEFT): a battery that runs down as it is read
    b'\x74' + name('LEFT') + integer(100) + name('LEFT') +
    # Store (Package () { 1, 9000, 0, 11100 }, Local0)
    b'\x70' + package(integer(2 if CHARGING else 1), integer(9000), integer(0), integer(11100)) + LOCAL0 +
    # Store (LEFT, Index (Local0, 2))
    b'\x70' + name('LEFT') + b'\x88' + LOCAL0 + integer(2) + b'\x00' +
    ret(LOCAL0)
)

bat = device('BAT0',
             name_obj('_HID', string('PNP0C0A')) +
             name_obj('_UID', integer(1)) +
             name_obj('LEFT', integer(30100)) +
             method('_STA', ret(integer(0x1F))) +
             method('_BIX', ret(bix)) +
             method('_BST', bst))
ac = device('AC',
            name_obj('_HID', string('ACPI0003')) +
            method('_PSR', ret(integer(1 if CHARGING else 0))))

# The laptop's panel, as firmware describes it under the graphics device:
# _BCL lists the levels it takes (the first two are its defaults on the
# charger and on battery; the rest unsorted, one twice, as real ones are),
# _BCM sets one, _BQC says which is set. backlight.c reads it.
bcl = package(*(integer(v) for v in (100, 40, 5, 20, 10, 30, 40, 50, 60, 70, 80, 90, 100, 50)))
lcd = device('LCD',
             name_obj('_ADR', integer(0x400)) +
             name_obj('BLVL', integer(100)) +
             method('_BCL', ret(bcl)) +
             method('_BCM', b'\x70' + b'\x68' + name('BLVL'), args=1) +   # Store (Arg0, BLVL)
             method('_BQC', ret(name('BLVL'))))
gfx = device('GFX0', name_obj('_ADR', integer(0x20000)) + lcd)

body = b'\x10' + pkglen(b'\x5C' + name('_SB') + bat + ac + gfx)     # Scope (\_SB)
header = struct.pack('<4sIBB6s8sI4sI', b'SSDT', 36 + len(body), 2, 0,
                     b'MYOS  ', b'FAKEBAT ', 1, b'MYOS', 1)
table = bytearray(header + body)
table[9] = (-sum(table)) & 0xFF                   # the checksum makes the sum 0
with open(OUT, 'wb') as f:
    f.write(table)
