#!/bin/sh
# Disk benchmark, MyOS vs Linux, on one disk type: ide, ahci, nvme, virtio.
# Both kernels run user/diskbench.c on a copy of the same ext2 image (two
# 8 MiB files of random data), in the same QEMU (-icount, pentium3, 64 MiB).
# Usage: [MEM=20M] tools/diskbench.sh TYPE [myos|linux|both]
# (MEM smaller than the files makes the page cache give memory back.)
set -e
TYPE=$1
WHO=${2:-both}
MEM=${MEM:-64M}
cd "$(dirname "$0")/.."
D=/tmp/dbench
if [ ! -f $D/disk.img ]; then
    mkdir -p $D/src
    head -c 8388608 /dev/urandom > $D/src/big
    head -c 8388608 /dev/urandom > $D/src/big2
    /opt/homebrew/opt/e2fsprogs/sbin/mke2fs -q -t ext2 -b 1024 -L myos-data -d $D/src $D/disk.img 64M
fi
[ -n "$NOMAKE" ] || make -s MODE=bench >/dev/null
IMG=$D/myos-$TYPE.img
cp $D/disk.img $IMG
case $TYPE in
    ide)    DISK="-drive file=$IMG,format=raw,if=ide,index=2" ;;
    ahci)   DISK="-device ahci,id=ahci -drive id=d0,file=$IMG,format=raw,if=none -device ide-hd,drive=d0,bus=ahci.0" ;;
    nvme)   DISK="-drive id=d0,file=$IMG,format=raw,if=none -device nvme,serial=myos0,drive=d0" ;;
    virtio) DISK="-drive id=d0,file=$IMG,format=raw,if=none -device virtio-blk-pci,drive=d0" ;;
    *) echo "usage: $0 ide|ahci|nvme|virtio [myos|linux|both]"; exit 1 ;;
esac
if [ $WHO != linux ]; then
    echo "== MyOS, $TYPE"
    cp build/bench/myos.img $D/boot-$TYPE.img       # own copies: runs can go in parallel
    cp build/usb.img $D/usb-$TYPE.img
    qemu-system-i386 -m ${MEM:-64M} -cpu pentium3 \
        -drive format=raw,file=$D/boot-$TYPE.img,index=0,media=disk \
        -drive format=raw,file=$D/usb-$TYPE.img,index=1,media=disk $DISK \
        -netdev user,id=net0 -device e1000,netdev=net0 -display none \
        -serial file:$D/myos-$TYPE.log -icount shift=0,sleep=off \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 >/dev/null 2>&1 || true
    grep -a -E "diskbench|^ata|^ahci|^nvme|^virtio|mounted at /data" $D/myos-$TYPE.log || true
fi
if [ $WHO != myos ]; then
    echo "== Linux 6.1, $TYPE"
    cp $D/disk.img $D/linux-$TYPE.img
    tools/linux-diskbench.sh $TYPE $D/linux-$TYPE.img | grep -E "cycles|failed|could not|no disk"
fi
