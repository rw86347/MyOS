# MyOS — project guide for Claude Code

## What this is
A hobby, from-scratch, lightweight operating system kernel written by Rodger as a learning project.
Goals, in priority order:
1. Boot on ~20-year-old hardware: 32-bit x86 (i686 — Pentium 4 / Pentium M) first, ARM later.
2. Run unmodified GNU/Linux user-space programs by implementing the **Linux system call ABI**
   (i386: `int 0x80`, Linux syscall numbers, Linux struct layouts, ELF binaries).
3. Networking, and eventually WiFi (last — by far the hardest part).

The full step-by-step plan is in `docs/ROADMAP.md`. Always check which step is current before starting work.

## How Rodger wants to work
- **Baby steps.** One roadmap step at a time; each step ends with something visible working in QEMU.
- **Teach while building.** Explain new concepts (GDT, paging, context switch, etc.) in plain language
  and comment code so it can be read and learned from. Don't dump large amounts of code at once.
- Keep the code small and readable over clever or fast.
- After finishing a step: update the checklist in `docs/ROADMAP.md` and add a short note to `docs/LOG.md`.

## Toolchain & environment
- Host: macOS on Apple Silicon (arm64). Installed via Homebrew:
  - `i686-elf-gcc`, `i686-elf-binutils` (cross-compiler — never use the host clang/gcc for kernel code)
  - `qemu` (`qemu-system-i386`; later `qemu-system-arm`)
  - `i686-elf-grub` + `xorriso` for bootable ISOs (or use Limine), `gdb` / `lldb` for debugging
  - `nasm` for assembly (or GAS syntax via the cross toolchain — pick one and stay consistent)
- User-space test programs: build **static** binaries against **musl** (e.g. in a Linux Docker
  container or with a musl cross toolchain) so they are real Linux i386 ELF executables.
- Debug: `qemu-system-i386 -s -S ...` then attach gdb to `localhost:1234`.
- Always log kernel output to the serial port too (`-serial stdio`) so output is visible in the terminal.

## Planned layout
```
arch/i386/     boot code, GDT/IDT, interrupts, paging, context switch (CPU-specific only)
arch/arm/      (later)
kernel/        scheduler, syscalls, ELF loader, process management (portable)
mm/            physical + virtual memory, kmalloc
fs/            VFS, initramfs (cpio), ext2
drivers/       serial, VGA, keyboard, PIT, ATA, PCI, network cards
net/           TCP/IP (likely lwIP port)
include/       headers
user/          test programs (built with musl)
docs/          ROADMAP.md, LOG.md, notes
```
Keep all CPU-specific code inside `arch/` from day one so the ARM port stays possible.

## Conventions
- Freestanding C (C11) + small amounts of assembly. Flags: `-ffreestanding -nostdlib -O2 -Wall -Wextra`.
- `make` builds the kernel, `make run` boots it in QEMU, `make debug` boots paused for gdb, `make clean`.
- Syscall numbers must match Linux i386 exactly (see `arch/x86/entry/syscalls/syscall_32.tbl` in the Linux source).

## References
- OSDev wiki: https://wiki.osdev.org (Bare Bones, Meaty Skeleton, Paging, etc.)
- xv6 (MIT teaching Unix) — small, readable reference kernel
- "Operating Systems: Three Easy Pieces" (free online book)
- Linux man pages, section 2, for exact syscall behavior
