# MyOS — project guide for Claude Code

## What this is
A hobby, from-scratch operating system kernel written by Rodger as a learning project:
**a small, fast race horse of a kernel** — no bloat, built around **speed and predictability**.
Goals, in priority order:
1. Run on **three CPU families** with one portable kernel (CPU-specific code only in `arch/`):
   - **Old PCs** (~20 years old): 32-bit x86, i686 — Pentium 4 / Pentium M, BIOS boot.
     **This is what we tune for: it must be fast on very old hardware.** First target.
   - **New x86-64 PCs**: GPD Pocket 4 (see `docs/hardwareRequirements.md`), MSI Katana GF66,
     and similar — UEFI, `arch/x86_64`. Don't let i386-only shortcuts leak outside `arch/i386/`.
   - **ARM64**: QEMU `virt` and **Raspberry Pi** bare metal (roadmap step 14).
   - **Apple Silicon Macs bare metal** (M1 MacBook Air and later — MyOS only, no macOS under it;
     roadmap step 15). Same `arch/aarch64/` as Pi; Apple boot and devices in `arch/aarch64/apple/`.
     Emulated x86 MyOS on an M1 Mac via UTM is dev-only, not this goal.
2. Run unmodified GNU/Linux user-space programs by implementing the **Linux system call ABI**
   (i386: `int 0x80`, Linux syscall numbers, Linux struct layouts, ELF binaries).
3. Networking, and eventually WiFi (last — by far the hardest part).

The full step-by-step plan is in `docs/ROADMAP.md`. Always check which step is current before starting work.

## How Rodger wants to work
- **Baby steps.** One roadmap step at a time; each step ends with something visible working in QEMU.
- **Teach while building.** Explain new concepts (GDT, paging, context switch, etc.) in plain language
  and comment code so it can be read and learned from. Don't dump large amounts of code at once.
- **Priorities for all code: speed and predictability first, then small, then readable.**
  Readable still matters (this is a learning project): when code is fast in a non-obvious
  way, a comment says why. Never trade correctness or security for speed.
- After finishing a step: update the checklist in `docs/ROADMAP.md` and add a short note to `docs/LOG.md`.
  Known shortcuts go in `docs/gaps.md`; performance work and results in `docs/performance.md`.

## Performance rules (binding for all new code)
Tune for the **old hardware**: small caches (keep hot code and data small), slow `int 0x80` on
the Pentium 4 (use `sysenter`), slow port I/O and segment-register loads (avoid them on hot
paths), few TLB entries (big pages). Anything that's faster on old CPUs is fine on new ones.
The goal is to beat Linux where a small kernel can: syscall and interrupt cost, context-switch
cost, **worst-case latency and slot jitter**, boot time, memory footprint. Linux will stay
ahead on throughput of big, complex workloads; don't add complexity chasing that.

1. **Know the cost.** Hot paths — syscalls, interrupts, the scheduler, context switch, page
   faults — are O(1) or proportional to the request (bytes copied). No searches, no unbounded
   loops, no busy-waiting in them. Prefer tables indexed by number over lists searched by name.
2. **Predictability beats average speed.** Every syscall finishes quickly; long work is split
   up or moved to a thread. Interrupt handlers do the minimum and defer the rest. No memory
   allocation in interrupt handlers. Worst case matters more than the average.
3. **Measure.** Every speed change comes with a before/after number from `make bench`, written
   to `docs/performance.md`. Never claim "faster" without one. QEMU numbers only compare
   versions of our code; real numbers come from target hardware.
4. **No bloat.** Implement only what real programs need (musl first — check its source), no
   legacy syscalls (see `docs/DESIGN.md`), no feature without a user, no abstraction layer with
   a single implementation (the `arch/` split is the exception). Test and benchmark code is
   compiled only into `make test` / `make bench` builds, never into a normal boot.
5. **Copy less.** Zero-copy wherever possible (e.g. initramfs file data is read straight from
   the archive). User memory is touched only through `copy_from_user` / `copy_to_user` /
   `get_user` (exception-table fixups, like Linux) after a range check — never by walking
   page tables first.
6. **Memory.** The kernel's direct map uses 4 MiB global pages. Small objects come from
   `kmalloc` size classes (O(1)). Know where every allocation's memory comes from.
7. **CPU state.** The kernel never uses FPU/SSE (`-mgeneral-regs-only`); user FPU state is
   saved eagerly (no lazy FPU — insecure). Syscalls enter with `sysenter` via the
   `AT_SYSINFO` entry page; `int 0x80` keeps working.
8. **I/O is asynchronous.** Console/serial output goes through a ring buffer drained by an
   interrupt (panics excepted). Devices use interrupts, not polling, once they have users.
9. **Design later steps the fast way from day one:**
   - Step 8: `fork` = copy-on-write; support `vfork` / `clone(CLONE_VM|CLONE_VFORK)` (musl's
     `posix_spawn`); demand paging; one page directory per process sharing the global
     kernel pages; signals delivered on syscall/interrupt return.
   - Step 9: disk I/O with IRQs + bus-master DMA (PIIX/ICH), read-ahead, a page cache.
   - Step 11: one page cache backs both `read()` and `mmap()`; a real vDSO with
     `clock_gettime` (no syscall to read the time).
   - Timers: when we move to the local APIC, use one-shot mode and skip ticks nobody needs.

## Toolchain & environment
- Host: macOS on Apple Silicon (arm64). Installed via Homebrew:
  - `i686-elf-gcc`, `i686-elf-binutils`, `x86_64-elf-gcc`, `x86_64-elf-binutils`
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
arch/x86_64/   64-bit twins of those files (roadmap 12b; arch/x86_64/PORT)
arch/arm/      (later)
kernel/        scheduler, syscalls, ELF loader, process management (portable)
mm/            physical + virtual memory, kmalloc
fs/            VFS, initramfs (cpio), ext2
drivers/       serial, VGA, keyboard, PIT, ATA, PCI, network cards
net/           TCP/IP (likely lwIP port)
include/       headers
user/          test programs: asm, and C with user/lib (a tiny libc); real musl programs later
docs/          ROADMAP.md, LOG.md, notes
live/          the live and installer disks: a repo of its own (myos-live), checked out here;
               its rules are in live/live.mk, which the Makefile includes when it is present
```
Two repos (2026-10-17): this one, github.com/rw86347/MyOS (the kernel, its tools and tests,
the docs), and github.com/rw86347/myos-live (the desktop programs, the overlay, the package
lists, the image builders, assets/themes), cloned into `live/`. WritePerfect is a third
(github.com/rw86347/writePerfect, all rights reserved: never copy its files into these two);
the live build reads it from `../wp51/wp51c` (`WP_DIR`).
Commit each change in the repo its files belong to (`git -C live ...` for `live/`). On Rodger's
Mac this clone's `.git` also keeps the old combined history (branches `m1`, `master`,
`dualChip`): local only, never push them; `main` is the published kernel history.
Keep all CPU-specific code inside `arch/` from day one so the ARM port stays possible.

## Conventions
- Freestanding C (C11) + small amounts of assembly. Flags: `-ffreestanding -nostdlib -O2 -Wall -Wextra
  -mgeneral-regs-only`.
- `make` builds the kernel, `make run` boots it in QEMU, `make debug` boots paused for gdb, `make clean`.
- `make test` builds with self-tests and checks the serial log; `make bench` builds with benchmarks
  and prints cycle counts. QEMU runs with `-cpu pentium3` (the feature set both target CPUs
  have), so we never rely on something the real machines lack.
- Syscall numbers must match Linux i386 exactly (see `arch/x86/entry/syscalls/syscall_32.tbl` in the Linux source).

## References
- OSDev wiki: https://wiki.osdev.org (Bare Bones, Meaty Skeleton, Paging, etc.)
- xv6 (MIT teaching Unix) — small, readable reference kernel
- "Operating Systems: Three Easy Pieces" (free online book)
- Linux man pages, section 2, for exact syscall behavior
- `docs/performance.md` — the performance plan and benchmark results; `docs/gaps.md` — known shortcuts
