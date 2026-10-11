#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# fetch-alpine.sh — get the Alpine Linux packages MyOS's root file system
# uses: unmodified 32-bit x86 Linux programs (MyOS speaks Linux's system
# call language, so they run as they are).
#
#   tools/fetch-alpine.sh DIR      unpacks them all into DIR (one tree)
#
# Versions and SHA-256 checksums are pinned below: a download that doesn't
# match is refused, so every build gets exactly the same files.
set -e
MIRROR=https://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86

# package               version              sha256 of the .apk
PACKAGES="
busybox-static          1.36.1-r31           7b099399505c480d16889f774bb3220b71e000bab70450b5ae904476368ee754
bash                    5.2.26-r0            88cf60707bb8667e71b57910917304542d9e64c1db9c7f427895faa9e7d51628
musl                    1.2.5-r3             7e56fb578429d6c493c3cc07a7390635a5d76dcc6e38c74fbec9279b2d68ab63
musl-dev                1.2.5-r3             307e33b04095707f39b8c2ec9224e66c11acd5510651e09a6e199ae364c68cd5
readline                8.2.10-r0            7ee65ca278c8a76dae3dcf9d253cd02b28958b20a1e44f60d358413820a82d2c
libncursesw             6.4_p20240420-r2     0d495180e37c6ac07a6c51be17e91464719857c8975c15091078d5c3b8495650
ncurses-terminfo-base   6.4_p20240420-r2     ba1c61cde706f90e4f45f9dc5feec39697808639553ead231fe9c9afdba48642
libnl3                  3.9.0-r1             ec7924a17c8db1a5c43dacfb3f2d24a1c1556ee66ad64a8790eb335e895025d2
iw                      6.9-r0               646fdefbdc35dc2a6fe2689755783587726212e987b66b67c311e89b01c4f88a
libcrypto3              3.3.7-r0             c35d5525e67404c89bad0ea32af98b26cb6cf6d3948839d5105f7f33b8c44067
libssl3                 3.3.7-r0             a9ce7d052be8281a17a1a73adb7f5c172058868394bba5bad0a9e3d693fef111
dbus-libs               1.14.10-r1           093da40c261720b7719328a1884c89988517b786608c37e6d2177029e362bab5
pcsc-lite-libs          2.2.3-r0             0840ee9b14c4766ef36eeb65c032aa1249cb106267a6e39cdb87a86d60c32d65
wpa_supplicant          2.10-r11             d2220046e883f8bf87a55e2388ad7462e199103bbe94ee1083f0dded93594a9e
"

dir=$1
mkdir -p "$dir/apk"
echo "$PACKAGES" | while read -r name version sha; do
    [ -n "$name" ] || continue
    apk="$dir/apk/$name-$version.apk"
    if [ ! -f "$apk" ]; then
        echo "Downloading $name $version from Alpine Linux..."
        curl -fsSL --max-time 300 -o "$apk.tmp" "$MIRROR/$name-$version.apk"
        mv "$apk.tmp" "$apk"
    fi
    got=$(shasum -a 256 "$apk" | cut -d' ' -f1)
    if [ "$got" != "$sha" ]; then
        echo "fetch-alpine: $name: checksum mismatch ($got), refusing it" >&2
        rm -f "$apk"
        exit 1
    fi
    tar -xzf "$apk" -C "$dir" 2>/dev/null
done
rm -f "$dir"/.PKGINFO "$dir"/.SIGN.* "$dir"/.pre-* "$dir"/.post-* "$dir"/.trigger
touch "$dir/.fetched"
echo "Alpine packages ready in $dir"
