#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# mbr-partition.sh — write the partition table into a disk image's MBR.
#
#   tools/mbr-partition.sh IMAGE
#       one entry covering the whole image after the MBR (a live disk)
#   tools/mbr-partition.sh IMAGE BOOT_SECTORS ROOT_START ROOT_SECTORS
#       entry 1: the boot area (sectors 1 .. BOOT_SECTORS-1)
#       entry 2: the system's ext2 partition (an installed MyOS)
#
# Our boot sector doesn't need a partition table, but many PC BIOSes won't
# boot a USB stick whose table is empty (they don't treat it as a disk).
# Entry 1 is marked active (bootable), type 0x7F ("reserved for
# experimental systems"): stage 2, the boot picture, the kernel and the
# initramfs live there as raw sectors. Entry 2 is type 0x83 ("Linux"),
# where the kernel looks for the ext2 file system labelled myos-root.
#
# Entry layout (16 bytes at offset 446 + 16*n): status, CHS start (3),
# type, CHS end (3), first sector (LBA, 4, little endian), sector count (4).
set -e
img=$1
oct() { printf "\\$(printf %o "$1")"; }     # one byte, as octal escape
le32() { oct $(($1 & 255)); oct $(($1 >> 8 & 255)); oct $(($1 >> 16 & 255)); oct $(($1 >> 24 & 255)); }

if [ $# -ge 4 ]; then
    boot_count=$(( $2 - 1 ))
else
    boot_count=$(( $(wc -c < "$img") / 512 - 1 ))
fi
{
    oct 128                 # 0x80: active (the one to boot)
    oct 0; oct 2; oct 0     # CHS of sector 1: head 0, sector 2, cylinder 0
    oct 127                 # type 0x7F
    oct 254; oct 255; oct 255   # CHS end: "use the LBA fields"
    le32 1                  # starts right after the MBR
    le32 $boot_count
    if [ $# -ge 4 ]; then
        oct 0                   # not active
        oct 254; oct 255; oct 255   # CHS start: "use the LBA fields"
        oct 131                 # type 0x83 (Linux)
        oct 254; oct 255; oct 255
        le32 "$3"
        le32 "$4"
    fi
} | dd of="$img" bs=1 seek=446 conv=notrunc 2>/dev/null
