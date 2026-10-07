# Hardware requirements: MSI Katana GF66 11UE

The laptop already used for x86_64 bring-up. Click BIOS 5 (the screen this
note is from) prints:

- Marketing name: **Katana GF66 11UE**
- Model / board: **MS-1581**
- Firmware: **MSI Click BIOS 5**, 64-bit UEFI only (no CSM)

That is enough to name the platform. Chip IDs below are from public
**11UE** probes of the same board (linux-hardware.org `#8f4e6cf713` and
`#ca68b014a7`, MSI subsystem `1462:12ea`) plus MSI's 11UE spec sheet.
The Intel, NVIDIA, Realtek, and AX201 functions do not change between
11UE units. The NVMe vendor, webcam, and touchpad chip can: confirm
with `lspci -nn` / `lsusb` on this machine when a Linux stick is handy.

This is a second target next to the GPD Pocket 4
([hardwareRequirements.md](hardwareRequirements.md)). Same rule: driver
paths are Linux **v7.2**; the shim in [DESIGN.md](DESIGN.md) is what we
compile those sources against. Native drivers we already have (PS/2
keyboard, NVMe, GOP/`simplefb`) stay native.

What MyOS has already seen on **this** unit is called out as such.

## The machine

- **CPU:** Intel Core i7-11800H (Tiger Lake-H, family 6 model 141).
  8 cores / 16 threads, 2.3–4.6 GHz, 24 MiB cache, 45 W. Some 11U-family
  SKUs ship an i5-11400H; the rest of the board is the same.
- **Chipset:** Intel HM570 (Tiger Lake-H PCH). LPC/ISA bridge `8086:438b`.
- **RAM:** 2× SODIMM, DDR4-3200. Typical 16 GB (8+8). Max 64 GB.
- **Screen:** 15.6″ 1920×1080 IPS, 144 Hz, not a touchscreen. Panel seen
  on a 11UE: AUO B156HAN08.4. Wired to the **Intel** iGPU in the probes
  (Optimus). GOP on this unit is at `0x400000000`.
- **iGPU:** Intel UHD Graphics (Tiger Lake-H GT1, 32 EUs). PCI
  `8086:9a60` (some units `8086:9a68`).
- **dGPU:** NVIDIA GeForce RTX 3060 Laptop (GA106M), 6 GB GDDR6, up to
  95 W. PCI `10de:2520`, subsystem `1462:12ea`. HDMI audio on the same
  card: `10de:228e`.
- **SSD:** M.2 NVMe (one or two slots; SATA AHCI is also on the PCH).
  Brand varies (Kingston, Sandisk/WD, Micron, Phison all show up).
- **Wi-Fi / Bluetooth:** Intel Wi-Fi 6 AX201 (CNVi). Wi-Fi PCI
  `8086:43f0` subsystem `8086:0074`. Bluetooth USB `8087:0026`.
- **Ethernet:** Realtek RTL8111/8168 Gigabit, PCI `10ec:8168` rev 15,
  subsystem `1462:12ea`.
- **Audio:** Intel Tiger Lake-H HD Audio `8086:43c8` (Linux binds
  `snd_sof_pci_intel_tgl` and/or `snd-hda-intel`). Speakers 2× 2 W.
  Combo 3.5 mm jack. Nahimic is Windows-only; ignore it.
- **Keyboard:** 99-key, red backlight, anti-ghost. **AT Translated Set 2**
  on the i8042 (`IRQ 1`). Not USB.
- **Touchpad:** I2C HID (`PNP0C50`) on Intel LPSS I2C `8086:43e8`. Common
  chips: Synaptics `06CB:7E7E`, or ELAN `04F3:30AA`.
- **Webcam:** USB UVC, 720p. One 11UE used Luxvisions `30c9:0042`.
- **Ports:** 2× USB 3.2 Gen1, 1× USB 2.0, 1× USB-C 3.2 Gen1 (not
  Thunderbolt), HDMI 4K@60, RJ45, combo audio. No SD reader. No COM1 —
  found on this unit 2026-10-06 when the serial ring filled.
- **Power:** 3-cell 53 Wh battery, 180 W barrel adapter.
- **Size:** about 359 × 259 × 25 mm, 2.1 kg.

It boots 64-bit UEFI. The i386 BIOS/MBR loader cannot start it.

## What “use this laptop” means

A usable MyOS session on this machine is: firmware hands us a
framebuffer, the built-in keyboard types, the live stick (or the
internal SSD) is the root disk, the pointer moves, the network works,
the battery and lid are visible, sound comes out of the speakers, and
the machine can sleep and power off. The RTX 3060 is **not** on that
list. GOP + the Intel iGPU already paint the lid.

## What a driver section means

Same five facts as the Pocket 4 note:

- How it attaches (PCI, USB, I2C, ACPI, or the i8042).
- The struct it fills in.
- Kernel services the shim (or our native driver) has to provide.
- Closed firmware the open source loads, when it loads any.
- The node a program opens.

## Shared services

[DESIGN.md](DESIGN.md) already gives drivers MMIO, I/O ports, IRQs, and
DMA buffers. This laptop adds the following, and every leaf driver
assumes them.

- **Long mode and UEFI.** ACPI tables, DMI, and PCI come from firmware.
  There is no BIOS `int 0x13` boot. Done on this unit: `BOOTX64.EFI`
  starts, GOP at `0x400000000` is mapped, `simplefb` has drawn 1920×1080.
- **PCI config space and BARs.** `pci_register_driver` / `pci_driver.probe`.
- **MSI and MSI-X**, not only a pin interrupt. xHCI, NVMe, iwlwifi, and
  r8169 all want `pci_alloc_irq_vectors`. Our NVMe driver still uses INTx
  (`docs/gaps.md`).
- **The DMA API.** `dma_map_single`, `dma_map_page`, coherent buffers.
- **`request_firmware`.** i915 (optional DMC), iwlwifi, btintel, r8169,
  and SOF all load closed blobs. The driver source is open. The blobs
  are not.
- **A USB host stack.** The live stick, Bluetooth, and the webcam sit
  behind xHCI. See USB.
- **ACPI and I2C.** The touchpad is an I2C HID device named by ACPI.
  Battery, AC, lid, and the EC are ACPI devices.
- **Timers that are not the 8254 PIT.** A Tiger Lake board is not
  required to have a usable PIT. Plan on the **local APIC timer** and
  **HPET**. There is also **no UART**: console must be the screen
  (`docs/gaps.md`, serial ring).
- **IOAPIC / APIC.** PIC IRQ 1 still reaches the i8042 on many boards.
  Device IRQs for NVMe, xHCI, and the NICs will not stay on the 8259.

## Display: GOP / simplefb (already the lid)

This is how the laptop shows a picture **today**.

- Source: UEFI Graphics Output Protocol, left in bootinfo by
  `arch/x86_64/boot/efi/efi_main.c`.
- On this unit: 1920×1080, framebuffer at `0x400000000` (the iGPU's
  64-bit BAR). `simplefb` (`drivers/video/simplefb.c`) and `fbcon`
  already draw it.
- Userspace: `/dev/fb0`. X maps that node. No modeset, no 144 Hz, no
  backlight, no HDMI.

Enough for login and a desktop. Not enough for brightness
([power-performance-settings.md](power-performance-settings.md)) or an
external monitor.

## GPU: i915 (Intel UHD, the panel)

- IDs: `8086:9a60` or `8086:9a68`, class Display VGA.
- Driver: `i915`, `drivers/gpu/drm/i915/i915_pci.c` (v7.2 still uses
  this name; later Linux split Xe).
- Attach: `pci_driver i915_pci_driver`. Tiger Lake-H GT1 is a named
  device id, not a class match.
- Struct: `drm_driver`. Features: GEM, modeset, atomic. Ioctls are the
  i915 GEM/context table.
- Shim: Kconfig `DRM_I915` depends on `DRM`, `PCI`, `INTEL_GTT`. Probe
  needs PCI BARs (one is the 256 MiB range at `0x4000000000` on the
  probes — same neighbourhood as our GOP), MSI, the DMA API, ACPI for
  the eDP panel and backlight, and optionally `request_firmware` for
  `i915/tgl_dmc_ver2_*.bin` (and GuC/HuC if enabled). Those blobs are
  closed.
- Userspace: `/dev/dri/card0` and the render node. Backlight shows up
  as `/sys/class/backlight/intel_backlight/`.

GOP/`simplefb` can stay until this probes. Then i915 owns the CRTC and
we stop blitting a shadow.

## GPU: NVIDIA GA106 (not required to use the laptop)

- ID: `10de:2520` (RTX 3060 Laptop).
- Open driver: `nouveau`, `drivers/gpu/drm/nouveau/`. Ampere (GA10x)
  support in v7.2 is incomplete: the lid already works without it.
- Closed driver: NVIDIA's proprietary module. No source to aim the shim
  at.
- HDMI on this chassis is **often wired only to the NVIDIA card**
  (Optimus). The internal panel is Intel. So: desktop without a dGPU
  driver; a TV on the HDMI jack may need nouveau or the blob, or a
  USB-C DisplayPort adapter if the Type-C pins carry it (this SKU's
  Type-C is USB 3.2 Gen1 — do not assume DP alt-mode).
- HDMI audio function `10de:228e` is `snd-hda-intel` on that BAR. Ignore
  until HDMI works.

Leave this until after i915, xHCI, and Wi-Fi.

## Keyboard: i8042 (already have a driver)

- Device: AT Translated Set 2 keyboard on the 8042, IRQ 1.
- Native driver: `drivers/keyboard.c` (and `drivers/evdev.c` as
  `/dev/input/event0`). Same path as the old PCs.
- The AUX port (IRQ 12) is a PS/2 mouse on some units; the real
  touchpad is I2C (below). `drivers/mouse.c` may see a ghost pointer.
- Shim: none. Confirm IRQ 1 still arrives (PIC or IOAPIC). Fn-row keys
  (brightness, airplane, touchpad toggle) are EC/WMI; they can wait.

This is the difference from the Pocket 4: that keyboard is USB HID.
This one is the 8042. A USB keyboard on a port still needs xHCI.

## USB host: xhci-pci

**This was the live-stick blocker.** The only USB controller this unit
showed is xHCI `8086:43ed`. Without it, the ext2 root on the stick is
invisible and process 1 is the RAM initramfs (`docs/LOG.md`, 2026-10-05).
Since 2026-10-07 MyOS has its own xHCI and USB stick driver
(`drivers/usb/`, `docs/USB-PLAN.md`); first run on this machine pending (U4).

- IDs:
  - `8086:43ed` — Tiger Lake-H PCH xHCI (the ports on the sides).
  - `8086:9a17` — CPU Thunderbolt USB xHCI (present on the SoC; this
    SKU's Type-C is not Thunderbolt, so it may have no ports).
- Driver: `xhci_pci` / `xhci_hcd`, `drivers/usb/host/xhci-pci.c`.
- Attach: `pci_driver xhci_pci_driver`. The ID table is one class match,
  `PCI_DEVICE_CLASS(PCI_CLASS_SERIAL_USB_XHCI, ~0)`. Both functions are
  claimed that way.
- Struct: a USB host controller (`usb_hcd`). `probe` is `xhci_pci_probe`.
- Shim: PCI BARs, MSI-X, DMA for USB transfer buffers.
- Userspace: device nodes after the host enumerates. The live stick is
  `usb-storage` / `uas` on this controller (see Storage).

## Storage: NVMe, AHCI, and USB SCSI

The OS has to speak all three. They are not interchangeable. Same
wording as the Pocket 4 note, with this board's IDs.

- **NVMe** is the internal SSD. Class `PCI_CLASS_STORAGE_EXPRESS`.
  Native driver: `drivers/nvme.c` (`nvme0n1`, one I/O queue, INTx).
  Linux: `nvme` in `drivers/nvme/host/pci.c`. Needs MSI/MSI-X and the
  DMA API for more than one command in flight. No firmware file. BIOS
  must not be in Intel RST/RAID mode or the SSD is hidden behind a
  different class.
- **AHCI** is the PCH SATA controller `8086:43d3` (`ahci`,
  `drivers/ata/ahci.c`). Empty unless a SATA drive is in the second
  bay. We already have a small AHCI driver for QEMU.
- **SCSI over USB** is the live stick and any USB disk. `usb-storage`
  (`drivers/usb/storage/usb.c`) and `uas` (`drivers/usb/storage/uas.c`),
  then `sd`. Needs the USB host section first. Node: `/dev/sdX`.

A program does not pick the protocol. The driver does, from the bus.

## Touchpad: intel-lpss, i2c-hid, hid-multitouch

- I2C controller: PCI `8086:43e8` (Tiger Lake-H Serial IO I2C #0).
  Driver: `intel-lpss` + `i2c-designware`,
  `drivers/mfd/intel-lpss-pci.c`, `drivers/i2c/busses/i2c-designware-platdrv.c`.
- Touchpad: ACPI `PNP0C50`. HID `06CB:7E7E` (Synaptics) or `04F3:30AA`
  (ELAN).
- Drivers:
  - `i2c_hid_acpi`, `drivers/hid/i2c-hid/i2c-hid-acpi.c`. ACPI IDs
    `ACPI0C50` and `PNP0C50`.
  - `hid-multitouch`, `drivers/hid/hid-multitouch.c` (`HID_GROUP_MULTITOUCH`).
- Shim: PCI BAR for LPSS, its MSI, ACPI so the child is instantiated,
  and the HID stack. No firmware blob.
- Userspace: `/dev/input/event*`.
- SMBus `8086:43a3` (`i2c_i801`) is a different adapter. The touchpad
  is not on it. Do not confuse the two.

## Ethernet: r8169

- ID: `10ec:8168` (`PCI_VDEVICE(REALTEK, 0x8168)` in
  `drivers/net/ethernet/realtek/r8169_main.c`). Rev 15 is an RTL8111H.
- Attach: `pci_driver rtl8169_pci_driver`.
- Struct: `net_device_ops rtl_netdev_ops`.
- Shim: PCI BAR, `pci_alloc_irq_vectors`, `dma_map_page` /
  `dma_map_single`. `rtl_request_firmware` loads a closed
  `rtl_nic/rtl8168h-*.fw` (which file depends on the MAC version the
  chip reads out).
- Userspace: a netdev (`eth0` / `enp3s0`). Normal sockets.

This is the cheap network: no 802.11, no firmware dance beyond one
small blob. Bring it up before Wi-Fi.

## Wi-Fi: iwlwifi (AX201)

- ID: PCI `8086:43f0` subsystem `8086:0074` (Tiger Lake PCH CNVi,
  AX201 160 MHz).
- Driver: `iwlwifi`, `drivers/net/wireless/intel/iwlwifi/pcie/drv.c`.
- Attach: `pci_driver iwl_pci_driver`. The row is
  `IWL_PCI_DEVICE(0x43F0, …)` with an AX201 cfg.
- Struct: `ieee80211_ops iwl_mvm_hw_ops` (`IWLMVM` / mac80211).
  Kconfig `IWLWIFI` depends on `PCI`, `HAS_IOMEM`, `CFG80211`, and
  selects `FW_LOADER`.
- Shim: PCI BAR, MSI, DMA, `request_firmware`. For this CNVi part the
  usual prefix is `iwlwifi-QuZ-a0-hr-b0` (API around 77). The driver
  walks the API range and loads `<prefix>-<api>.ucode`. Those files
  are closed. (AX210 on the Pocket 4 is a different prefix,
  `iwlwifi-ty-a0-gf-a0`. Do not reuse those blobs.)
- Userspace: a netdev via cfg80211/nl80211. `wpa_supplicant` in user
  space. Roadmap step 13.

## Bluetooth: btusb

- ID: USB `8087:0026`.
- Driver: `btusb`, `drivers/bluetooth/btusb.c`, `BTUSB_INTEL_COMBINED`.
- Needs the USB host section first. Firmware download is
  `drivers/bluetooth/btintel.c` (`intel/ibt-*.sfi` / `.ddc`, closed).
- Userspace: an HCI socket. BlueZ opens the `hci_dev`. Not needed to
  sit at the laptop and type.

## Audio: SOF / snd-hda-intel

- ID: PCI `8086:43c8`.
- Modern bind: `snd_sof_pci_intel_tgl`,
  `sound/soc/sof/intel/pci-tgl.c`. Firmware `intel/sof/sof-tgl.ri` plus
  a topology `.tplg` named from ACPI. Closed.
- Fallback: `snd-hda-intel`, `sound/hda/controllers/intel.c`,
  `PCI_VDEVICE(INTEL, 0x43c8)`. Codec is read from the HDA link (Realtek
  on this chassis). Speakers and the jack can work this way with no SOF
  blob.
- Userspace: ALSA, `/dev/snd/pcmC*D*`.

Prefer the HDA fallback first. SOF is more moving parts for the same
speakers.

## Webcam: uvcvideo

- Class match: `USB_INTERFACE_INFO(USB_CLASS_VIDEO, 1, …)` in
  `drivers/media/usb/uvc/uvc_driver.c`. Product IDs vary; one 11UE is
  `30c9:0042`.
- Shim: USB host + isochronous transfers. No firmware in this driver.
- Userspace: `/dev/video0`.

## ACPI, battery, lid, fans, backlight

None of these are PCI devices we write from scratch.

- **Power button, lid, AC, battery:** ACPI (`PNP0C0C`, `PNP0C0D`,
  `ACPI0003`, `PNP0C0A`). Need an AML interpreter and the usual
  `/sys/class/power_supply/` nodes. Sleep on this generation is
  **`s2idle`** (Modern Standby). S3 only if Click BIOS offers it.
  Power off is `_S5`.
- **Thermal / RAPL:** `8086:9a03` (`processor_thermal_device`). Nice
  for Settings; the EC still turns the fans.
- **Fans and RGB / Fn keys:** MSI embedded controller. Linux has
  `msi-wmi` / `msi-ec` for some boards; the program inside the EC is
  closed and updates only with MSI's Windows BIOS+EC package. We can
  live with the EC's automatic curve for a long time.
- **Backlight:** once i915 is up, `intel_backlight`. Until then the
  brightness slider stays disabled
  ([power-performance-settings.md](power-performance-settings.md)).

## Closed firmware, no source to compile

- **BIOS / Click BIOS 5.** MSI's updater runs on Windows. `fwupd` may
  or may not see a capsule. Secure Boot should stay off for our
  unsigned `BOOTX64.EFI`.
- **Intel ME** (`8086:43e0`, `mei_me`). We do not talk to it.
- **NVIDIA GA106** if we want a complete modeset driver: proprietary
  blob, or wait for nouveau Ampere.
- **EC program.** Power button, charge, fans, keyboard backlight.
- **i915 DMC / GuC, iwlwifi, btintel, r8169, SOF** blobs: listed in
  their sections. Ship them on the live disk under `/lib/firmware`
  when those drivers land.

## What this asks of the kernel, in order

A Katana bring-up that follows these drivers looks like this. Items
marked **done** were already visible on this unit.

1. **64-bit UEFI, GOP/`simplefb`, long mode.** Done. Gold bar + kernel
   text at 1920×1080. No COM1: keep the serial ring from blocking the
   screen.
2. **i8042 keyboard** so you can type at a prompt. Driver exists; confirm
   IRQ 1 on hardware.
3. **xHCI `8086:43ed` + USB mass-storage** so the live stick is `/`.
   This is the next visible win. SCSI USB storage rides on this.
4. **NVMe** so the internal SSD exists (install target). AHCI if a
   SATA drive is present. BIOS: AHCI/NVMe, not RST RAID.
5. **ACPI:** `_S5` power off, battery, AC, lid. `s2idle` after that.
6. **LPSS I2C + i2c-hid** so the touchpad exists.
7. **`r8169`** so a cable is a network.
8. **`iwlwifi` + `request_firmware`** so the AX201 is a network.
9. **`snd-hda-intel`** (then SOF if the codec needs it) for speakers
   and the jack.
10. **`i915`** so we own the panel: 144 Hz, backlight, no GOP blit.
11. **`uvcvideo`**, **`btusb`**, HDMI / nouveau last.

The RTX 3060, Nahimic, RGB, and the EC program stay outside that list.

## What we still need from this exact machine

The BIOS screen cannot show PCI IDs. When convenient, from Linux:

```text
lspci -nn
lsusb
cat /sys/class/dmi/id/product_name
cat /sys/class/dmi/id/board_name
```

Expect `Katana GF66 11UE` / `MS-1581`. Note the NVMe vendor, the
touchpad HID ID, and the webcam. Those three are the only functions
that commonly differ between 11UE boxes.
