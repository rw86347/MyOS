#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# Alpine aarch64 musl-dev — static musl test programs for MyOS ARM64 bring-up.
set -e
MIRROR=${MIRROR:-https://dl-cdn.alpinelinux.org/alpine/v3.20/main/aarch64}
dir=$1
name=musl-dev
ver=1.2.5-r3
sha=89a641400ddd298cf2b40a3b6d7d0a20278d5a01cae79fc639ca8fd27b2fdf12
mkdir -p "$dir/apk"
apk="$dir/apk/$name-$ver.apk"
if [ ! -f "$apk" ]; then
    echo "Downloading $name $ver (aarch64)..."
    curl -fsSL --max-time 300 -o "$apk.tmp" "$MIRROR/$name-$ver.apk"
    mv "$apk.tmp" "$apk"
fi
got=$(shasum -a 256 "$apk" | cut -d' ' -f1)
if [ "$got" != "$sha" ]; then
    echo "fetch-alpine-arm64-musl: checksum mismatch" >&2
    exit 1
fi
tar -xzf "$apk" -C "$dir" 2>/dev/null
rm -f "$dir"/.PKGINFO "$dir"/.SIGN.* "$dir"/.pre-* "$dir"/.post-* "$dir"/.trigger
touch "$dir/.fetched-musl"
echo "Alpine aarch64 musl-dev ready in $dir"
