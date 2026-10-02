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
- 2026-10-02: Performance pass (docs/performance.md has the numbers). CLAUDE.md: speed and
  predictability first, binding performance rules, two targets (old i686 PCs tuned for, GPD
  Pocket 4 x86-64/UEFI later). Measurement: cpu.c (cpuid, required features, SEP quirk),
  rdtsc cycle counter calibrated in the background by the PIT IRQ, `make bench` (QEMU
  -icount shift=0,sleep=off: repeatable instruction counts; -cpu pentium3 everywhere).
  Build modes: normal / test (-DSELFTEST) / bench (-DBENCH) in build/, build/test/,
  build/bench/; test code only in its mode. Wins: async serial (8 KiB ring, IRQ 4, OUT2
  bit!) + kprintf buffered (one VGA cursor move per piece), short writes instead of
  spinning; rep movsd/stosd memcpy/memset (11.7x); slot->thread table; PGE global kernel
  pages; CR0.WP; exception-table uaccess (copy_from_user/get_user32/strncpy_from_user, -EFAULT
  via fixup, no page-table walks); sysenter + vsyscall page at 0xBFFFF000 (AT_SYSINFO);
  kmalloc size classes 16..1024 (O(1), 240x faster with 1000 live objects); boot 11.6 ms ->
  0.6 ms. User C test programs via user/lib (crt0 + mini libc). Gotchas: sysenter.asm and
  sysenter.c both made sysenter.o (renamed sysentry.asm); make 3.81 picks the FIRST matching
  pattern rule (user rules must come before the kernel's); QEMU real-time jitter (~5 ms) was
  host noise. 56 self-tests pass.
- 2026-10-02: Step 7 done. fs/initramfs.c unpacks the cpio (newc) archive once into an inode
  tree (fs/vfs.c): names and data point into the archive (zero-copy), a 512-bucket hash on
  (directory, name) makes lookup one probe per path component. struct file + per-thread fd
  table (32); console is fds 0-2 (shared static file). Syscalls: read, write (generic),
  open, openat (AT_FDCWD), close, lseek, _llseek, statx (Linux layout, static_assert 256 B;
  AT_EMPTY_PATH = fstat). read() caps at 256 KiB per call. Programs moved out of the kernel
  into /bin in the initramfs (test/bench builds), user_exec(path, argv) loads them in place;
  user/programs.asm removed. user/files.c: 28 checks (open/read/stat/lseek/close, errors,
  "..", the 110 KB file in order, -EFAULT for unmapped/kernel/read-only-code buffers) pass.
  57 self-tests pass. Benchmarks: open+close 1131, read 4 KiB 1453, statx 1044.
  Next: step 6b (musl, needs Docker) and step 8 (processes: fork/COW, execve, wait4, pipes,
  signals, TTY — BusyBox shell).
- 2026-10-02: Page allocator: mark_range sets whole bitmap words (32 frames) in the middle
  of a range, with an instant count for all-used words. Kernel ready (QEMU icount):
  512 MiB 4181 -> 181 us, 64 MiB 621 -> 119 us. Boot estimate for a real P4: BIOS POST
  dominates (~5-8 s); MyOS after the BIOS ~0.1-0.25 s from IDE, nearly all disk reads.
- 2026-10-02: Step 8 (kernel side) done, BusyBox pending. Page share counts (pmm extra_refs);
  per-process page directories (kernel half copied, heap page tables preallocated so copies
  never go stale, 64 MiB kernel heap); mm.c: areas (VMAs), demand paging, brk, mmap/munmap,
  mprotect; COW fork (vmm_pd_fork, vmm_cow_fault). proc.c: spawn, fork (child starts at
  isr_return with a copy of the syscall frame), execve (new mm first, so it can fail
  safely; #! scripts), exit (reparent, zombies), wait4. Threads: wait queues, run queue,
  timed sleeps (sorted list). Scheduler: free/leftover slots go round-robin to best-effort
  threads. VFS: symlinks, dev nodes, pipes (one-page ring), getdents64, chdir/getcwd,
  dup2, fcntl, writev; poll/select. TTY: line discipline, termios, Ctrl-C -> SIGINT to the
  foreground group; keyboard sends terminal bytes; VGA understands VT100. Signals with
  Linux frames; sysenter returns via iret when EIP changed. 8g: set_thread_area (GDT 6-8
  per thread), uname, clocks from CMOS, nanosleep, prlimit, static PIE, clone, *at().
  Tests: forktest 26, pipetest 27, sigtest 26, systest 27, ttytest 12 checks; exact page
  accounting (no leaks). 62 self-tests pass. Thread switch 94 cycles; fork+exit+wait 51k;
  null syscall 128 (signal check inlined). Bugs found by tests: tests assumed timing that
  only holds with preemption off (fixed with sched_yield); setpgid on a session leader
  is EPERM (correct). Next: BusyBox + bash (Docker).
- 2026-10-02: Step 8 done: BusyBox runs. Alpine's busybox-static 1.36.1 (x86, unmodified,
  downloaded by tools/fetch-busybox.sh with a pinned SHA-256) + 305 applet links in /bin
  (list from `busybox --list` run on MyOS itself), /sbin and /usr/* link to /bin. /init =
  BusyBox shell script -> `sh -l` in a loop; /etc/profile, passwd, group, motd, hostname.
  Fixes found by BusyBox: #! handling moved into program_load (the kernel's own spawn of
  /init is a script); execve must reset FS/GS (the old TLS descriptor is gone -> #GP on
  `pop gs`). `make test-shell` types commands via QEMU sendkey (tools/qemu-type.sh): echo,
  ls, `echo | wc -w`, cat, cd/pwd, uname all work. Unknown syscalls now logged once
  (BusyBox cat tries sendfile64 first). Next: bash.
- 2026-10-02: bash runs (Alpine 5.2.26, dynamically linked, unmodified) as the login shell.
  tools/fetch-alpine.sh: pinned + checksummed Alpine x86 packages (busybox-static, bash,
  musl, readline, libncursesw, ncurses-terminfo-base). Kernel: PT_INTERP (load the
  program at 0x00400000, ld-musl at the first mmap gap; AT_BASE/AT_ENTRY/AT_PHDR) and
  file-backed mmap, demand-paged from the initramfs (VMAs carry inode + offset; splits
  keep offsets). /init starts `bash -l` (or BusyBox sh). Found by bash: musl calls the
  32-bit-time pselect6 (308) when the timeout fits — readline got ENOSYS and bash logged
  out at once; added pselect6/ppoll time32, setfsuid/gid, getpeername/socketcall
  (ENOTSOCK). `make test-shell`: bash runs commands, pipes, loops, BusyBox sh, history
  (up arrow) and Ctrl-C interrupting `sleep 30`. Boot 713 us (330 initramfs files).
- 2026-10-02: Step 8i: writable files (everything still in RAM). fs/ramfs.c: a file starts
  as a pointer into the initramfs and moves into pages of its own on its first change
  (only the bytes it keeps are copied); holes read as zeros; at most half of RAM
  (ENOSPC). vfs.c: create / unlink / rename (Linux's replace rules, no moving a directory
  into itself), O(1) removal with "pprev" links, reference counts (open files, mmap'd
  areas, current directories) so a deleted file in use lives on until let go; directory
  cursors so `rm -r` can delete entries while reading them. open: O_CREAT/O_EXCL/O_TRUNC/
  O_APPEND, read/write access checked. New kernel/sys_fs.c: mkdir(at), mknod(at), symlink(at),
  unlink(at), rmdir, rename(at/at2), chmod/fchmod(at), chown family, utimensat (both time
  sizes), truncate/ftruncate (+64), statfs64/fstatfs64; creat; link -> EPERM. execve needs
  an x bit and can run programs and scripts written at run time. user/fstest.c: 158 checks;
  `make test-shell` now does mkdir -p, >, >>, cp, mv, sed -i, rm -r. The page-leak check
  still balances exactly. Cost: open+close 1395 -> 1441, read 1479 -> 1509, exec+exit
  33194 -> 33631 cycles (reference counts, access checks). Decided: network drivers come
  from Linux through the shim (DESIGN.md); roadmap now lists steps 9a-9e, 10, 12b.
- 2026-10-02: `make bench` now also times boot to a bash prompt: it starts /init and stops
  the clock the first time a program waits for a key. 690 us kernel ready + 6142 us for
  /init and bash = 6.8 ms (QEMU virtual time). Wall clock in QEMU on the Mac: ~0.5 s from
  starting QEMU (0.36 s of that is QEMU, SeaBIOS and the loader copying 3.1 MB).
- 2026-10-02: `make utm` (tools/mkutm.sh): the boot disk inside a ready-made UTM VM
  ("dist/MyOS read-write.utm"): i440FX, Pentium III, 256 MiB, IDE, VGA, e1000, USB off and
  PS/2 on (UTM's defaults give a USB keyboard and remove the PS/2 controller, and the image
  must be an IDE disk, not the "boot ISO"). Settings checked against UTM 4.6.4's source.
- 2026-10-02: Step 9a: the IDE driver sleeps instead of spinning: the drive's IRQ (14/15)
  wakes the waiting thread; READ/WRITE MULTIPLE gives one IRQ per 16 sectors (one per
  sector made QEMU's instruction-count clock jump 128 times per 64 KiB); a channel whose
  IRQ never arrives falls back to polling; one command per channel at a time (sleeping
  lock); drive-cache flush is a separate blockdev op. Disks and partitions are found in
  every boot. fs/bcache.c: 256 one-page buffers, O(1) hash lookup, LRU reuse, write-back
  by a "flush" thread (started on the first change) about once a second + one drive flush
  per batch; sync/fsync/fdatasync call it. The kernel now counts idle time (for `top`, and
  so `make bench` can report CPU work only: QEMU's disk timing varied 10x between runs).
  64 KiB read: 223 722 -> 34 365 cycles of CPU work; cache hit 101 cycles. 7 new self-tests.
- 2026-10-02: Step 9b: files survive a restart. fs/ext2.c reads and writes Linux's ext2:
  superblock/group/bitmap handling, direct + 1/2/3-level indirect blocks, directories
  (entries split/merged in place, a directory's hash-index flag cleared when we change
  it), fast and slow symlinks, device nodes, truncate, rename (in-place replace, ".."
  updated), unlink with the inode freed at the last close. One sleeping lock per file
  system; user buffers are touched before taking it (a page fault on an mmap'd disk file
  needs the lock too). VFS: an fs_ops table (load_dir, create, unlink, rename, evict,
  setattr, truncate, read_kernel, statfs) and mounts; disk directories join the tree on
  first look; EXDEV across file systems (mv copies), EBUSY for a mount's root; stat shows
  the disk's inode numbers and a device number per file system. The first ext2 disk is
  mounted at /data at boot; the initramfs test files moved to /test. Makefile: disks/
  data.img for `make run` (kept), fresh test disks made by mke2fs from known files (1 KiB
  blocks for `make test`, 4 KiB for test-shell); after each run Linux's e2fsck must find
  the disk clean and debugfs must read what MyOS wrote. fstest runs on /data too (158
  checks), user/ext2test.c reads mke2fs's files (double-indirect) and the mount rules.
  The UTM VM got a 256 MiB data disk (kept when the VM is rebuilt). Boot: +36 us of CPU
  work; `make bench` now shows disk waits apart (they vary with QEMU's clock).
  Installed: Homebrew e2fsprogs (mke2fs, e2fsck, debugfs). Also: roadmap step 11b Graphics.
- 2026-10-02: Boot picture, as a trial. Stage 2 switches to VGA mode 13h (320x200)
  while it is still in real mode, copies assets/myos-logo.png (converted by
  tools/mksplash.py to a 64,000-byte grey ramp) onto 0xA0000, and draws a
  progress bar as the kernel and initramfs load. It holds two seconds, then
  returns to text mode 0x03 before protected mode, so the kernel console is
  unchanged. The picture is its own span of sectors between stage 2 and the
  kernel. A trial UTM goes on the Desktop with its own UUID; `make utm` still
  writes dist/MyOS read-write.utm.
- 2026-10-02: Boot picture fixes (review of the trial above): stage2.asm didn't assemble
  (`hold_spins` was never declared), and `xor ax, ax` before setting ES wiped the low half
  of EAX — the disk sector the loader is at — so every kernel chunk after the first came
  from the picture's sectors and the kernel never started (no serial output at all). Now
  AX is saved around it. The picture and progress bar show (checked with a QEMU
  screendump); make test and test-shell pass. Cost: the 2-second `hold` makes boot to a
  bash prompt 2.5 s instead of 0.5 s in QEMU (the kernel's own 6.9 ms is unchanged).
