#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# fetch-alpine64.sh — Alpine x86_64 busybox-static for the 64-bit initramfs.
set -e
MIRROR=${MIRROR:-https://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86_64}
dir=$1
name=busybox-static
ver=1.36.1-r31
sha=1ca8a2894b073edea2a04231f335ad48795f92617c5eacae59458c51202068ed
mkdir -p "$dir/apk"
apk="$dir/apk/$name-$ver.apk"
if [ ! -f "$apk" ]; then
    echo "Downloading $name $ver (x86_64)..."
    curl -fsSL --max-time 300 -o "$apk.tmp" "$MIRROR/$name-$ver.apk"
    mv "$apk.tmp" "$apk"
fi
got=$(shasum -a 256 "$apk" | cut -d' ' -f1)
if [ "$got" != "$sha" ]; then
    echo "fetch-alpine64: checksum mismatch" >&2
    exit 1
fi
tar -xzf "$apk" -C "$dir" 2>/dev/null
touch "$dir/.fetched64"
