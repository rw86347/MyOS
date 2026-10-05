# x86_64 kernel port — plan and status

Goal: one portable `kernel/`, `mm/`, `fs/`, `drivers/` tree; CPU code only in
`arch/x86_64/`. Userland is already Alpine x86_64 + ELF64 `myos-login`
(`make ARCH=x86_64 live-uefi`). The blocker is the kernel: today it is still
the long-mode **hello** in `arch/x86_64/early.c`.

## Phases (baby steps, each ends in something visible)

| Phase | Deliverable | Visible check |
|-------|-------------|---------------|
| **A** | UEFI loader loads initrd + E820 into bootinfo; Makefile builds full kernel link | `make ARCH=x86_64` links (may not run yet) |
| **B** | `entry.asm`, 4-level paging, `pmm`, identity/direct map | Serial: `Memory: … MiB usable, paging on` |
| **C** | IDT, PIC, timer, serial, `initrd_init` | `initramfs: … KiB` |
| **D** | Linux **syscall** entry + enough syscalls for musl `/init` | runit stage 1 text |
| **E** | ELF64 loader, `execve`, threads, `fork`/`clone` basics | `/sbin/init` starts |
| **F** | `simplefb` with 64-bit GOP (no shadow), evdev/keyboard | X starts |
| **G** | Golden Hour on MSI | `myos-login` on hardware |

Phases **A–C** are arch + boot. **D–E** are the bulk of `kernel/` wired to
x86_64 registers and `syscall`. **F** fixes framebuffer mmap for `0x400000000`.
**G** is the stick you already have (`build/x86_64/myos-live-uefi.img`).

## ABI choices (fixed early)

- **Syscall:** Linux x86_64 (`syscall`; `rax` = nr, `rdi rsi rdx r10 r8 r9`).
  Numbers from Linux `syscall_64.tbl`. No i386 `int 0x80` on this kernel.
- **User addresses:** `user_addr_t` = `uint64_t` when `ARCH_X86_64` is defined
  (`include/user.h`). VMAs, brk, stack, ELF64 load addresses use it.
- **ELF:** `EM_X86_64`, `ELFCLASS64`; dynamic musl via `PT_INTERP`.
- **Memory map:** keep `phys_addr_t` (64-bit) everywhere; direct map covers
  all usable RAM below a cap (start with 2 GiB mapped, raise as paging grows).

## Files in `arch/x86_64/` (twins of `arch/i386/`)

| i386 | x86_64 | Notes |
|------|--------|--------|
| `entry.asm` | done (GDT, bss, stack) | add PML4 bootstrap in phase B |
| `linker.ld` | done (1 MiB LMA) | higher half later |
| `boot/efi/efi_main.c` | in progress | initrd + E820, stay in long mode |
| `paging.c` | **todo** | 4-level, `vmm_*` with 64-bit VA |
| `gdt.c`, `pic.c`, `cpu.c`, `pat.c`, `fpu.c` | port from i386 | mostly C |
| `isr.asm`, `switch.asm`, `syscall.asm` | **todo** | 64-bit stack frame |
| `uaccess.asm`, `usermode.asm` | **todo** | canonical user checks |
| `interrupts.c`, `sysenter.c` | → `syscall.c` | no sysenter on x64 |
| `include/*` | expand | `bootinfo`, `memlayout`, `paging` |

## What not to do

- Do not `#ifdef __x86_64__` through `kernel/syscall.c`; use arch tables or
  small wrappers in `arch/x86_64/`.
- Do not fork `live/` or duplicate themes; only `build/live/extra.x86_64`.
- Pentium `make test` (`ARCH=i386`) must stay green after each step.

## Current status

- **Done:** hello on QEMU + MSI; x86_64 live userland + GPT image; i386 live
  login path (RAM shadow blit) for comparison.
- **Started:** phase A (this document, bootinfo, loader, Makefile).
- **In progress:** phase B (`arch/x86_64/paging.c`, commented for review). Shared
  code uses `#ifdef ARCH_X86_64` / `-DARCH_I386` so i386 stays unchanged.
- **Next work:** phase C (IDT/isr.asm, syscall) so the full kernel links; then
  serial proof on OVMF.

## Review (2026-10-05, Claude)

Boot under QEMU + OVMF (`make ARCH=x86_64 test-uefi`) reaches `net: lo up`: memory,
paging, initramfs, PCI, bochs DRM + fbcon, AHCI, pcache all work. Fixed:

1. **New-thread frame one word short** (`kernel/thread.c`, `thread_create_in`):
   `switch.asm` pops 8 registers + `ret` (9 words), the frame had 8, so the first switch
   to any new thread (`sched_start`'s idle thread) returned into garbage
   (`RIP=0x2014`). `thread_fork` was already right.
2. **`struct thread.esp` was `uint32_t`**: `switch_context` stores all 8 bytes of RSP,
   overwriting the low half of `t->stack` (TSS RSP0, kfree). Now `uintptr_t` (i386
   unchanged).

Still blocking `/init` (QEMU log: `-d int,cpu_reset`; it triple-faults, so the serial
log just stops):

3. **User space and the kernel overlap.** `KERNEL_VBASE == 0` and the direct map is the
   identity map of 0..2 GiB, in EVERY address space; `MMAP_BASE` is 0x400000 and
   `vmm_pd_create` copies the whole PML4, so a process shares the kernel's PDPT/PD/PT.
   Mapping /init's pages unmaps kernel memory (first fault: the initramfs at 0x24f000,
   then `exception()` faults on its own .rodata -> recursion -> #DF -> triple fault).
   Fix properly: kernel in the higher half — direct map at PML4[256]
   (0xFFFF800000000000), image at -2 GiB (`-mcmodel=kernel`), user space = PML4[0..255];
   `vmm_pd_create` copies only PML4[256..511] from the kernel's own PML4 (never the
   current process's); `vmm_pd_destroy`/`fork` walk only the lower half. Interim option:
   kernel owns PML4[0] (the first 512 GiB), user mappings must be above it.
4. **mm.c is still 32-bit**: `struct vma` start/end are `uint32_t`, `vmm_map` takes
   `uint32_t` VAs, and every bound is `KERNEL_VBASE` (0 here, so every mapping is
   rejected and every user fault is "outside"). Needs `user_addr_t` throughout and a
   `USER_ADDR_MAX` bound. `include/mm.h` still defines the i386 layout and collides with
   `include/user.h` (the "redefined" warnings).
5. **64-bit pointer truncation** (compiler warnings to fix, not silence):
   `arch/x86_64/include/uaccess.h` (casts to 32-bit), `kernel/futex.c`, `arch/x86_64/pat.c`,
   always-true range checks in `arch/x86_64/paging.c` and `uaccess.h`.
6. Fixed: `syscall_entry.asm` had `sysretq`, which NASM assembles as a LABEL (no such
   mnemonic; it's `o64 sysret`) — the return instruction was missing, so a syscall would
   have fallen off the end. Still open: `arch/x86_64/cpu.c:52` format types.
