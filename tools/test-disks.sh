#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# The self-tests (make test's kernel) with the ext2 test disk on each of the
# other disk drivers: AHCI (SATA), NVMe, virtio-blk, a USB stick (xHCI).
# `make test` covers IDE. They run in parallel, each on its own copies of
# the images.
# Usage: tools/test-disks.sh      (exit status 0: all passed)
cd "$(dirname "$0")/.."
make -s MODE=test >/dev/null || exit 1
make -s test >/dev/null 2>&1                    # (re)makes build/test/data.img
D=/tmp/myos-test-disks
mkdir -p $D
for t in ahci nvme virtio usb; do
    cp build/test/data.img $D/data-$t.img
    cp build/test/myos.img $D/boot-$t.img
    cp build/usb.img $D/usb-$t.img
    case $t in
        ahci)   DISK="-device ahci,id=ahci -drive id=d0,file=$D/data-$t.img,format=raw,if=none -device ide-hd,drive=d0,bus=ahci.0" ;;
        nvme)   DISK="-drive id=d0,file=$D/data-$t.img,format=raw,if=none -device nvme,serial=t0,drive=d0" ;;
        virtio) DISK="-drive id=d0,file=$D/data-$t.img,format=raw,if=none -device virtio-blk-pci,drive=d0" ;;
        usb)    DISK="-device qemu-xhci,id=xhci -drive id=d0,file=$D/data-$t.img,format=raw,if=none -device usb-storage,bus=xhci.0,drive=d0" ;;
    esac
    ( (sleep 30; echo quit) | qemu-system-i386 -m 64M -cpu pentium3 \
        -drive format=raw,file=$D/boot-$t.img,index=0,media=disk \
        -drive format=raw,file=$D/usb-$t.img,index=1,media=disk $DISK \
        -netdev user,id=net0 -device e1000,netdev=net0 -device edu -display none \
        -serial file:$D/$t.log -monitor stdio >/dev/null 2>&1 ) &
done
wait
ok=0
for t in ahci nvme virtio usb; do
    L=$D/$t.log
    if grep -q "mounted at /data" $L && grep -q "ext2test: all [0-9]* checks passed" $L &&
       grep -q "fstest /data/fst: all [0-9]* checks passed" $L &&
       grep -q "Self-tests: [0-9]* passed, 0 failed" $L; then
        echo "$t: OK"
    else
        echo "$t: FAILED (see $L)"; grep -a "FAIL" $L; ok=1
    fi
done
exit $ok
