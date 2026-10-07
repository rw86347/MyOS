#!/bin/sh
# Disk benchmark, MyOS vs Linux, on one disk type: ide, ahci, nvme, virtio,
# usb (a USB stick on xHCI).
# Both kernels run user/diskbench.c on a copy of the same ext2 image (two
# 8 MiB files of random data), in the same QEMU (-icount, pentium3, 64 MiB).
# Usage: [MEM=20M] tools/diskbench.sh TYPE [myos|linux|both]
# (MEM smaller than the files makes the page cache give memory back.)
# BPS=62914560: the disk moves at most that many bytes/s (60 MiB/s: an old
# IDE disk streaming), so waiting for it is real.
# ICOUNT=0: real time instead of counted instructions — for streambench's
# wall-clock numbers (diskbench's then mean little).
set -e
TYPE=$1
WHO=${2:-both}
MEM=${MEM:-64M}
T=${BPS:+,throttling.bps-total=$BPS}
[ "${ICOUNT:-1}" = 0 ] && CLOCK= || CLOCK="-icount shift=0,sleep=off"
cd "$(dirname "$0")/.."
D=/tmp/dbench
if [ ! -f $D/src/big4 ]; then
    rm -f $D/disk.img
    mkdir -p $D/src
    for f in big big2 big3 big4; do head -c 8388608 /dev/urandom > $D/src/$f; done
    /opt/homebrew/opt/e2fsprogs/sbin/mke2fs -q -t ext2 -b 1024 -L myos-data -d $D/src $D/disk.img 64M
fi
[ -n "$NOMAKE" ] || make -s MODE=bench >/dev/null
IMG=$D/myos-$TYPE.img
cp $D/disk.img $IMG
case $TYPE in
    ide)    DISK="-drive file=$IMG,format=raw$T,if=ide,index=2" ;;
    ahci)   DISK="-device ahci,id=ahci -drive id=d0,file=$IMG,format=raw$T,if=none -device ide-hd,drive=d0,bus=ahci.0" ;;
    nvme)   DISK="-drive id=d0,file=$IMG,format=raw$T,if=none -device nvme,serial=myos0,drive=d0" ;;
    virtio) DISK="-drive id=d0,file=$IMG,format=raw$T,if=none -device virtio-blk-pci,drive=d0" ;;
    usb)    DISK="-device qemu-xhci,id=xhci -drive id=d0,file=$IMG,format=raw$T,if=none -device usb-storage,bus=xhci.0,drive=d0" ;;
    *) echo "usage: $0 ide|ahci|nvme|virtio|usb [myos|linux|both]"; exit 1 ;;
esac
if [ $WHO != linux ]; then
    echo "== MyOS, $TYPE"
    cp build/bench/myos.img $D/boot-$TYPE.img       # own copies: runs can go in parallel
    cp build/usb.img $D/usb-$TYPE.img
    qemu-system-i386 -m ${MEM:-64M} -cpu pentium3 \
        -drive format=raw,file=$D/boot-$TYPE.img,index=0,media=disk \
        -drive format=raw,file=$D/usb-$TYPE.img,index=1,media=disk $DISK \
        -netdev user,id=net0 -device e1000,netdev=net0 -display none \
        -serial file:$D/myos-$TYPE.log $CLOCK \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 >/dev/null 2>&1 || true
    grep -a -E "diskbench|streambench|^ata|^ahci|^nvme|^virtio|^usb-storage|mounted at /data" $D/myos-$TYPE.log || true
fi
if [ $WHO != myos ]; then
    echo "== Linux 6.1, $TYPE"
    cp $D/disk.img $D/linux-$TYPE.img
    tools/linux-diskbench.sh $TYPE $D/linux-$TYPE.img | grep -E "cycles|failed|could not|no disk"
fi
