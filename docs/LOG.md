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
- 2026-10-02: Working ahead on graphics (step 11b), for an XP-like desktop with IceWM on
  X11: drivers/mouse.c, the PS/2 mouse on the 8042's AUX port (IRQ 12): AUX on, IRQ 12 in
  the controller's config byte, defaults, the IntelliMouse "knock" (sample rates 200,
  100, 80 -> ID 3 = wheel, 4-byte packets), streaming on. The IRQ handler only collects
  packet bytes (resyncing on bit 3 of the first byte), decodes 9-bit dx/dy and the
  4-bit wheel and puts the movement in a ring. keyboard.c now ignores bytes that are the
  mouse's (status bit 5). `make test` sends mouse_move/mouse_button through QEMU's
  monitor and checks the decoded packets. Boot +5 us of CPU work.
- 2026-10-03: Deferred-work audit (workflow; 45 items from 4 of 8 areas came back before
  the usage limit) and the fixes it found due, each checked in the code first: the linker
  script fails the build if kernel + .bss pass 4 MiB; only ext2 disks labelled myos-data
  are mounted (a foreign Linux disk in an old PC is left alone — tested); a thread woken
  by an interrupt while the CPU idles runs at once, not at the next tick (disk reads woke
  up to 8 times per 64 KiB, each waiting up to 1 ms); CMOV is now a required CPU feature
  (the kernel is built for i686); kfree catches a double free of a small object (a mark
  in the free object; new self-test). kmalloc+kfree 63 -> 67 cycles.
- 2026-10-03: Step 9c-1: the whole system on a disk. VFS: mounts now COVER a directory
  (Linux-style: lookups continue in the mounted root, ".." climbs out, the covered
  directory can't be removed), the root can be switched before any process exists, and
  an in-memory tree (/dev) can be mounted on a disk directory. Boot finds our file
  systems by label: myos-root -> "/", myos-data -> /data, anything else left alone.
  `make hd` builds an installed-style disk: the boot area plus a 256 MiB ext2 myos-root
  partition (mke2fs -d at an offset; tools/mbr-partition.sh writes both entries);
  `run-hd`; `test-hd` boots it twice (files in /root, /work, /etc survive) and checks
  the root partition with e2fsck and debugfs. Found on the way: (1) a real kernel bug —
  a thread that borrowed an address space (execve loading the new program, the kernel
  spawning /init) got its process's usual one back after sleeping on the disk, so the
  program was written into the wrong address space (crash on a copy-on-write page);
  threads now remember the address space they use; (2) idle-time accounting counted
  other threads' work as idle when the idle thread was switched away mid-wait: the
  interrupt that wakes the CPU now ends idle time; (3) booting from disk took 3.1 s in
  QEMU from root to prompt: a bigger block cache (an eighth of RAM, up to 8 MiB) gave
  2.1 s, and a short spin before sleeping on the disk interrupt 0.3 s (QEMU takes ~2.5 ms
  to wake a halted CPU). Thread switch 94 -> 97 cycles.
- 2026-10-03: Step 9c-2, and the read half of 9e. Grok, while Claude was out of tokens;
  notes in docs/for-claude.md. Page cache (`fs/pcache.c`, 128 direct-mapped pages):
  exec maps the ELF and faults pages in, with one page of read-ahead, instead of
  copying the whole file. A write drops that file's cached pages. `make test`
  (`pcache_selftest`) and `make test-shell` pass. `make bench`: /init + bash CPU work
  6144 -> 1204 µs; exec of the tiny `true` is 33779 cycles (was 33631). FAT32 is now
  a VFS mount at /mnt, read-only (`fs/fatvfs.c`); `fatvfs_selftest` reads HELLO.TXT,
  a long name, a nested file, and the 64 KiB pattern, and checks that create returns
  EROFS. Writing FAT, the installer (9c-3), and DMA/AHCI (9d) are still open.
  Networking was not started: it is step 10, and the stack should follow Linux's
  driver boundary (DESIGN.md, 2026-10-03).
- 2026-10-03: `make utm-hd` (tools/mkutm.sh --installed): a second UTM VM, "MyOS
  installed", with ONE disk holding the whole system (make hd); its disk keeps your
  files, so a rebuild leaves it alone. Tested UTM-style (i440FX, qcow2 on IDE, 256 MiB):
  a file in /root and bash's history (after `exit`) survive a restart.
- 2026-10-03: Step 10a, Grok. The Linux-shaped packet path, not the card yet.
  `sk_buff` (reserve/put/pull), `net_device` with `ndo_start_xmit`, a 32-deep
  receive queue drained by `net_rx_action` (not from the interrupt), NAPI, and
  loopback. PCI configuration space (ports 0xCF8/0xCFC) lists bus 0; QEMU now
  has an e1000 (`-device e1000`) and the scan finds 8086:100e at 00:03.0 IRQ 11.
  `make test`: 94 passed, including `net: loopback delivered` and
  `pci: e1000 8086:100e`. Linux's e1000 source stays out until there is a
  licence file. Notes: docs/for-claude.md. Next: drive that card, one frame.
- 2026-10-03: Step 10b, Grok. Original e1000 (not Linux's driver) on the
  existing `net_device` / NAPI calls, plus ARP and ICMP (`net/ipv4.c`).
  QEMU's gateway 10.0.2.2 answers an echo to 10.0.2.15. `make test`: 96
  passed, including `net: ping 10.0.2.2 ok`. QEMU drops receives for a
  second after the receiver is enabled; the first transmit waits that out.
  No speed claim: nothing was benchmarked. No sockets, so the shell still
  cannot ping. Notes: docs/for-claude.md.
- 2026-10-03: Step 10c, Grok. UDP, a DNS A lookup to 10.0.2.3, and one TCP
  client (handshake, one segment in flight, close). `make test`: 99 passed,
  including `net: dns example.com is ...` and `net: tcp example.com HTTP ok`.
  That HTTP line needs the host to reach example.com. Still no socket
  syscalls, so BusyBox ping/wget/udhcpc do not work yet. Notes: docs/for-claude.md.
- 2026-10-03: Step 10d, Grok. UDP and raw ICMP sockets (`net/socket.c`, the
  direct syscalls 359-373 and `socketcall`), `/etc/resolv.conf`, and
  `alarm`/`setitimer` (ITIMER_REAL; SIGALRM from the timer tick). BusyBox
  `ping -c 2 10.0.2.2` gets 2 of 2 replies; `ping example.com` resolves the
  name through musl. Google and example.com drop pings from this network (the
  Mac gets no reply either). Fixed in the terminal: keys typed while a program
  ran were lost when bash switched back to raw mode. `make test`: 99 passed;
  `make test-shell` OK. No speed claim. Next: TCP sockets for `wget`.
- 2026-10-03: Network benchmark, Grok. `user/netbench.c` runs unchanged on MyOS
  (`make bench`) and on Debian 12's Linux 6.1 (`tools/linux-netbench.sh`), same
  QEMU and e1000, host echo by `tools/echo-server.py`. UDP request/response, client
  side, instruction count: MyOS 3 452, Linux 12 045 (12 112 with
  `mitigations=off`). Not real cycles; see docs/performance.md. TCP not measured
  (no TCP sockets yet). Fixed on the way: the e1000's first transmit busy-waited
  with interrupts off and hung a syscall sent soon after boot; ARP now retries
  3 times, 500 ms apart, instead. Network security gaps listed in docs/gaps.md.
- 2026-10-03: Step 10e, Grok. TCP sockets, client side: `net/tcp.c` is one
  connection per socket (16 KiB rings, write() returns once copied, up to 4
  segments in flight, MSS option, retransmit 250 ms doubling, RST checks), and
  `net_thread` runs receive and TCP timers when no program is in the kernel.
  BusyBox `wget` fetches example.com and a 149 266-byte file with the right
  md5. netbench TCP echo, instruction count: MyOS 5 480, Linux 6.1 20 861.
  `make test` 99 passed; `make test-shell` OK (now with wget).
- 2026-10-03: Hidden I/O costs, Grok. `-icount` counts instructions, so three
  real-hardware costs never showed: PIO disk reads (32 768 port reads per 64 KiB,
  ~4 ms of CPU on a P4), `wbinvd` per packet in the e1000 driver, and a 2 048-read
  status spin before each disk sleep. Added bus-master DMA to `drivers/ata.c`
  (step 9d, PIIX/ICH; PIO fallback; test read at boot): 64 KiB read 41 788 ->
  1 250 instructions of CPU work. e1000 uses a compiler barrier instead of wbinvd
  (PC DMA is coherent). `pci_init` now runs before the disks. `make test` 99
  passed, `make test-shell` OK.
- 2026-10-03: Network security review and three fixes, Cursor. Read the stack
  against the public record (Ripple20 CVE-2020-11896, AMNESIA:33, SACK Panic
  CVE-2019-11477, RFC 6528/6056, Kaminsky and SAD DNS); threat model, the rules
  new network code must follow, and the plan are now in `docs/security.md`.
  Fixed: (1) `ip_send` no longer blocks — a frame whose next hop is unknown waits
  in a 4-slot queue while an ARP request goes out, and the reply sends it. This
  also closes a race where `tcp.c`'s one static `seg` buffer could be overwritten
  by another thread while `arp_resolve` slept, putting the wrong segment on the
  wire. (2) ARP resolution used to be impossible from inside the receive path:
  `net_rx_action`'s `rx_busy` guard made its own call a no-op, so the reply was
  never drained and every such send stalled 1.5 s and failed. (3) The neighbour
  cache now learns only from a reply to a request of ours (a request for our
  address is answered from the MAC on the frame, never recorded), entries expire
  after 60 s, and a full table evicts the least recently used — so forged ARP
  can no longer evict the gateway. `make test` 99 passed, ping/DNS/TCP to
  example.com all still work and no skb leak. Still open in docs/gaps.md: the
  CSPRNG and everything that needs it (ISN, ports, DNS IDs), rate limiting,
  broadcast ICMP replies, the e1000 short-frame padding leak, and fuzzing.
- 2026-10-03: Second security pass over the network stack, Cursor. Reviewed
  `net/socket.c` and `drivers/pci.c` too (socket.c was clean — every user length
  clamped, sockaddrs zeroed before copy-out, iovlen capped). Confirmed there is no
  IPv6 anywhere, which makes a good share of AMNESIA:33 unreachable code for us.
  Seven more fixes: IP broadcast is no longer received at all (we were answering
  broadcast ICMP echo, a smurf amplifier, and handing broadcast UDP to sockets as
  if unicast); a ping reply now has to come from the host we pinged; an oversized
  ICMP echo still reaches raw sockets instead of being dropped; the e1000 zeroes
  the padding of short frames, which were carrying up to 18 bytes of the previous
  packet in that slot out onto the wire; kernel DNS uses an ephemeral port per
  lookup instead of a fixed 53000 that any local program could bind to break it;
  a DNS reply must now match the question we asked (`qdcount` 1 and a byte compare
  of the question section); and concurrent `dns_resolve` calls can no longer swap
  answers through the shared globals. `make test` 99 passed, ping/DNS/TCP to
  example.com all still work. Full list with what is still open in
  `docs/security.md`; the remaining items are mostly one bug — no CSPRNG — plus
  rate limiting, a TCP persist timer, and CAP_NET_RAW.
- 2026-10-03: Honest accounting on the security pass, Cursor. Re-checked the fixes
  and found one more real flaw: `parse_a` takes the first A record in a DNS answer
  without checking which name it belongs to (RFC 5452 response matching — the
  question section is verified, the answer records are not). Left unfixed on
  purpose: requiring the owner name to equal the question breaks every CNAME, so
  doing it right needs chain handling and its own step. Also recorded three things
  the earlier write-up overstated or skipped: a forged ARP reply can still win the
  race on a request we did send (only unsolicited ones are now ignored), `ip_send`
  checks the next hop rather than the destination so we still send to broadcast,
  and a local program can flush the 4-slot ARP queue. Added a "what this review
  does not cover" section to `docs/security.md`: it was reading not testing, the
  fixes are verified only against the happy path, and ext2 / the page cache parse
  attacker-controlled disk bytes with none of this scrutiny.
- 2026-10-03: Disk I/O made fast, and three new disk drivers, Cursor. Rodger asked
  for disk access "WAY faster than Linux" on IDE, SATA, NVMe and virtio. The reason
  MyOS was slow: `read()` went through the block cache 1 KiB at a time, and that
  cache holds only 2 MiB of 1 KiB blocks, so an 8 MiB file was never warm. Now
  `read()` is a copy out of a rewritten page cache (4-way, a quarter of RAM),
  misses read ahead up to 128 KiB with one scatter-gather DMA command straight into
  the cache's frames, and writes go out as one command per run of dirty blocks.
  New drivers from the specs: AHCI (`sda`), NVMe (`nvme0n1`), virtio-blk (`vda`),
  plus IRQ sharing. Result, instructions per call vs Debian's Linux 6.1 in the same
  QEMU: warm 64 KiB read 18 192 vs ~37 030, cold 64 KiB ~28 500 vs 77 015-124 784,
  64 KiB write ~130 000-150 000 vs 478 792-734 799 — 2.0-5.6× fewer on every
  operation and disk type (table in `docs/performance.md`). `make test` 99 passed,
  `make test-shell` OK (e2fsck clean), `tools/test-disks.sh` OK on all three new
  drivers. QEMU only; real-disk timing still to measure.
- 2026-10-03: Fuzzing harness for the packet parsers, Cursor. `tools/fuzz/` builds
  the five net sources with the host clang and links them against stub versions
  of the 20 kernel symbols they reference (`i686-elf-nm -u` named them), so they
  run under AddressSanitizer: a one-byte overrun becomes a located abort instead
  of silent corruption that boots fine. Only `interrupts.h` and `uaccess.h` are
  replaced — they are inline i386 assembly — so the net code is compiled
  unmodified and anything found is a real bug. Apple's clang has the sanitizer
  runtimes but no libFuzzer, so `driver.c` is the mutation loop:
  `-fsanitize-coverage=trace-pc-guard` for feedback, a mutator that knows wire
  lengths are big-endian, a death callback to save the input that killed it, and
  an alarm so a parser that never returns counts as a failure too.
  Five targets: `wire` (raw bytes to on_ip), `ip` (header repaired, body hostile),
  `arp` (with a request outstanding so `remember` is reachable), `dns` and `tcp`.
  The last two let the stack send first, read the transaction id or sequence
  number out of the frame, and put it in the reply, so the bytes reach `parse_a`
  and the TCP state machine instead of being rejected at the door. 85 seeds shaped
  after the CVEs in `docs/security.md` — Ripple20 length lies, AMNESIA:33 pointer
  loops, SACK-panic tiny MSS, options with a zero length byte.
  Result: 25M runs, no crash, no sanitizer report, no hang, with `make
  fuzz-coverage` showing 100% of `on_arp`/`udp_input`/`skip_name`, 98% of `on_ip`,
  95% of `parse_a`, 86% of `tcp_input`. Two harness bugs of my own on the way:
  `pump()` was eating the SYN before the target could read its sequence number
  (so TCP never left SYN_SENT and `synack` covered no more than a blind reset),
  and I called `tcp_timers(0)` where the kernel calls `tcp_timers(timer_ms())`,
  so nothing was ever reaped and the 16-connection table ran dry after 16 runs —
  which the harness reported as a leak. Checked before believing it; neither was
  a kernel bug. The harness also demonstrates the open `parse_a` flaw: the
  `dns/foreign-a` seed makes the resolver return an address taken from an answer
  about `evil.example` when it asked about `example.com`, and the run passes,
  because accepting the wrong address is not a memory error.
  Ran it out to 125M runs total: still no crash. Coverage stopped moving after the
  first 25M, so the next 100M bought nothing — the way to find more is a target
  that reaches code these five do not, not a longer campaign. `remember` at 63.6%
  is the clearest hole: the missed lines are the ARP cache's LRU eviction, which
  needs more neighbours in play than any target creates.
- 2026-10-03: `make attacks` — tests that assert an attack is rejected, Cursor.
  The fuzzer proves 125M malformed packets do not corrupt memory; it cannot tell
  whether the stack did the right thing, because accepting a forged ARP reply or
  answering a broadcast ping is not a memory error and no sanitizer complains.
  So `tools/fuzz/attacks.c` performs the attacks and checks the behaviour that
  stops them: 30 checks, ~2 seconds. Extracted the packet builders and the reply
  pump out of targets.c into `tools/fuzz/packet.c` so the fuzzer and the tests
  share them.
  Half the checks are positive controls on purpose — "no reply was sent" passes
  perfectly when the stack is broken and never replies to anything, so every
  "must be ignored" has a "must work" beside it. That caught two bugs in my own
  tests, both the same mistake: `pk_pump()` drains the send queue, so a test that
  looked for a reply after pumping found nothing and declared the stack silent.
  Same class of mistake as the SYN-draining bug in the fuzz harness yesterday.
  Result: 30 passed, 0 failed, 1 known bug open. The known one is `parse_a`
  accepting an A record about `evil.io` when we asked about `example.com`; it is
  recorded with `known()` rather than `ok()`, so it reports without failing the
  build and prints `FIXED` the day it starts passing. Still untested: the e1000
  short-frame padding, because that driver needs PCI and MMIO and is not in this
  build — it wants a wire capture via `qemu -object filter-dump`.
- 2026-10-03: RFC 5452 response matching in `parse_a`, Cursor. The last known bug
  from the security review: the parser took the first A record in the answer
  section whatever name it was about, so a reply that passed the transaction id,
  the port, the source address and the question could still carry an address for
  something else. That is the check the Kaminsky attack evaded.
  The reason it was not a one-liner: requiring the owner name to equal the
  question breaks every CNAME, because there the A record is about the end of the
  chain, not the question. So `parse_a` walks the chain — a record counts if its
  owner is the question or the target of a CNAME that already counted, up to
  CNAME_MAX hops — and skips records about other names rather than stopping at
  them, so a forged record placed first cannot hide the real answer behind it.
  Comparing names means resolving compression, so `name_expand` is now the one
  place a pointer is followed instead of stepped over. Following pointers is how
  CVE-2020-24338 hung a stack forever; this is safe because a pointer must land
  strictly further back than the last one did, so the targets strictly decrease
  and a cycle is unbuildable, with a hop limit keeping the cost linear. Names are
  lowercased first: a name ignores case on the wire (RFC 4343) and a server may
  answer EXAMPLE.com, so a byte compare would have rejected good replies while
  looking like security.
  `make attacks` printed `FIXED` on the next run, which is what the `known()`
  mechanism is for. Promoted it to a real check and added five more around it:
  a foreign record is skipped rather than fatal, a CNAME chain is followed, a
  CNAME loop terminates, a self-referential pointer is refused, and a reply
  differing only in case is accepted. 36 passed, 0 failed, no known bugs left.
  Verified beyond the tests: `make test` still reports 99 passed / TEST OK with
  a real lookup, `net: dns example.com is 172.66.147.243`, and the HTTP fetch
  over TCP still works. Re-fuzzed the dns target since `name_expand` is new code
  doing the dangerous operation: coverage went from 230 to 256 edges and 20M runs
  found nothing.

## 2026-10-03 — 9d-2: writes through the page cache, a cache as big as free RAM
- Writes now copy into the file's pages in the page cache and mark them dirty;
  the disk gets runs of up to 32 pages as one command (on `fsync`, `sync`, once a
  second, or when RAM/8 — at most 32 MiB — is dirty). File data no longer passes
  through the block cache at all, which also removed a lookup per block from
  reads. 64 KiB write: ~130 000 → ~100 800 instructions on all four disk types
  (NVMe 150 174 → 101 006, no bounce copies); cold random 4 KiB 6 335 → 4 204.
- The cache may hold all free RAM; when the page allocator runs out it takes
  clean pages back (`pcache_reclaim`). Found by running the benchmark in 20 MiB:
  the first version only cleared "used" bits on its first call, freed nothing,
  and a page fault killed `init`. Now a second round takes the pages.
- The cache slot went back to 16 bytes (one set per cache line): warm 64 KiB read
  18 736 → 18 144.
- Why all of RAM: on the old PC's disk a miss costs ~4 000 hits, so each extra
  "9" of hit rate makes the average access 10× faster — the math is in
  `docs/performance.md` ("Cache sizing").
- New test: `ext2test` writes a 300 000-byte pattern in odd pieces, overwrites,
  truncates, leaves a gap; the Makefile checks the bytes on the disk with debugfs.

- 2026-10-03: TCP persist timer — a zero window no longer stalls forever, Cursor.
  Rodger asked for the one bug from the review that needed a feature rather than a
  corrected check. `output()` computes `limit = min(snd_wnd, 4 * mss)`, so with the
  peer advertising a window of 0 the send loop never ran — and the `arm()` call that
  starts the retransmit timer lives *inside* that loop. So `rto_at` stayed 0, which
  means "timer off", and nothing would ever happen on that connection again: no
  retransmit, no timeout, no error. `tcp_write` had already told the caller the data
  was accepted and a blocking write waited in `nap()` for good. Two bytes set to
  zero by the peer, reachable from the SYN-ACK onward.
  The fix is RFC 1122 4.2.2.17: let **one byte** past a shut window. The peer has to
  drop a byte that is past its window but has to answer with its current window,
  which is both how a lost window update is recovered from and how we learn the peer
  is alive. Because that byte counts as sent, the ordinary retransmit timer repeats
  it — the repetition *is* the persist timer, so no second timer and no new field in
  `struct tcp_conn`. Same change also arms a timer when the probe itself fails to go
  out because the card's ring was full, which was a second way to the same dead end;
  the comment there had always claimed "the timer resends", and there was no timer.
  Two things did not fall out for free. A shut window is not a lost packet — the peer
  is alive and its application has stopped reading, which can legitimately last
  minutes — so probing gets a longer leash and backs off to 60 s rather than 4 s:
  four minutes to give up instead of twenty seconds. Bounded all the same, which
  deviates from the RFC's "stay open as long as the peer answers" on purpose, because
  something that never gives up is a hang and a hang is what the attacker wanted.
  Linux does the same with `tcp_retries2`. An orphaned connection is still capped at
  `ORPHAN_MS` (10 s) by `end_at`, so the longer leash cannot hold a connection slot
  after the application is done.
  And a window that reopens has to *clear* that backoff. `take_ack` deliberately
  ignores an ack that acknowledges nothing new, which is exactly what a pure window
  update is — so without an explicit reset the probes stayed charged against the
  connection, recovery crawled on a minute-old deadline, and the next timer killed a
  connection that had just become healthy. That is a bug the fix introduces if
  written carelessly, so it gets its own check.
  Four new checks in `make attacks` (40 passed, 0 failed), and each was run against
  the reverted code to confirm it fails there. Worth doing: the first version of the
  backoff check passed either way, because it advanced the clock by one ordinary
  retransmit interval while the stale deadline the bug leaves behind is a minute out,
  so the timer it meant to provoke never fired and the test measured nothing.
  Re-fuzzed: 3M runs on the tcp target and 2M on each of the other four, no crashes,
  and the tcp target found 3 new inputs — the probe path is new reachable code.
  `make test` still reports 99 passed / DISK OK / TEST OK with a real DNS lookup and
  a real HTTP fetch over TCP. Kernel 160404 bytes.
