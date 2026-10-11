#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# Wrap the ARM64 kernel ELF in a native UTM virt machine (Apple Silicon).
#
#   tools/mkutm-arm64.sh build/arm64/kernel.elf "$HOME/Desktop/MyOS ARM64.utm"
set -e
kernel=$1
out=$2
[ -f "$kernel" ] && [ -n "$out" ] || {
    echo "usage: $0 kernel.elf out.utm [uuid]" >&2
    exit 1
}
VM_UUID=${3:-A1B2C3D4-E5F6-4789-ABCD-0123456789AB}
name=$(basename "$out" .utm)
mkdir -p "$out/Data"
cp -f "$kernel" "$out/Data/kernel.elf"
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
			<key>Hardware</key>
			<string>virtio-ramfb</string>
		</dict>
	</array>
	<key>Drive</key>
	<array/>
	<key>Information</key>
	<dict>
		<key>Name</key>
		<string>$name</string>
		<key>UUID</key>
		<string>$VM_UUID</string>
	</dict>
	<key>Input</key>
	<dict>
		<key>UsbBusSupport</key>
		<string>Disabled</string>
	</dict>
	<key>Network</key>
	<array/>
	<key>QEMU</key>
	<dict>
		<key>AdditionalArguments</key>
		<array>
			<string>-kernel</string>
			<string>kernel.elf</string>
		</array>
		<key>Hypervisor</key>
		<true/>
		<key>UEFIBoot</key>
		<false/>
		<key>RNGDevice</key>
		<false/>
	</dict>
	<key>Serial</key>
	<array>
		<dict>
			<key>Mode</key>
			<string>Terminal</string>
			<key>Target</key>
			<string>Auto</string>
		</dict>
	</array>
	<key>Sharing</key>
	<dict>
		<key>DirectoryShareMode</key>
		<string>None</string>
	</dict>
	<key>Sound</key>
	<array/>
	<key>System</key>
	<dict>
		<key>Architecture</key>
		<string>aarch64</string>
		<key>CPU</key>
		<string>host</string>
		<key>CPUCount</key>
		<integer>1</integer>
		<key>MemorySize</key>
		<integer>256</integer>
		<key>Target</key>
		<string>virt</string>
	</dict>
</dict>
</plist>
EOF
plutil -lint -s "$out/config.plist"
echo "Built $out (ARM64 QEMU virt, serial terminal, -kernel kernel.elf)"
