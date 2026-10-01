# Dev Log

Short dated notes after each work session: what was done, what broke, what was learned.

- 2026-10-01: Project created. Plan in ROADMAP.md. Next: Step 0 (toolchain).
- 2026-10-01: Steps 0 + 1 done. Toolchain via Homebrew (brew downloads from ghcr.io kept hanging —
  Ctrl-C and re-run fixes it; skipped GRUB since we boot with our own MBR). Wrote MBR bootloader
  (int 0x13 ext read -> 0x10000, fast A20, flat GDT, protected mode), entry.asm (clear .bss),
  VGA + serial drivers. QEMU shows "Hello world from MyOS!"; `info registers` confirms CR0.PE=1,
  CS32, A20=1. Design decisions (slot scheduler, user-space drivers, storage plan) in DESIGN.md.
  Next: Step 2 (IDT, exceptions, PIC, PIT, keyboard).
