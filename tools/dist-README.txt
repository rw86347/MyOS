MyOS — a hobby operating system, on a USB stick
================================================

MyOS is a small operating system kernel written from scratch. It runs real,
unmodified Linux programs: this image boots into the bash shell, with the
BusyBox set of Unix commands (ls, cat, grep, vi, ...).

It is an experiment and has NOT been tested on many real PCs yet — that's
what you're helping with. Thank you!


WHAT YOU NEED
-------------
- A PC that can boot the old way ("legacy BIOS"). On newer PCs (UEFI) this
  is called "CSM" or "Legacy boot" in the firmware settings — it must be on.
  Machines that are UEFI-only can't start MyOS yet; use an emulator (below).
- A 32-bit or 64-bit x86 processor, Pentium Pro / Pentium II or newer.
- At least 64 MB of RAM.
- A keyboard (a USB keyboard works if the BIOS has "USB legacy support").
- A USB stick you don't mind erasing (anything from 8 MB up).


WRITING THE IMAGE TO A USB STICK  (this ERASES the stick!)
----------------------------------------------------------
Windows:  use Rufus (rufus.ie) — choose myos-usb.img, and when asked,
          write in "DD image" mode. Or balenaEtcher (etcher.balena.io).

macOS:    diskutil list                      (find the stick, e.g. /dev/disk4)
          diskutil unmountDisk /dev/disk4
          sudo dd if=myos-usb.img of=/dev/rdisk4 bs=1m
          diskutil eject /dev/disk4

Linux:    lsblk                              (find the stick, e.g. /dev/sdb)
          sudo dd if=myos-usb.img of=/dev/sdb bs=1M conv=fsync

Double-check the disk name: dd overwrites whatever you point it at.


BOOTING
-------
Plug the stick in, start the PC, and open the boot menu (usually F12, F11,
F8 or Esc right after power-on). Choose the USB stick. You should see
"MyOS: stage 1", then a welcome text and a bash prompt:  myos:/#

Things to try:   ls /bin      uname -a      echo hello | wc -c
                 cat /etc/motd    ps (doesn't work yet)    help

To stop: just switch the PC off — nothing is written to any disk.


TRYING IT IN AN EMULATOR INSTEAD
--------------------------------
QEMU (qemu.org), as a hard disk:
    qemu-system-i386 -m 256M -drive format=raw,file=myos-usb.img
QEMU, as a USB stick:
    qemu-system-i386 -m 256M -device usb-ehci,id=ehci \
        -drive if=none,id=stick,format=raw,file=myos-usb.img \
        -device usb-storage,bus=ehci.0,drive=stick,bootindex=0
VirtualBox: convert it first, then attach myos.vdi as the disk of a new
"Other/Unknown" 32-bit machine:
    VBoxManage convertfromraw myos-usb.img myos.vdi --format VDI


WHAT DOESN'T WORK YET
---------------------
- Files can be created, edited, moved and deleted, but only in memory:
  everything you change is gone when you switch off (nothing is written
  back to the USB stick yet).
- No network, no sound, no mouse, no graphics — text screen only.
- ps, top, free, df, mount: coming next. Ctrl-Z / fg / bg: not yet.


PLEASE REPORT BACK
------------------
- The PC model (and CPU, if you know it), and whether it booted.
- If it stopped or crashed: a photo of the screen helps a lot. MyOS prints
  what went wrong before it halts.


WHAT'S INSIDE
-------------
The MyOS kernel and boot loader, plus these unmodified programs from Alpine
Linux 3.20 (x86): BusyBox 1.36.1, bash 5.2.26, readline 8.2 (GPL), musl
1.2.5, ncurses 6.4 (MIT-style licences). Their source code is available
from Alpine: https://gitlab.alpinelinux.org/alpine/aports (and the
projects' own sites).
