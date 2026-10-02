#!/bin/sh
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
readline                8.2.10-r0            7ee65ca278c8a76dae3dcf9d253cd02b28958b20a1e44f60d358413820a82d2c
libncursesw             6.4_p20240420-r2     0d495180e37c6ac07a6c51be17e91464719857c8975c15091078d5c3b8495650
ncurses-terminfo-base   6.4_p20240420-r2     ba1c61cde706f90e4f45f9dc5feec39697808639553ead231fe9c9afdba48642
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
