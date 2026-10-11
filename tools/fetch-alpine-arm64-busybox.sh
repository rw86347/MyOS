#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# Alpine aarch64 busybox-static — unmodified /init for MyOS ARM64 bring-up.
set -e
MIRROR=${MIRROR:-https://dl-cdn.alpinelinux.org/alpine/v3.20/main/aarch64}
dir=$1
name=busybox-static
ver=1.36.1-r31
sha=1d8e7a7fc2ed69bdb8eb7be9d962489c21a0f75b72d8a325437c8a09d4cfac70
mkdir -p "$dir/apk"
apk="$dir/apk/$name-$ver.apk"
if [ ! -f "$apk" ]; then
    echo "Downloading $name $ver (aarch64)..."
    curl -fsSL --max-time 300 -o "$apk.tmp" "$MIRROR/$name-$ver.apk"
    mv "$apk.tmp" "$apk"
fi
got=$(shasum -a 256 "$apk" | cut -d' ' -f1)
if [ "$got" != "$sha" ]; then
    echo "fetch-alpine-arm64-busybox: checksum mismatch" >&2
    exit 1
fi
tar -xzf "$apk" -C "$dir" 2>/dev/null
rm -f "$dir"/.PKGINFO "$dir"/.SIGN.* "$dir"/.pre-* "$dir"/.post-* "$dir"/.trigger
touch "$dir/.fetched-busybox"
echo "Alpine aarch64 busybox-static ready in $dir"
