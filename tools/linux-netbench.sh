#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# Run user/netbench.c on Linux (Debian 12's stock i686 kernel) in the same
# QEMU setup `make bench` uses, for docs/performance.md. Needs
# tools/echo-server.py running on the host. Extra kernel arguments go in $1
# (e.g. "mitigations=off").
set -e
DEB=linux-image-6.1.0-50-686_6.1.176-1_i386.deb
W=/tmp/myos-linux-netbench
BIN=$(cd "$(dirname "$0")/.." && pwd)/build/bench/user/netbench.elf
[ -f "$BIN" ] || { echo "build it first: make MODE=bench build/bench/user/netbench.elf"; exit 1; }
mkdir -p $W && cd $W
if [ ! -f boot/vmlinuz-6.1.0-50-686 ]; then
    curl -sSfO https://deb.debian.org/debian/pool/main/l/linux-signed-i386/$DEB
    ar x $DEB data.tar.xz
    tar xf data.tar.xz ./boot/vmlinuz-6.1.0-50-686 \
        ./lib/modules/6.1.0-50-686/kernel/drivers/net/ethernet/intel/e1000/e1000.ko
fi
rm -rf root && mkdir -p root/dev root/proc
cp "$BIN" root/init
cp lib/modules/6.1.0-50-686/kernel/drivers/net/ethernet/intel/e1000/e1000.ko root/
(cd root && find . | cpio -o -H newc 2>/dev/null > ../init.cpio)
qemu-system-i386 -m 64M -cpu pentium3 -kernel boot/vmlinuz-6.1.0-50-686 \
    -initrd init.cpio -append "console=ttyS0 rdinit=/init $1" \
    -netdev user,id=net0 -device e1000,netdev=net0 -display none \
    -serial file:serial.log -icount shift=0,sleep=off -no-reboot >/dev/null 2>&1
grep netbench serial.log
