#!/bin/sh
# Run user/diskbench.c on Linux (Debian 12's stock i686 kernel, 6.1) in the
# same QEMU setup as MyOS's disk benchmark (tools/diskbench.sh), on one of
# the four disk types: ide, ahci, nvme, virtio. $2: the ext2 test image.
set -e
TYPE=$1
IMG=$2
W=/tmp/lnx
K=$W/boot/vmlinuz-6.1.0-50-686
MODS=$W/lib/modules/6.1.0-50-686
BIN=$(cd "$(dirname "$0")/.." && pwd)/build/bench/user/diskbench.elf
DEB=linux-image-6.1.0-50-686_6.1.176-1_i386.deb
mkdir -p $W && cd $W
if [ ! -d "$MODS" ]; then
    curl -sSfO https://deb.debian.org/debian/pool/main/l/linux-signed-i386/$DEB
    ar x $DEB data.tar.xz
    tar xf data.tar.xz ./lib/modules ./boot/vmlinuz-6.1.0-50-686
fi
case $TYPE in
    ide)    ROOTS="ata_piix sd_mod"
            DISK="-drive file=$IMG,format=raw,if=ide,index=0" ;;
    ahci)   ROOTS="ahci sd_mod"
            DISK="-device ahci,id=ahci -drive id=d0,file=$IMG,format=raw,if=none -device ide-hd,drive=d0,bus=ahci.0" ;;
    nvme)   ROOTS="nvme"
            DISK="-drive id=d0,file=$IMG,format=raw,if=none -device nvme,serial=myos0,drive=d0" ;;
    virtio) ROOTS="virtio_pci virtio_blk"
            DISK="-drive id=d0,file=$IMG,format=raw,if=none -device virtio-blk-pci,drive=d0" ;;
    *) echo "usage: $0 ide|ahci|nvme|virtio image"; exit 1 ;;
esac
rm -rf root && mkdir -p root/dev root/proc root/mnt root/m
cp "$BIN" root/init
# Load order: every module after the ones it depends on (depends= in .modinfo).
python3 - "$MODS" root $ROOTS crc32c_generic ext4 <<'EOF'
import os, re, shutil, sys
mods, root, want = sys.argv[1], sys.argv[2], sys.argv[3:]
info = {}
for d, _, fs in os.walk(mods):
    for f in fs:
        if f.endswith('.ko'):
            data = open(os.path.join(d, f), 'rb').read()
            m = re.search(rb'\x00depends=([^\x00]*)', data)
            name = f[:-3].replace('-', '_')
            info[name] = (os.path.join(d, f), [x.replace('-', '_') for x in
                          (m.group(1).decode().split(',') if m and m.group(1) else [])])
order, seen = [], set()
def visit(n):
    if n in seen:
        return
    seen.add(n)
    for dep in info[n][1]:
        visit(dep)
    order.append(n)
for n in want:
    visit(n)
with open(os.path.join(root, 'modules.list'), 'w') as out:
    for n in order:
        shutil.copy(info[n][0], os.path.join(root, 'm', n + '.ko'))
        out.write('/m/%s.ko\n' % n)
print('modules:', ' '.join(order))
EOF
(cd root && find . | cpio -o -H newc 2>/dev/null > ../disk-init.cpio)
qemu-system-i386 -m 64M -cpu pentium3 -kernel $K -initrd disk-init.cpio \
    -append "console=ttyS0 rdinit=/init quiet" $DISK -display none \
    -serial file:diskbench-$TYPE.log -icount shift=0,sleep=off -no-reboot >/dev/null 2>&1
grep diskbench diskbench-$TYPE.log
