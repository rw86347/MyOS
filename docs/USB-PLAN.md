# USB: the live stick as a disk

Goal: the MSI boots the 64-bit live stick and mounts its ext2 `myos-root`
partition as `/`. Then the whole desktop (Chromium, Firefox, ...) runs from
the stick, and files you save stay on it. Today the kernel can't see the
stick once it is running ("storage: no disks"), so only the small login
initramfs reaches RAM.

Decided (2026-10-07): **our own xHCI and mass-storage drivers.** Linux's
`xhci-hcd` and `usb-storage` are GPL-2.0 only, so the 2026-10-06 licence rule
keeps them out. Storage drivers are native anyway (2026-10-02). Sources are
the public specs: Intel's *eXtensible Host Controller Interface for USB*
(xHCI 1.2) and the USB-IF *Mass Storage Class Bulk-Only Transport* 1.0, plus
SCSI's SBC/SPC command sets. Linux's code is read only to learn which
controller quirks matter on real hardware.

Shape (smallest that boots from a stick):

```
  fs (ext2, FAT)  ->  blockdev "sda"
  drivers/usb/usb_storage.c  bulk-only transport: CBW -> data -> CSW, SCSI
                          READ(10)/WRITE(10); zero-copy scatter-gather
  drivers/usb/xhci.c      the controller: rings, commands, ports,
                          enumeration (address, descriptors, configuration)
```

Speed choices:
- A disk command queues the command block and the data at once, rings
  both doorbells, and takes an interrupt at the end of the data, then one
  for the status: **two interrupts**, where Linux's usb-storage takes three.
  (Queueing the status as well would save one more, but Linux and Windows
  never do that, and QEMU's emulated stick then never answers.)
- The data goes straight between the page cache's pages and the stick:
  the xHCI's scatter-gather (one TRB per piece) needs no bounce buffer.
- Enumeration runs in a kernel thread (`usb`) while the IDE/AHCI/NVMe
  drivers start, so boot waits only for the slower of the two.

Steps (each ends with something visible in QEMU):
- [x] **U1** The controller: hand-off from the firmware, reset, rings,
  MSI/MSI-X interrupts; every connected port is listed with its speed.
  QEMU: `-device qemu-xhci -device usb-storage`.
- [x] **U2** Enumeration: slot, address, descriptors, configuration.
  Prints vendor:product, the name and the class of each device.
- [x] **U3** Mass storage: `sda` as a block device; its partitions; a
  `myos-root` on the stick becomes `/`. `make test-usb` boots the live
  image from a QEMU USB stick with no other disk (18 s to the login
  screen); `tools/test-disks.sh` runs the file-system self-tests on a USB
  stick; `tools/diskbench.sh usb` compares with Linux.
  If the loader says it came from USB and no `myos-root` is there yet,
  the kernel waits up to 10 s for it (a stick that shows up late is found
  by the port-change interrupt).
- [x] **U4** The MSI: the 64-bit live stick runs the full desktop from the
  stick (2026-10-07). Both Intel controllers start (8086:9a17, the CPU's
  Thunderbolt one, and 8086:43ed, 24 ports), on MSI; the stick (24a9:205a,
  "USB DISK 3.2", 60 000 MiB) became sda and sda2 became "/"; the webcam
  and the Bluetooth half of the AX201 were listed too. The stick ran at
  480 Mb/s on a USB 2 port: open whether it was in the USB 2.0 socket or
  the USB 3 link did not come up after our reset.

- [x] **U5** USB keyboards and mice (2026-10-17, for the GPD P2 Max, whose
  built-in keyboard and touchpad are one USB device): interrupt-IN
  endpoints in `xhci.c` (`usb_intr_start`: the completion, in the
  interrupt, hands the report over and queues the next transfer; a failed
  endpoint is cleared and restarted by the usb thread), `usb_hid.c` for
  the class, and a report-descriptor parser (`drivers/hid/hid.c`), not the
  boot protocol, so wheels, extra buttons and composite devices work.
  Reports become the same key and movement events as PS/2's
  (`keyboard_key`, `mouse_report`), so X needed no change. Tested in QEMU
  with `usb-kbd` and `usb-mouse` at 12 and 480 Mb/s, on i386 and x86_64,
  and with no PS/2 controller at all (`-machine q35,i8042=off`).

Later (not needed for the stick): hubs, unplugging, UAS (faster USB 3
disks), EHCI/UHCI for the old PCs, and Linux's USB driver API
(`usb_driver`, URBs) for drivers like `btusb`.
