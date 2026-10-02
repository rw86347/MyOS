#!/bin/sh
# mbr-partition.sh — write one partition entry into a disk image's MBR.
#
#   tools/mbr-partition.sh IMAGE
#
# Our boot sector doesn't need a partition table, but many PC BIOSes won't
# boot a USB stick whose table is empty (they don't treat it as a disk).
# So we add one entry covering the whole image after the MBR: marked
# active (bootable), type 0x7F ("reserved for experimental systems").
#
# Entry layout (16 bytes at offset 446): status, CHS start (3), type,
# CHS end (3), first sector (LBA, 4, little endian), sector count (4).
set -e
img=$1
total=$(( $(wc -c < "$img") / 512 ))
count=$(( total - 1 ))
oct() { printf "\\$(printf %o "$1")"; }     # one byte, as octal escape
le32() { oct $(($1 & 255)); oct $(($1 >> 8 & 255)); oct $(($1 >> 16 & 255)); oct $(($1 >> 24 & 255)); }
{
    oct 128                 # 0x80: active (the one to boot)
    oct 0; oct 2; oct 0     # CHS of sector 1: head 0, sector 2, cylinder 0
    oct 127                 # type 0x7F
    oct 254; oct 255; oct 255   # CHS end: "use the LBA fields"
    le32 1                  # starts right after the MBR
    le32 $count
} | dd of="$img" bs=1 seek=446 conv=notrunc 2>/dev/null
