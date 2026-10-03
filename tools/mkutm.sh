#!/bin/sh
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
#   - i440FX + PIIX chipset ("pc"), Pentium III CPU, 256 MiB, IDE disks
#   - "VGA" display = QEMU's std VGA (Bochs, 1234:1111): MyOS's DRM driver
#     switches it to 1920x1200 with a pixel console and /dev/fb0
#   - USB off and the PS/2 controller on: with USB on, UTM gives the machine
#     a USB keyboard (and with "PS/2" unticked removes the PS/2 controller
#     altogether) — MyOS only has a PS/2 keyboard driver so far
#   - an Intel e1000 network card, for step 10
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
set -e
installed=0
if [ "$1" = --installed ]; then
    installed=1
    shift
fi
img=$1
out=$2
[ -f "$img" ] && [ -n "$out" ] || { echo "usage: $0 [--installed] disk.img out.utm" >&2; exit 1; }
MKE2FS=${MKE2FS:-/opt/homebrew/opt/e2fsprogs/sbin/mke2fs}

if [ $installed = 1 ]; then
    VM_UUID=${3:-9B2C4E61-7D3A-4F58-A1E6-2C8D5B0F7A93}
    DISK_UUID=3E7A9C15-52B4-4D0E-8F61-A7C2D9E04B38
else
    VM_UUID=${3:-4ED3E77A-4A46-4ABB-AA03-07F9108A6B16}
    DISK_UUID=BD3A2E96-4AD2-421D-8562-3C6563FD797D
fi
DATA_UUID=6A1F0C52-8E3B-4C1D-9F27-5B0D3E8A7C41
name=$(basename "$out" .utm)

mkdir -p "$out/Data"
# qcow2: UTM lets QEMU guess the disk format, and qcow2 is never guessed wrong.
# Written under another name, then renamed over the old one: a VM that is
# running keeps the disk it opened, and its next start gets the new one.
if [ $installed = 1 ] && [ -f "$out/Data/$DISK_UUID.qcow2" ]; then
    echo "Kept the system disk in $out (your files); delete it for a fresh one"
else
    qemu-img convert -q -f raw -O qcow2 "$img" "$out/Data/$DISK_UUID.qcow2.new"
    mv -f "$out/Data/$DISK_UUID.qcow2.new" "$out/Data/$DISK_UUID.qcow2"
fi
DATA_DRIVE=
if [ $installed = 0 ]; then
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
			<string>IDE</string>
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
		<string>Disabled</string>
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
			<string>5A:4D:59:4F:53:01</string>
			<key>Mode</key>
			<string>Shared</string>
			<key>PortForward</key>
			<array/>
		</dict>
	</array>
	<key>QEMU</key>
	<dict>
		<key>AdditionalArguments</key>
		<array/>
		<key>BalloonDevice</key>
		<false/>
		<key>DebugLog</key>
		<false/>
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
		<false/>
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
	<array/>
	<key>System</key>
	<dict>
		<key>Architecture</key>
		<string>i386</string>
		<key>CPU</key>
		<string>pentium3</string>
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
		<integer>256</integer>
		<key>Target</key>
		<string>pc</string>
	</dict>
</dict>
</plist>
EOF
plutil -lint -s "$out/config.plist"
echo "Built $out"
