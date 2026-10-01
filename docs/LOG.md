# Dev Log

Short dated notes after each work session: what was done, what broke, what was learned.

- 2026-10-01: Project created. Plan in ROADMAP.md. Next: Step 0 (toolchain).
- 2026-10-01: Steps 0 + 1 done. Toolchain via Homebrew (brew downloads from ghcr.io kept hanging —
  Ctrl-C and re-run fixes it; skipped GRUB since we boot with our own MBR). Wrote MBR bootloader
  (int 0x13 ext read -> 0x10000, fast A20, flat GDT, protected mode), entry.asm (clear .bss),
  VGA + serial drivers. QEMU shows "Hello world from MyOS!"; `info registers` confirms CR0.PE=1,
  CS32, A20=1. Design decisions (slot scheduler, user-space drivers, storage plan) in DESIGN.md.
  Next: Step 2 (IDT, exceptions, PIC, PIT, keyboard).
- 2026-10-01: Jumped ahead to storage. tools/mkusb.sh builds a 64 MiB FAT32 "USB stick" image
  (MBR + partition at 1 MiB) with mtools. Kernel: kprintf, string funcs, block device layer,
  ATA PIO driver (polling, LBA28), MBR partition scan, read-only FAT32 (LFN, lookup, read).
  QEMU attaches usb.img as IDE hdb (no USB stack yet). Boot prints the tree; `make test` runs
  10 self-tests headless — all pass. Gotchas: uint32_t is `unsigned long` on i686-elf (use %lu);
  macOS has no `timeout`; mtools' `mcat` dumps the raw disk (use `mtype`).
- 2026-10-01: Step 2 done. Kernel GDT (incl. ring-3 segments for later), IDT with 48 asm stubs
  (isr.asm) -> interrupt_dispatch(); exceptions print a register dump and halt; PIC remapped to
  vectors 32-47 (spurious IRQ 7/15 handled); PIT at 1000 Hz (1 ms tick for the slot scheduler);
  PS/2 keyboard (scancode set 1, US, Shift/Caps/Ctrl) into a ring buffer; uptime clock top-right.
  F12 = divide error, F11 = #GP. `make test` now types keys via QEMU `sendkey` and checks timer,
  disk tests, echo and the exception handler. Lesson: GCC turned `1/zero` into a compare (div by
  zero is UB) — used inline asm `div` to force the fault. Added -MMD header dependency tracking.
  Next: Step 3 (memory: memory map, page allocator, paging, kmalloc).
