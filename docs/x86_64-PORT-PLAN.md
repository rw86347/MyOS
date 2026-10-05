# x86_64 kernel port — plan and status

Goal: one portable `kernel/`, `mm/`, `fs/`, `drivers/` tree; CPU code only in
`arch/x86_64/`. Userland is already Alpine x86_64 + ELF64 `myos-login`
(`make ARCH=x86_64 live-uefi`). Since 2026-10-05 the portable kernel runs on
x86_64 under OVMF and runs 64-bit BusyBox: `/init` and an interactive shell
(`make ARCH=x86_64 test-uefi-shell`).

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
| `entry.asm` | done | boot page tables, jump to the higher half |
| `linker.ld` | done | VMA -2 GiB, LMA 1 MiB, `-mcmodel=kernel` |
| `boot/efi/efi_main.c` | done | initrd + E820 + GOP, stays in long mode |
| `paging.c` | done | 4-level; kernel = PML4 256 + 511, shared; fork COW |
| `gdt.c`, `pic.c`, `cpu.c`, `pat.c`, `fpu.c` | done | GDT order fixed for sysret |
| `isr.asm`, `switch.asm`, `syscall_entry.asm` | done | one frame layout for both |
| `uaccess.asm`, `usermode.asm` | done | 64-bit exception table |
| `interrupts.c`, `syscall.c` | done | `syscall` instruction, no sysenter |
| `include/*` | done | `bootinfo`, `memlayout`, `paging`, `uaccess` |

## What not to do

- Do not `#ifdef __x86_64__` through `kernel/syscall.c`. The one exception is
  a single block at its end: the x86_64 syscall table and the few adapters for
  calls whose x86_64 ABI differs (stat, mmap's byte offset, clone's argument
  order, arch_prctl). It lives there because the handlers are `static` in that
  file; a table in `arch/` would have to export about a hundred of them.
- Do not fork `live/` or duplicate themes; only `build/live/extra.x86_64`.
- Pentium `make test` (`ARCH=i386`) must stay green after each step.

## Current status

- **Done (phases A–E, 2026-10-05):** the portable kernel boots under OVMF, runs in
  the higher half, and runs 64-bit BusyBox: `/init`, `fork`/`execve`, a login shell,
  pipes, files. `make ARCH=x86_64 test-uefi-shell` types commands and checks them.
- **Done (2026-10-05, later):** signals (kernel/signal.c on both CPUs; the frame in
  arch/<cpu>/signal.c) and the missing system calls (below).
- **Next:** phase F (the live desktop on this kernel).

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

## Higher half and 64-bit user space (2026-10-05, Claude)

Memory map (arch/x86_64/include/memlayout.h, like Linux): user space is the bottom
half, `0 .. 0x7FFFFFFFF000` (include/user.h), so fixed-address programs (Alpine's
busybox-static is ET_EXEC at 0x400000) load where they were linked. The kernel is the
top half, in two PML4 entries made at boot and shared by pointer with every process:
256 = direct map of RAM at `0xFFFF800000000000` (2 MiB pages), 511 = the image at
`0xFFFFFFFF80000000` plus heap, MMIO and write-combined windows (the i386 layout one
level up). entry.asm builds temporary tables (identity + direct map + image over the
first 1 GiB), loads CR3 and jumps up; `vmm_init` replaces them without the identity map.

Fixed on the way (each one a crash or a silent corruption):
- `user_enter` overwrote the entry address (`mov ax, ...` clobbers RAX).
- syscall entry built a frame in a different order from interrupts, clobbered R10
  (argument 4), and returned through a garbage `pop rsp`. Now one frame layout; sysret
  only when RCX/R11 match the frame and RIP is a user address, else iretq.
- GDT order did not fit sysret (it would have loaded the TSS as CS); EFER.SCE was never
  set; SFMASK now also clears TF/DF/AC/NT.
- uaccess.asm: `__copy_user` copied in the wrong direction, returned garbage for
  lengths that are multiples of 8, and its exception-table entries pointed at the
  instruction before the faulting one (as did `__strncpy_user`'s); `__fault_in_user`
  read `write` from the i386 stack slot. The table is 64-bit (`dq`) now.
- uaccess.h compared against `KERNEL_VBASE`, which was 0: every user copy failed.
- `PAGE_SIZE` was `4096u`, so `~(PAGE_SIZE - 1)` masked addresses to 32 bits.
- `arch_prctl(ARCH_SET_FS)` was ignored (musl's TLS): now `thread->tls[0]` is the FS
  base, written to the MSR at each switch; `CLONE_SETTLS` takes the FS base too.
- The syscall layer was 32-bit. Handlers take and return `uintptr_t`/`intptr_t` (the
  same types as before on i386); helpers that carry user pointers were widened
  (found with a one-off `-Wconversion` build: paths in sys_fs.c, futex addresses,
  `clear_tid`, socket addresses, ioctl arguments). `struct user_iovec`,
  `user_msghdr`, `k_sigaction` and `sysinfo` follow the program's word size.
- arch/x86_64/syscall_dispatch.c translated numbers to i386 ones and routed some calls
  to the wrong handler (fstat -> fcntl, newfstatat -> faccessat, mmap -> mmap2 with a
  byte offset). Replaced by a table indexed by x86_64 number (kernel/syscall.c).

Since then (2026-10-05):
- **Signals** work on x86_64: Linux's x86_64 rt_sigframe (red zone, fxsave area,
  SA_RESTORER), rt_sigreturn that refuses a non-user RIP/RSP, faults become signals,
  delivery on syscall and interrupt return. kernel/signal.c is shared again; only the
  frame is per CPU (arch/i386/signal.c, arch/x86_64/signal.c).
- **The x86_64 gaps are filled:** statfs/fstatfs, select, getitimer/setitimer,
  timer_create (sigevent is pointer-sized), times, getrusage, fcntl locks, the direct
  shm/sem/msg calls, fallocate, fadvise64, sendfile, gettimeofday, time, sysinfo.
- `user/sys/systest.c` checks them from a program's side on both CPUs (no C library,
  Linux's struct layouts written out), in `make test-uefi-shell`.

Still to do:
1. kprintf: x86_64 builds still have `-Wformat` warnings where `%lu` prints a
   `uint32_t` (an `unsigned long` on i686-elf, an `unsigned int` here).
2. `make ARCH=x86_64 test` (the kernel self-tests) does not build yet.
3. Lengths in mm.c are 32-bit: `mmap` of 4 GiB or more is refused (-ENOMEM), and
   `munmap`/`mprotect`/`mremap` lengths are cut to 32 bits.
4. Real hardware (the GPD Pocket 4): only QEMU + OVMF so far.
