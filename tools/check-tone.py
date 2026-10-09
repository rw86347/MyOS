#!/usr/bin/env python3
"""check-tone.py WAV HZ SECONDS — is there a tone of HZ, at least SECONDS
long, in WAV (16-bit PCM)? For make test-sound: QEMU writes what the sound
card played into the file.

Goertzel's algorithm measures one frequency's strength without a full
Fourier transform: the tone must be far stronger than two neighbours
(HZ x 0.7 and HZ x 1.4), and loud samples must cover SECONDS of the file.
"""
import math
import struct
import sys


def power(samples, rate, hz):
    k = 2 * math.cos(2 * math.pi * hz / rate)
    s1 = s2 = 0.0
    for x in samples:
        s1, s2 = x + k * s1 - s2, s1
    return s1 * s1 + s2 * s2 - k * s1 * s2


def main():
    path, hz, need = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    # RIFF by hand: QEMU writes the sizes into the header only when it shuts
    # its audio down cleanly; a size of 0 means "to the end of the file".
    try:
        data = open(path, 'rb').read()
    except OSError as e:
        print(f"FAIL: no sound recorded ({e})")
        return 1
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        print("FAIL: no sound recorded (not a WAVE file)")
        return 1
    pos, fmt, raw = 12, None, b''
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:] if cid == b'data' and size == 0 else data[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data':
            raw = body
            break
        pos += 8 + size + (size & 1)
    if not fmt:
        print("FAIL: WAV without a format")
        return 1
    _, ch, rate, _, _, bits = fmt
    if bits != 16:
        print(f"FAIL: {bits}-bit WAV, expected 16")
        return 1
    width = 2
    raw = raw[:len(raw) // (width * ch) * width * ch]
    left = struct.unpack('<%dh' % (len(raw) // 2), raw)[::ch]
    loud = [x for x in left if abs(x) > 1000]
    seconds = len(loud) / rate
    # 0.3 s of steady tone, from 50 ms after it starts (no start-up, no gap
    # between the two ways sndtest plays: a gap would smear the spectrum)
    start = next((i for i, x in enumerate(left) if abs(x) > 1000), 0) + rate // 20
    part = left[start:start + rate * 3 // 10]
    p = power(part, rate, hz)
    rest = max(power(part, rate, hz * 0.7), power(part, rate, hz * 1.4), 1.0)
    print(f"check-tone: {path}: {rate} Hz, {len(left) / rate:.2f} s, {seconds:.2f} s loud, "
          f"{hz:.0f} Hz {10 * math.log10(p / rest):.0f} dB above its neighbours")
    if seconds < need:
        print(f"FAIL: only {seconds:.2f} s of sound, expected {need}")
        return 1
    if p < 100 * rest:
        print(f"FAIL: no {hz:.0f} Hz tone")
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
