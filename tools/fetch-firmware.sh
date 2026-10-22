#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# fetch-firmware.sh — the firmware files MyOS's drivers load at run time,
# from Linux's linux-firmware collection (kernel.org): the programs the
# devices run themselves. Today: Intel's Wi-Fi (iwlwifi) for the AX201 in
# the MSI laptop, all three chip revisions it may be (docs/WIFI-PLAN.md),
# the AX210 in the GPD Pocket 4 (its .ucode and its .pnvm, the
# per-platform settings iwlwifi loads with it), and the Wireless-AC 7265
# in the GPD P2 Max.
#
#   tools/fetch-firmware.sh DIR     puts them in DIR (the image's /lib/firmware)
#
# The tag and every file's SHA-256 are pinned: a download that doesn't
# match is refused. Downloads are kept in build/ext/firmware/. The licence
# goes along with the files, as it asks.
set -e
TAG=20260916
BASE=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
CACHE=build/ext/firmware

# path in linux-firmware                          sha256
FILES="
intel/iwlwifi/iwlwifi-Qu-b0-hr-b0-77.ucode      2d3dd82853e1f5859de168fc4c3e2078703cb5c62c38ae39658c28ca5b664443
intel/iwlwifi/iwlwifi-Qu-c0-hr-b0-77.ucode      73b622e56919f0e8707448458b1ac44c5bdb19cdec40cf64f1afda030411ed17
intel/iwlwifi/iwlwifi-QuZ-a0-hr-b0-77.ucode     8e80445e8b4871492c6b690d30118cbef548cd9e0518f70e73a6f3875b7e9602
intel/iwlwifi/iwlwifi-ty-a0-gf-a0-89.ucode      cd4f8fe2242c115cf8687878c2534fbb436fbaa7172f49a1f69952a23c686ac9
intel/iwlwifi/iwlwifi-ty-a0-gf-a0.pnvm          cc4c0de4593292490459f1aa8c1606ea370a0307f5be9089ef99a0aa8e36be7a
intel/iwlwifi/iwlwifi-7265D-29.ucode            c52bcaa0595f64d5fb32b7db3dd4b832ea7bf78f1037bcafc19d9c0f5c5c617e
LICENSES/LICENCE.iwlwifi_firmware               16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5
"

dir=$1
[ -n "$dir" ] || { echo "usage: $0 DIR" >&2; exit 2; }
mkdir -p "$CACHE" "$dir"
echo "$FILES" | while read -r path sha; do
    [ -n "$path" ] || continue
    name=$(basename "$path")
    file="$CACHE/$TAG-$name"
    if [ ! -f "$file" ]; then
        echo "Downloading $name (linux-firmware $TAG)..."
        curl -fsSL --max-time 300 -o "$file.tmp" "$BASE/$path?h=$TAG"
        mv "$file.tmp" "$file"
    fi
    got=$(shasum -a 256 "$file" | cut -d' ' -f1)
    if [ "$got" != "$sha" ]; then
        echo "fetch-firmware: $name: checksum mismatch ($got), refusing it" >&2
        rm -f "$file"
        exit 1
    fi
    # Drivers ask for the plain name (iwlwifi-Qu-b0-hr-b0-77.ucode):
    # linux-firmware's own install links it there too.
    cp "$file" "$dir/$name"
done
echo "Firmware ready in $dir"
