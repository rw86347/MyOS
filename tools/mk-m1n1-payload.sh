#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# Stage MyOS kernel for m1n1 / Asahi-style chain load (roadmap 15b).
#
#   tools/mk-m1n1-payload.sh build/arm64/kernel.elf build/arm64/m1n1
set -e
elf=$1
dest=$2
OBJCOPY=${OBJCOPY:-aarch64-elf-objcopy}
NM=${NM:-aarch64-elf-nm}
load=0x40100000

[ -f "$elf" ] && [ -n "$dest" ] || {
    echo "usage: $0 kernel.elf out-dir" >&2
    exit 1
}

mkdir -p "$dest"
"$OBJCOPY" -O binary "$elf" "$dest/kernel.bin"
cp -f "$elf" "$dest/kernel.elf"
entry=$("$NM" "$elf" 2>/dev/null | awk '/[[:space:]]_start$/ { print $1; exit }')
cat > "$dest/handoff.txt" <<EOF
MyOS ARM64 m1n1 hand-off (step 15b)
====================================
Load:   kernel.bin or kernel.elf at physical ${load} (MMU off, EL1)
Entry:  _start = ${entry:-unknown} (must match load for raw bin)
DTB:    pass hardware device-tree physical pointer in x0 (Linux convention)
Return: none — kernel must not return to the loader

Docs: docs/hardware-apple-silicon.md
EOF
echo "m1n1 payload: $dest (kernel.bin $(wc -c < "$dest/kernel.bin" | tr -d ' ') bytes)"
