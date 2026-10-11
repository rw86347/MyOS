#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# Alpine aarch64 bash + musl + readline for ARM64 UTM login.
set -e
MIRROR=${MIRROR:-https://dl-cdn.alpinelinux.org/alpine/v3.20/main/aarch64}
dir=$1
PACKAGES="
bash                    5.2.26-r0            081b0a361203d37732fdc65412c9bb2d5944e7dc198c76d23b4cb30743d8c461
musl                    1.2.5-r3             e455c49c6c3de1dfcd4b9867c35097f588de2fb01a939c77ddb149f8e6086a24
readline                8.2.10-r0            9e69da5ea8ff8c0bdde1ffa272ec4287684eebda8fdfe0f4a45d842d0ff787e6
libncursesw             6.4_p20240420-r2     0def98cbfab5e85e4ddfacd85840d79039f60a7e9a7717294ffa68a9c63370d8
ncurses-terminfo-base   6.4_p20240420-r2     82141d0035a58317791e139a0b68c0837c98c0836bc0bafb10bb16adf0bcc7bd
"
mkdir -p "$dir/apk"
echo "$PACKAGES" | while read -r name version sha; do
    [ -n "$name" ] || continue
    apk="$dir/apk/$name-$version.apk"
    if [ ! -f "$apk" ]; then
        echo "Downloading $name $version (aarch64)..."
        curl -fsSL --max-time 300 -o "$apk.tmp" "$MIRROR/$name-$version.apk"
        mv "$apk.tmp" "$apk"
    fi
    got=$(shasum -a 256 "$apk" | cut -d' ' -f1)
    if [ "$got" != "$sha" ]; then
        echo "fetch-alpine-arm64-root: $name checksum mismatch" >&2
        exit 1
    fi
    tar -xzf "$apk" -C "$dir" 2>/dev/null
done
rm -f "$dir"/.PKGINFO "$dir"/.SIGN.* "$dir"/.pre-* "$dir"/.post-* "$dir"/.trigger
touch "$dir/.fetched-root"
echo "Alpine aarch64 bash root ready in $dir"
