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
- 2026-10-01: Step 3 done. MBR now saves the BIOS E820 memory map at 0x1000 (MBR is 292/446
  bytes). Kernel relinked to run at 0xC0010000 (loaded at 0x10000; linker AT()); entry.asm builds
  a 4 MiB boot page table mapped at 0 and 0xC0000000, enables paging, jumps high. pmm.c: bitmap
  frame allocator (RAM up to 768 MiB), reserves low 1 MiB + kernel. paging.c: direct-maps all RAM
  at 0xC0000000, removes identity map (NULL now faults), vmm_map/unmap/translate. kheap.c:
  first-fit kmalloc/kfree at 0xF0000000 with merging. 31 self-tests pass; F10 = NULL deref ->
  page fault with CR2 decoding. klog() = serial-only output (E820 map).
  Next: Step 4 (kernel threads, context switch, time-slot scheduler).
- 2026-10-01: Step 4a: context switch. switch.asm saves EBP/EBX/ESI/EDI on the old thread's
  stack, swaps ESP, pops the new thread's registers and `ret`s into it. New threads get a fake
  frame whose return address is thread_trampoline (calls fn(arg), then thread_exit). kernel/
  thread.c: thread table, cooperative round-robin thread_yield(). Demo: ping/pong alternate;
  35 self-tests pass. Dead threads' stacks aren't freed yet (need a reaper).
  Next: timer-driven preemption, then the slot scheduler.
- 2026-10-02: Step 4b: preemption. Timer IRQ sets need_resched; interrupt_dispatch calls
  thread_preempt_check() after the EOI and switches threads on the interrupted thread's own
  stack (isr.asm already saved everything; it `iret`s when switched back). thread_yield()
  runs with interrupts off (irq_save/irq_restore); new threads `sti` in the trampoline.
  Test: two spinners that never yield both count ~7M in 50 ms. 37 self-tests pass.
  Added docs/gaps.md (known shortcuts and TODOs). Next: the time-slot scheduler.
- 2026-10-02: Step 4 done (4c: slot scheduler). config/slots.conf (brakes 4 9 14 19,
  radio 6 16) -> tools/slots2c.awk -> build/gen/slots_conf.c (checks: slots 1/10 reserved,
  one owner per slot). kernel/sched.c: every tick advances the slot (1..20) and runs its
  owner; slots 1 and 10 run main; free slots or finished owners run an idle thread (hlt).
  thread.c is now mechanics only (switch/find/next_ready). Test: tasks note the slot they
  wake in for 200 ms -> "brakes ran in slots: 4 9 14 19". 40 self-tests pass.
  Bug found: main noted slot 20 once because the frame started "before slot 1"; now starts
  at slot 1. Serial lines end in \r\n — grep anchors need [[:space:]]*$.
  Next: Step 5 (user mode, TSS, int 0x80).
- 2026-10-02: Step 5 done. TSS in the GDT (entry 5, 0x28; ltr) — ESP0 updated on every thread
  switch to that thread's kernel stack; no I/O bitmap. int 0x80 gate with DPL 3 (0xEE) ->
  syscall_dispatch(eax, ebx..ebp), result in saved EAX. Linux numbers: exit 1, write 4,
  exit_group 252; unknown -> -ENOSYS + serial log. vmm_user_ok() checks user pointers (U bit at
  both levels, below 0xC0000000). user_enter (usermode.asm) builds a fake ring-3 iret frame.
  User faults (CS & 3) kill only the program (exit code 139). Programs: user/hello.asm
  (write + EFAULT check + exit(42)), user/peek.asm (reads kernel memory -> killed); flat
  binaries at 0x08048000, incbin'd into the kernel. 43 self-tests pass.
  Next: Step 6 (ELF loader, static musl hello world).
- 2026-10-02: Step 6a: ELF loader. kernel/elf.c checks the header (ELF32, LSB, ET_EXEC,
  EM_386) and every PT_LOAD (offsets inside the file, addresses in user space, not page 0),
  maps pages writable only if PF_W, copies filesz and zeroes the rest (.bss). usermode.c
  builds the Linux start-up stack: argc, argv, envp (empty), auxv (PHDR, PHENT, PHNUM,
  PAGESZ, ENTRY, UID/GID, SECURE, RANDOM). vmm_unmap_user_all() frees all user pages and
  page tables. User programs are now ELF (nasm + i686-elf-ld). New user/args.asm prints its
  argv. 5 broken-ELF cases refused. 50 self-tests pass. Gotcha: nasm doesn't expand %1
  inside "strings" — use %defstr/%strcat.
- 2026-10-02: FPU/SSE fix (found in the performance review, docs/performance.md). FPU state
  was never saved on thread switches. arch/i386/fpu.c: CR0 EM/TS off, MP/NE on, CR4 OSFXSR
  (+OSXMMEXCPT) if supported, fninit, clean-state template; fxsave/fxrstor (fnsave/frstor on
  CPUs without FXSR). Each thread has a 512-byte save area; only user threads (uses_fpu,
  set by thread_start_fpu before entering ring 3) are saved/restored, eagerly. Kernel built
  with -mgeneral-regs-only. Test: user/fpu.asm keeps st0/xmm0 while a kernel "wiper" thread
  clobbers them under preemption -> exit 0; with save/restore removed it exits 1 (verified).
  Also: lazy thread reaping (thread_create reuses DEAD slots), stale comments fixed,
  docs/performance.md written. 51 self-tests pass.
- 2026-10-02: Step 7a: two-stage boot. MBR now only loads stage 2 (8 sectors at 0x7E00).
  stage2.asm: E820 map, A20, loads kernel to 1 MiB and initrd.cpio after the kernel's .bss
  in 32 KiB chunks (int 0x13 AH=0x42 into a bounce buffer at 0x10000, then int 0x15 AH=0x87
  copies it above 1 MiB), BOOTINFO = {initrd_phys, initrd_size, e820 count, entries},
  protected mode, jump. Makefile passes LBAs/sizes/INITRD_LOAD (from __kernel_end via nm)
  with -D. Kernel relinked at 0xC0100000. PSE: entry.asm maps the first 4 MiB with one big
  page (no boot page table); vmm_init maps all RAM with 4 MiB pages (one page table for the
  partial last 4 MiB). pmm reserves the initramfs. initramfs/ -> cpio newc (bsdcpio -H newc
  -R 0:0) + generated data/numbers.txt (110 KB, 4 load chunks). kernel/initrd.c checks
  magic, trailer, reservation. Booted first try; 55 self-tests pass.
  Next: 7b, cpio reader + VFS (open/read/close/stat).
