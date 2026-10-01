# MyOS Roadmap

Target first: 32-bit x86 (i686) in QEMU. Real hardware and ARM come later.
Mark a step `[x]` only when its "Done when" check works.

## Phase 1 — Boot and basic kernel
- [x] **0. Toolchain** — cross-compiler (`i686-elf-gcc`), QEMU, GDB, Makefile.
  Done when: `make` produces a kernel binary.
- [x] **1. Boot & print** — our own BIOS MBR bootloader: load kernel from disk, enable A20, GDT,
  enter protected mode; kernel writes to VGA text buffer and serial port. (See `docs/DESIGN.md`.)
  Done when: QEMU shows **"Hello world from MyOS!"**.
- [x] **2. CPU tables & interrupts** — GDT, IDT, exception handlers, PIC remap, PIT timer, PS/2 keyboard.
  Done when: a fault prints a message instead of rebooting; typed keys echo to the screen.
- [x] **3. Memory** — BIOS E820 memory map (read by our MBR), physical page allocator, paging with
  the kernel in the higher half (0xC0000000, Linux's 3G/1G split), kernel heap (`kmalloc`/`kfree`).
  Done when: allocate/free tests pass and paging is on.
- [x] **4. Threads & scheduler** — kernel threads, context switch, then the **time-slot scheduler**:
  20 × 1 ms slots, slots 1 and 10 for the system, named tasks assigned to slots from a config file.
  Done when: two named threads print in their assigned slots (e.g. "brakes" in 4,9,14,19).

## Phase 2 — Running Linux programs
- [x] **5. User mode & syscalls** — ring 3, TSS, `int 0x80` handler using **Linux i386 syscall numbers**.
  Done when: a user-mode program calls `write()` and `exit()`.
- [ ] **6. ELF loader** — load static ELF executables.
  Done when: an unmodified static musl "hello world" runs.
- [ ] **7. VFS + initramfs** — virtual file system, cpio initramfs loaded by our stage-2 loader.
  - [x] 7a: stage-2 loader (kernel at 1 MiB, initramfs after it, no size limit), 4 MiB pages.
  Done when: `open`/`read`/`close`/`stat` work on initramfs files.
- [ ] **8. Processes & TTY** — `fork`, `execve`, `wait4`, `pipe`, `dup2`, signals, TTY line discipline.
  Done when: **a BusyBox shell runs** and `ls`, `cat`, `echo | wc` work. (Big milestone!)
- [ ] **9. Disk & ext2** — ATA/IDE PIO driver, ext2 read then write.
  Done when: files survive a reboot.
  - [x] Done early (polling, in-kernel): block device layer, ATA PIO read/write, MBR partitions,
    FAT32 read-only (long names, path lookup, file reads). Test disk: `make usb`; `make test`.
  - [ ] FAT32 write, ext2, plug into the VFS (step 7), IRQ-driven I/O (after step 2).

## Phase 3 — Networking & beyond
- [ ] **10. PCI & network** — PCI enumeration, e1000 or RTL8139 driver, TCP/IP (lwIP port), BSD sockets syscalls.
  Done when: `ping` and `wget` work in QEMU.
- [ ] **11. Fuller Linux ABI** — `mmap`, `brk`, dynamic linking (`ld.so`), more syscalls as programs need them.
  Done when: ordinary dynamically linked GNU programs run.
- [ ] **12. Real hardware** — boot from USB/CD on an old Pentium 4 / Pentium M machine.
- [ ] **13. WiFi** — pick ONE chip with open docs (e.g. Atheros AR5xxx / ath5k), port driver + firmware loading,
  802.11 + WPA2 (run `wpa_supplicant` in user space).
  Done when: connects to a WPA2 network.
- [ ] **14. ARM port** — one board (original Raspberry Pi), device tree, ARM boot path.

## Notes
- Drivers: most will move to user space, built against a Linux-kernel-API shim (see `docs/DESIGN.md`).
- WiFi alone can be more code than everything else combined — plan for it last.
- If speed matters more than learning: Linux kernel + BusyBox ("Linux From Scratch") reaches the same goals far faster.
