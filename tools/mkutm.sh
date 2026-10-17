#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# mkutm.sh — wrap the boot disk image in a UTM virtual machine (macOS).
#
#   tools/mkutm.sh build/myos.img "dist/MyOS read-write.utm" [vm-uuid]
#
# The third argument is the VM's UUID. Leave it off for the usual machine.
# A trial copy (for example on the Desktop) needs its own UUID, or UTM
# treats the two folders as one machine.
#
# A .utm "file" is a folder: config.plist (the machine) and Data/ (its disks).
# Double-click it in Finder and UTM opens it. The machine is set up like an
# old PC, the way MyOS expects (values checked against UTM 4.6.4's source):
#
#   - i440FX + PIIX chipset ("pc"), Pentium III + SSE2 (the real targets
#     are a Pentium 4 / Pentium M, which have SSE2; Alpine Firefox needs
#     it), 256 MiB (1024 MiB for the live disk: Firefox will not start in
#     256), IDE disks
#   - "VGA" display = QEMU's std VGA (Bochs, 1234:1111): MyOS's DRM driver
#     switches it to 1920x1200 with a pixel console and /dev/fb0
#   - USB off and the PS/2 controller on, as the old PCs have it (their USB
#     is UHCI/EHCI, which MyOS doesn't drive; its USB keyboards, mice and
#     tablets need xHCI)
#   - an Intel e1000 network card, in one of UTM's three QEMU network modes
#     (--net, default "shared"):
#       shared    Apple's vmnet, NAT'd behind the Mac. A real router: it
#                 forwards ICMP, so `ping google.com` works. Hands out an
#                 address by DHCP, which is why MyOS needed a DHCP client
#                 before this mode was any use (net/dhcp.c).
#       emulated  QEMU's own SLIRP. 10.0.2.15/24, also by DHCP. TCP, UDP and
#                 DNS reach the internet but ICMP to anywhere except the
#                 gateway does not, because SLIRP would need a raw socket and
#                 therefore root. Ping the gateway, not google.com.
#       bridged   on the real LAN, with an address from the real router.
#   - a second IDE disk, 256 MiB of ext2: MyOS mounts it at /data, and
#     files there survive a restart
#
# The UUIDs are fixed, so a rebuilt VM is the same VM to UTM. Running this
# again on an existing VM replaces the boot disk and the settings but KEEPS
# the data disk (your files) and UTM's own files.
#
#   tools/mkutm.sh --installed build/myos-hd.img "out.utm"
#
# makes a different VM: ONE disk with the whole system on it (boot area +
# ext2 myos-root, `make hd`) — an installed MyOS, where everything you
# change survives a restart. That disk holds your files, so it's made only
# once: running this again keeps it (delete it for a fresh one).
# And with --x86_64, the 64-bit machine, for the x86_64 live disk (GPT:
# an EFI system partition and myos-root):
#
#   tools/mkutm.sh --x86_64 build/x86_64/myos-live-uefi.img "out.utm" [uuid]
#
#   - a q35 chipset booting through UEFI, QEMU's qemu64 CPU (what
#     `make test-usb` and the Chromium runs use), 4 GiB (Chromium wants it)
#   - the disk on q35's SATA controller: UTM's "IDE" on q35 is the ICH9
#     AHCI, which drivers/ahci.c runs
#   - std VGA, the e1000, PS/2 keyboard and mouse as above, and Intel HD
#     Audio with UTM's duplex codec (drivers/sound)
#   - two disks: the live system (changes kept in it), and an empty 32 GB
#     one to install MyOS on (made once; a rebuild keeps it)
#
#   tools/mkutm.sh --x86_64 --usb-stick build/x86_64/myos-live-uefi.img "out.utm"
#
# installs the way a real laptop does: the live system is a USB stick on
# UTM's USB 3 bus (a NEC xHCI), started from first, and the empty disk is
# NVMe, as in the GPD P2 Max. With USB on, UTM puts its USB keyboard, mouse
# and tablet on a *USB 2* controller of its own (QEMU's -usb: EHCI and
# UHCI), which MyOS doesn't drive, and its xHCI gets only the stick and its
# USB-sharing slots. So the machine also gets a keyboard and a tablet on
# the xHCI (QEMU sends keys to the keyboard made last, and the pointer to
# the tablet the system reads), and the tablet is how UTM hands over the
# Mac's pointer without capturing it (drivers/hid/hid.c, /dev/input/event2).
# Found 2026-10-17 from UTM's own debug log (--debug-port turns it on).
# After installing, move the NVMe disk above the USB drive in UTM's settings
# (Drives): the firmware starts the first one. (Removing the USB drive there
# deletes its image.)
set -e
installed=0
replace_disk=0
x64=0
usb_stick=0
memory=256
net=Shared
debug_port=
while :; do
    case "$1" in
    --installed) installed=1; shift ;;
    --x86_64) x64=1; installed=1; replace_disk=1; memory=4096; shift ;;
    --usb-stick) usb_stick=1; shift ;;
    # Overwrite the system disk even if this VM already exists. Default for
    # --installed is to keep it (your files). The live image is the software
    # on the disk, so `make utm-live` always replaces it.
    --replace-disk) replace_disk=1; shift ;;
    --memory) memory=$2; shift 2 ;;
    # shared | emulated | bridged; see the note above. Capitalised for the
    # plist, which is how UTM writes it.
    --net) net=$(echo "$2" | tr '[:lower:]' '[:upper:]' | cut -c1)$(echo "$2" | cut -c2-); shift 2 ;;
    # Put the QEMU monitor on 127.0.0.1:PORT and the serial console on
    # PORT+1, so the machine can be booted, read and driven from a script:
    #   nc 127.0.0.1 4445 > boot.log          the console
    #   tools/qemu-type.sh 'cmd' | nc 127.0.0.1 4444    keystrokes
    # Left out of a VM meant for a person, which should not be listening on
    # anything.
    #
    # Both over TCP, which is not the obvious choice -- a file and a unix
    # socket would be simpler. UTM runs QEMU inside the App Sandbox, and the
    # sandbox refuses to bind a unix socket ("Operation not permitted") and
    # refuses to open a file for writing anywhere outside the VM bundle,
    # including /tmp and $HOME. A listening TCP socket on localhost is the one
    # channel it allows. Found by trying the other two.
    --debug-port) debug_port=$2; shift 2 ;;
    *) break ;;
    esac
done
img=$1
out=$2
[ -f "$img" ] && [ -n "$out" ] || {
    echo "usage: $0 [--installed | --x86_64 [--usb-stick]] [--net shared|emulated|bridged] [--debug-port PORT] disk.img out.utm [uuid]" >&2
    exit 1; }
MKE2FS=${MKE2FS:-/opt/homebrew/opt/e2fsprogs/sbin/mke2fs}

if [ $usb_stick = 1 ] && [ $x64 = 0 ]; then
    echo "$0: --usb-stick is for --x86_64 (a UEFI machine)" >&2
    exit 1
fi
DISK_IF=IDE TARGET_IF=IDE USB_BUS=Disabled
if [ $usb_stick = 1 ]; then
    DISK_IF=USB TARGET_IF=NVMe USB_BUS=3.0
fi
if [ $usb_stick = 1 ]; then
    VM_UUID=${3:-8F3D6A21-C47B-4E95-9B0A-1D5E7C2F64B8}
    DISK_UUID=7D2E4A91-3B6C-4F05-9A18-E5C0B7D24F66
elif [ $x64 = 1 ]; then
    VM_UUID=${3:-0C6F4B2A-9E1D-4C37-B85A-64F2D1E0A9C3}
    DISK_UUID=7D2E4A91-3B6C-4F05-9A18-E5C0B7D24F66
elif [ $installed = 1 ]; then
    VM_UUID=${3:-9B2C4E61-7D3A-4F58-A1E6-2C8D5B0F7A93}
    DISK_UUID=3E7A9C15-52B4-4D0E-8F61-A7C2D9E04B38
else
    VM_UUID=${3:-4ED3E77A-4A46-4ABB-AA03-07F9108A6B16}
    DISK_UUID=BD3A2E96-4AD2-421D-8562-3C6563FD797D
fi
DATA_UUID=6A1F0C52-8E3B-4C1D-9F27-5B0D3E8A7C41

# The card's MAC address, derived from the VM's UUID rather than fixed.
#
# A fixed one is a trap in "shared" mode: Apple's vmnet is one network shared
# by every guest on the Mac, so two MyOS VMs started together would have the
# same address, and DHCP would hand them the same lease or neither. Bridged
# mode is worse -- that collision would be on the real LAN. Deriving it from
# the UUID keeps a rebuilt VM's address stable (UTM sees the same machine, and
# a DHCP server offers it the same lease) while making two VMs differ.
#
# 5A:4D:59 is "ZMY" and is locally administered (bit 1 of the first byte set),
# so it cannot collide with a real manufacturer's range.
mac_suffix=$(echo "$VM_UUID" | tr -d - | tr 'A-F' 'a-f' | cut -c1-6)
MAC="5A:4D:59:${mac_suffix:0:2}:${mac_suffix:2:2}:${mac_suffix:4:2}"
MAC=$(echo "$MAC" | tr 'a-f' 'A-F')
name=$(basename "$out" .utm)

mkdir -p "$out/Data"
# qcow2: UTM lets QEMU guess the disk format, and qcow2 is never guessed wrong.
# Written under another name, then renamed over the old one: a VM that is
# running keeps the disk it opened, and its next start gets the new one.
if [ $installed = 1 ] && [ $replace_disk = 0 ] && [ -f "$out/Data/$DISK_UUID.qcow2" ]; then
    echo "Kept the system disk in $out (your files); delete it for a fresh one"
else
    qemu-img convert -q -f raw -O qcow2 "$img" "$out/Data/$DISK_UUID.qcow2.new"
    mv -f "$out/Data/$DISK_UUID.qcow2.new" "$out/Data/$DISK_UUID.qcow2"
fi
DATA_DRIVE=
if [ $x64 = 1 ]; then
    # The disk to install on: empty, 32 GB, sparse (qcow2 grows as it is
    # written). Kept across rebuilds like the data disk.
    TARGET_UUID=E3B1C7A4-5D28-4F9E-8A06-2C4F7B91D5E8
    [ -f "$out/Data/$TARGET_UUID.qcow2" ] ||
        qemu-img create -q -f qcow2 "$out/Data/$TARGET_UUID.qcow2" 32G
    DATA_DRIVE="		<dict>
			<key>Identifier</key>
			<string>$TARGET_UUID</string>
			<key>ImageName</key>
			<string>$TARGET_UUID.qcow2</string>
			<key>ImageType</key>
			<string>Disk</string>
			<key>Interface</key>
			<string>$TARGET_IF</string>
			<key>InterfaceVersion</key>
			<integer>1</integer>
			<key>ReadOnly</key>
			<false/>
		</dict>"
elif [ $installed = 0 ]; then
    DATA_DRIVE="		<dict>
			<key>Identifier</key>
			<string>$DATA_UUID</string>
			<key>ImageName</key>
			<string>$DATA_UUID.qcow2</string>
			<key>ImageType</key>
			<string>Disk</string>
			<key>Interface</key>
			<string>IDE</string>
			<key>InterfaceVersion</key>
			<integer>1</integer>
			<key>ReadOnly</key>
			<false/>
		</dict>"
fi
if [ $installed = 0 ] && [ ! -f "$out/Data/$DATA_UUID.qcow2" ]; then
    tmp=$(mktemp -t myos-data)
    "$MKE2FS" -q -t ext2 -O none,filetype,sparse_super -E root_owner=0:0 \
        -L myos-data -b 4096 -F "$tmp" 256M
    qemu-img convert -q -f raw -O qcow2 "$tmp" "$out/Data/$DATA_UUID.qcow2"
    rm -f "$tmp"
    echo "New data disk (256 MiB, ext2) in $out"
fi

# The machine itself: an old PC, or (--x86_64) a modern UEFI one.
if [ $x64 = 1 ]; then
    ARCH=x86_64 CPU=qemu64 TARGET=q35 UEFI="<true/>"
    SOUND="
		<dict>
			<key>Hardware</key>
			<string>intel-hda</string>
		</dict>
	"
else
    ARCH=i386 CPU=pentium3,+sse2 TARGET=pc UEFI="<false/>" SOUND=
fi

EXTRA_ARGS=
DEBUG_LOG="<false/>"
if [ -n "$debug_port" ]; then
    # UTM's own log too (in the VM's Data/): the QEMU command line it built
    # and what QEMU printed.
    DEBUG_LOG="<true/>"
fi
if [ $usb_stick = 1 ]; then
    # The keyboard and tablet on UTM's xHCI ("usb-controller-0", UTM's name
    # for its first USB controller). It has 4 ports, and with the stick and
    # UTM's 3 USB-sharing slots there are 6 devices: QEMU would add a hub by
    # itself once down to its last free port, and MyOS has no hub driver yet
    # (docs/gaps.md, USB). 8 ports: no hub.
    EXTRA_ARGS="
			<string>-global</string>
			<string>nec-usb-xhci.p2=8</string>
			<string>-global</string>
			<string>nec-usb-xhci.p3=8</string>
			<string>-device</string>
			<string>usb-kbd,bus=usb-controller-0.0</string>
			<string>-device</string>
			<string>usb-tablet,bus=usb-controller-0.0</string>"
fi
if [ -n "$debug_port" ]; then
    serial_port=$((debug_port + 1))
    EXTRA_ARGS="$EXTRA_ARGS
			<string>-monitor</string>
			<string>tcp:127.0.0.1:$debug_port,server,nowait</string>
			<string>-serial</string>
			<string>tcp:127.0.0.1:$serial_port,server,nowait</string>"
fi

cat > "$out/config.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Backend</key>
	<string>QEMU</string>
	<key>ConfigurationVersion</key>
	<integer>4</integer>
	<key>Display</key>
	<array>
		<dict>
			<key>DownscalingFilter</key>
			<string>Linear</string>
			<key>DynamicResolution</key>
			<false/>
			<key>Hardware</key>
			<string>VGA</string>
			<key>NativeResolution</key>
			<false/>
			<key>UpscalingFilter</key>
			<string>Nearest</string>
		</dict>
	</array>
	<key>Drive</key>
	<array>
		<dict>
			<key>Identifier</key>
			<string>$DISK_UUID</string>
			<key>ImageName</key>
			<string>$DISK_UUID.qcow2</string>
			<key>ImageType</key>
			<string>Disk</string>
			<key>Interface</key>
			<string>$DISK_IF</string>
			<key>InterfaceVersion</key>
			<integer>1</integer>
			<key>ReadOnly</key>
			<false/>
		</dict>
$DATA_DRIVE
	</array>
	<key>Information</key>
	<dict>
		<key>IconCustom</key>
		<false/>
		<key>Name</key>
		<string>$name</string>
		<key>UUID</key>
		<string>$VM_UUID</string>
	</dict>
	<key>Input</key>
	<dict>
		<key>MaximumUsbShare</key>
		<integer>3</integer>
		<key>UsbBusSupport</key>
		<string>$USB_BUS</string>
		<key>UsbSharing</key>
		<false/>
	</dict>
	<key>Network</key>
	<array>
		<dict>
			<key>Hardware</key>
			<string>e1000</string>
			<key>IsolateFromHost</key>
			<false/>
			<key>MacAddress</key>
			<string>$MAC</string>
			<key>Mode</key>
			<string>$net</string>
			<key>PortForward</key>
			<array/>
		</dict>
	</array>
	<key>QEMU</key>
	<dict>
		<key>AdditionalArguments</key>
		<array>$EXTRA_ARGS
		</array>
		<key>BalloonDevice</key>
		<false/>
		<key>DebugLog</key>
		$DEBUG_LOG
		<key>Hypervisor</key>
		<false/>
		<key>PS2Controller</key>
		<true/>
		<key>RNGDevice</key>
		<false/>
		<key>RTCLocalTime</key>
		<false/>
		<key>TPMDevice</key>
		<false/>
		<key>TSO</key>
		<false/>
		<key>UEFIBoot</key>
		$UEFI
	</dict>
	<key>Serial</key>
	<array/>
	<key>Sharing</key>
	<dict>
		<key>ClipboardSharing</key>
		<false/>
		<key>DirectoryShareMode</key>
		<string>None</string>
		<key>DirectoryShareReadOnly</key>
		<false/>
	</dict>
	<key>Sound</key>
	<array>$SOUND</array>
	<key>System</key>
	<dict>
		<key>Architecture</key>
		<string>$ARCH</string>
		<key>CPU</key>
		<string>$CPU</string>
		<key>CPUCount</key>
		<integer>1</integer>
		<key>CPUFlagsAdd</key>
		<array/>
		<key>CPUFlagsRemove</key>
		<array/>
		<key>ForceMulticore</key>
		<false/>
		<key>JITCacheSize</key>
		<integer>0</integer>
		<key>MemorySize</key>
		<integer>$memory</integer>
		<key>Target</key>
		<string>$TARGET</string>
	</dict>
</dict>
</plist>
EOF
plutil -lint -s "$out/config.plist"
echo "Built $out (network: $net)"
