#!/usr/bin/env bash
# =============================================================================
# mkusb.sh — build a FAT32 "USB stick" disk image with test files on it
#
#   usage: tools/mkusb.sh [output.img]      (default: build/usb.img)
#   needs: mtools (brew install mtools) — works on image files, no mounting
#
# Layout, like a stick formatted by Windows/macOS/Linux:
#
#   LBA 0            MBR: boot code (empty here) + partition table + 0x55AA
#   LBA 1..2047      unused gap (partitions start at 1 MiB for alignment)
#   LBA 2048..end    partition 1, type 0x0C (FAT32 with LBA addressing)
#                    -> FAT32 file system, volume label MYOSUSB
#
# Inside the FAT32 partition:
#   [reserved sectors: boot sector (BPB), FSInfo, backup boot sector]
#   [FAT #1][FAT #2]   the File Allocation Table: one 32-bit entry per cluster,
#                      each pointing to the file's next cluster (a linked list)
#   [data clusters]    everything else, including the root directory (FAT32's
#                      root dir is an ordinary cluster chain, unlike FAT12/16)
# =============================================================================
set -euo pipefail

OUT=${1:-build/usb.img}
DISK_MB=64
PART_START=2048                                   # in 512-byte sectors (= 1 MiB)
DISK_SECTORS=$(( DISK_MB * 1024 * 1024 / 512 ))   # 131072
PART_SECTORS=$(( DISK_SECTORS - PART_START ))     # 129024

command -v mformat >/dev/null || { echo "mtools not found: brew install mtools" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PART="$WORK/part.img"

# --- 1. Format the partition as FAT32 ----------------------------------------
#  -F           FAT32 (FAT32 needs at least 65525 clusters; we get ~128000)
#  -c 1         1 sector (512 B) per cluster — small, so even small files use
#               several clusters: good for testing cluster chains
#  -T, -h, -s   total sectors and a fake geometry (64 heads x 32 sectors)
#  -H 2048      "hidden sectors": where this partition starts on the disk.
#               Stored in the boot sector; some OSes check it.
#  -v           volume label
MTOOLS_SKIP_CHECK=1 mformat -C -i "$PART" -F -c 1 -T "$PART_SECTORS" -h 64 -s 32 \
    -H "$PART_START" -v MYOSUSB ::

# --- 2. Create test files and directories -------------------------------------
# Built on the host in $WORK/root, then copied in with mcopy.
R="$WORK/root"
mkdir -p "$R/docs" "$R/src/kernel" "$R/src/drivers" "$R/data"

printf 'Hello from a FAT32 USB stick!\n' > "$R/HELLO.TXT"
cat > "$R/README.TXT" <<'EOF'
MyOS test USB image.
Directories:
  docs/          text files, including long file names
  src/           a nested tree (src/kernel, src/drivers)
  data/          larger files that span many clusters
  empty/         an empty directory
EOF

printf 'Short names are stored as 8.3 directory entries.\n' > "$R/docs/notes.txt"
printf 'Long names need extra VFAT "LFN" directory entries.\n' \
    > "$R/docs/This is a long file name.txt"
printf 'Mixed Case Name.\n' > "$R/docs/MixedCase.md"

printf 'int main(void) { return 0; }\n' > "$R/src/main.c"
printf '/* scheduler goes here */\n'    > "$R/src/kernel/sched.c"
printf '/* FAT32 driver goes here */\n' > "$R/src/drivers/fat32.c"

# 100 numbered lines (~3 KB, 6+ clusters): easy to check for corruption.
for i in $(seq 1 100); do printf 'line %03d of numbers.txt\n' "$i"; done > "$R/data/numbers.txt"
# 64 KiB of a repeating byte pattern (128 clusters).
LC_ALL=C perl -e 'print map { chr($_ % 256) } 0 .. 65535' > "$R/data/pattern.bin"

mcopy -i "$PART" -s -Q "$R"/* ::/
mmd   -i "$PART" ::/empty

# --- 3. Build the whole disk: MBR + partition ---------------------------------
dd if=/dev/zero of="$OUT" bs=512 count="$DISK_SECTORS" 2>/dev/null

# A partition-table entry is 16 bytes; entry 1 is at byte offset 446.
#   byte  0    status: 0x80 = bootable, 0x00 = not
#   1..3       start in old CHS form — 0xFE 0xFF 0xFF means "use LBA instead"
#   byte  4    partition type: 0x0C = FAT32 (LBA)
#   5..7       end in CHS form — likewise unused
#   8..11      start LBA      (32-bit little-endian)
#   12..15     sector count   (32-bit little-endian)
le32() {   # print a number as 4 little-endian bytes
    printf "\\$(printf %03o $(( $1 & 255 )))\\$(printf %03o $(( ($1 >> 8) & 255 )))"
    printf "\\$(printf %03o $(( ($1 >> 16) & 255 )))\\$(printf %03o $(( ($1 >> 24) & 255 )))"
}
{
    printf '\x00\xFE\xFF\xFF\x0C\xFE\xFF\xFF'
    le32 "$PART_START"
    le32 "$PART_SECTORS"
} | dd of="$OUT" bs=1 seek=446 conv=notrunc 2>/dev/null
printf '\x55\xAA' | dd of="$OUT" bs=1 seek=510 conv=notrunc 2>/dev/null

dd if="$PART" of="$OUT" bs=512 seek="$PART_START" conv=notrunc 2>/dev/null

echo "Built $OUT (${DISK_MB} MiB, FAT32 partition at LBA $PART_START)"
echo "List it with:  mdir -i $OUT@@1M -/ ::"
