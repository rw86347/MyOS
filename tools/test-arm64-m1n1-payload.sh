#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
set -e
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
payload=$root/build/arm64/m1n1

[ -f "$payload/kernel.bin" ] || { echo "FAIL: missing kernel.bin"; exit 1; }
[ -f "$payload/kernel.elf" ] || { echo "FAIL: missing kernel.elf"; exit 1; }
[ -f "$payload/handoff.txt" ] || { echo "FAIL: missing handoff.txt"; exit 1; }
grep -q '0x40100000' "$payload/handoff.txt" \
  || { echo "FAIL: handoff.txt missing load address"; exit 1; }
grep -q 'x0' "$payload/handoff.txt" \
  || { echo "FAIL: handoff.txt missing x0 DTB note"; exit 1; }
size=$(wc -c < "$payload/kernel.bin" | tr -d ' ')
[ "$size" -gt 100000 ] || { echo "FAIL: kernel.bin too small ($size)"; exit 1; }
echo "m1n1 payload OK: kernel.bin ${size} bytes, handoff.txt load 0x40100000"
