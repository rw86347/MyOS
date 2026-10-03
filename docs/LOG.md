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

- 2026-10-03: A CSPRNG, and the four predictable numbers it unblocked, Cursor.
  Rodger's summary of what was left: "mostly one bug wearing four hats — no
  CSPRNG, which blocks the keyed ISN, random ephemeral ports, random DNS
  transaction IDs, and per-destination IP IDs — plus rate limiting and
  CAP_NET_RAW." That is the right diagnosis, so the generator came first.
  `kernel/random.c` is ChaCha20 with fast key erasure: every block of output
  also produces the next key and the old one is overwritten, so reading kernel
  memory now does not reveal the bytes already handed out. ChaCha20 suits this
  machine — 32-bit adds, xors and rotates, no tables to miss in the cache, no
  SSE, which matters because the kernel may not touch the FPU registers at all.
  The cipher is the easy half. Neither target CPU has RDRAND or RDSEED (Intel
  added them years later), so there is no generator to ask and the entropy has
  to be *measured*. What is measurable is how long things take: a read of port
  0x80 is an ISA bus cycle, and the cycle counter says how long it really took,
  which is not constant on real hardware because DRAM refresh, bus contention,
  cache state and SMIs all move the low bits. 512 samples, keeping the second
  difference (the change in the interval, not the interval — only its variation
  is unpredictable), folded into a rotating pool. Linux does the same in
  `try_to_generate_entropy()` when it has nothing better. Honest limits written
  down in docs/security.md: each sample is worth maybe half a bit to two bits,
  which is an estimate and not a measurement, and under QEMU with `-icount` the
  cycle counter is deterministic so the emulator gets the same key every boot.
  A broken ChaCha20 still produces output that looks perfectly random, so no
  statistical test would catch a wrong rotation — `kernel/random_test.c` checks
  it against the RFC 8439 vector at every boot instead. 107 self-tests now.
  Then the four, each a few lines once the generator existed. The ISN is
  `M + F(four-tuple, secret)` per RFC 6528, M a 4 us timer so numbers still
  climb with time; it was `timer_ms() * 64000 + 0x10000000`, the same value for
  every connection in a millisecond. Ephemeral ports are random in
  49152..65535 through one shared `ip_ephemeral_port` — random choice, not a
  random start then a scan, because with a scan one port tells you the next.
  The DNS transaction id is random (it was a counter: one subtraction from free
  after a single observed lookup, which is Kaminsky's arithmetic). The IP ID is
  per destination in 64 secret-keyed buckets with random starts; one global
  counter let any peer subtract two ids and read off the machine's whole
  traffic rate, which is what nmap's idle scan counts. `getrandom` and
  AT_RANDOM were on the same xorshift — and AT_RANDOM is what musl builds its
  stack guard from, so that was a predictable canary.
  Rate limiting is a token bucket, 64/s with a burst of 64, in 16 slots. The
  key is *where the reply is going*, so the protected party is the victim being
  aimed at; it is per-peer because a global counter is itself an oracle (watch
  it run out and you learn someone else is talking to us — CVE-2016-5696, SAD
  DNS); and the cutoff is jittered because a limit at exactly N is a clean edge
  to measure. Covers echo replies, ARP replies and the reset for an unknown
  connection. Not the ACK for in-window data: that one is required, not a
  reflector.
  CAP_NET_RAW is the one item that cannot be done, and the honest reason is
  that every uid is 0 and `setuid` is a stub, so the check would be granted to
  everything and would only look like security. What *was* possible: a raw
  socket could send any ICMP type, which is a weapon aimed at other hosts — a
  redirect reroutes a neighbour's traffic, a forged "fragmentation needed"
  tears down connections between two other machines. Nothing in the tree uses
  SOCK_RAW yet and ping sends exactly one thing, so a raw socket may now send
  an echo request and nothing else. That removes the attack rather than gating
  it. The remaining half (any program may open the socket and read every ICMP
  packet) is documented as blocked on users.
  11 new checks in `make attacks` (51 passed, 0 failed), and a new
  `make verify-tests` that reintroduces each of seven bugs by patching the
  source and checks a test notices. It earned its keep immediately: three cases
  reported the *wrong* test failing, because a patch and a build inside the same
  second leave make thinking the binary is current, so the previous case's
  binary was answering for the next one. Same trap as earlier today.
  The fuzzer needed work too. kernel/random.c is compiled into that build for
  real — a stand-in would mean the tests were not exercising the generator the
  kernel uses — but the generator carries its key forward, so the stream used
  to depend on how many runs had gone before, and the rate limiter now *drops*
  packets on an unpredictable budget. A saved crash would have quietly stopped
  reproducing. Each run re-keys now: 40% of the throughput (167k to 100k
  runs/s) for a crash that can actually be replayed. 3M runs on each of the
  five targets, no crashes, and 22 new corpus inputs between them.
  Measured, per the rules: boot CPU work 1093 -> 1111 us (+18), slot jitter
  unchanged at 4 us, both null-syscall costs unchanged, kernel +2108 bytes.
  QEMU undercounts the boot cost badly here — it charges one instruction for a
  port read that takes a microsecond on a real P4 — so expect nearer 0.5 ms on
  the target, which is why the sample count is 512 and not 5000.
  `make test`: 107 passed, 0 failed, DISK OK, TEST OK, with a real DNS lookup
  and a real HTTP fetch over TCP still working.

## 2026-10-03 — 9d-3 part 1: read-ahead that doesn't wait
- Rodger: "one command at a time" is a big issue. Split it into steps; this is
  the first. New block-layer call `blockdev_submit`: a request that returns at
  once and calls `done` when the disk finishes (for now a worker thread per disk
  runs them). The page cache uses it to read the next read-ahead window while the
  program copies this one; pages on their way are pinned and marked, and a reader
  who gets there first sleeps until they arrive.
- Found on the way: the IDE driver's short spin before sleeping would now spin in
  the worker while the program waits for the CPU. It spins only when nothing else
  could run.
- Measured honestly: QEMU can't show the gain (its disk is RAM, and its throttle
  saves up credit while the guest computes). Cost: +1–3% instructions per read.
  The win is for real disks, estimated in `docs/performance.md`.
- Tests: `kernel/bio_test.c` (submit doesn't wait, same bytes), pcache reads
  `numbers.txt` in order with 68 of 71 pages read ahead. `make test` 114 passed,
  test-shell, AHCI/NVMe/virtio OK, and the 12 MiB low-memory run is fine.

## 2026-10-03 — The live disk's software
- Rodger wants an XP-like live disk with a word processor, dev tools, the MyOS
  source, a package manager, and logins for several users. `live/packages.txt`
  lists them in groups; `tools/fetch-live.py --update` resolves them against
  Alpine 3.20 x86 (main + community, `so:`/`cmd:` dependencies) and pins all
  239 packages with SHA-256 in `live/packages.lock`; `make live-root` rebuilds
  from the lock.
- Output is one archive, `build/live/rootfs.tar` (8 814 entries), not a folder:
  the Mac can't unpack Linux trees faithfully (case, and files owned by root;
  setuid `passwd`, `doas`, `bbsuid` keep their bits in the archive). It has
  apk's database (`lib/apk/db/installed`, all 239), `/etc/apk/world`, the source
  in `/usr/src/myos`, and `live/overlay` on top: BusyBox init with a console
  login, xdm (commented out until /dev/fb0), IceWM with `OpaqueMove`/
  `OpaqueResize`, a passwordless live user "myos" in wheel, `doas`.
- Alpine's 14 install scripts can't run on the Mac; `/etc/myos/rc.boot` runs
  them on the first boot, in install order.
- Size before compression: base 8.3 MiB, login 9.4, desktop 58.4, office
  (AbiWord) 40.0, dev 238.6, source 1.5; 357 MiB in all, 151 MiB of packages.
- Not checked: Alpine's apk reading that database (Docker wasn't running). Not
  yet possible on MyOS: separate users (every process is root), threads
  (GLib, gdb, adb), more than 32 memory areas per process (X, gcc, AbiWord).

## 2026-10-03 — audit of the kernel outside the network stack

Pointed the method that had been used on the network code at the filesystem,
the syscall layer and the drivers, with two attackers in mind: a crafted disk
image, and a hostile unprivileged program. Fifteen bugs, thirteen fixed.

The important one is not a bug but a wrong belief: `copy_to_user` and
`copy_from_user` **can sleep**, because a fault on an `mmap`'d file page is
handled by the page cache, which waits for the disk. "A syscall is atomic
because interrupts are off" is therefore false, and three ring buffers — the
pipe, the socket datagram queue, and both TCP rings — had committed their
head/tail/length *after* the copy. Since a `fork` shares those objects, two
processes could make the counters diverge, underflow an unsigned difference,
and turn the next call into an unbounded out-of-bounds kernel write. All three
now commit before copying, and the TCP rings clamp as well, so a raced count
cannot leave the array. The class is written up in `docs/gaps.md`, because the
rest of the kernel has not been checked for it.

ext2 gave up seven out-of-bounds accesses against a crafted image, all from
trusting a field on the disk: an unchecked `rec_len` that put a 4-byte write
anywhere in the 64 KiB past a page, a `name_len` never compared with `rec_len`
that made `ls` print the next page, an `inode_size` that was not required to be
a power of two, group counts that could exceed the bits in a bitmap block, a
fast symlink reporting 196 bytes of unzeroed heap through `readlink`, an
ignored `load_symlink` failure that dereferenced NULL in ring 0, and a
`blocks_count` of zero dividing by zero at mount. Real images still mount and
e2fsck stays clean.

An unprivileged program could halt the machine with one `rt_sigreturn`: `gs`
and `fs` were restored from the program's own signal frame after checking only
that the low bits said ring 3, and a selector like `0x0B` faults in `pop gs`
while still at CPL 0, which is fatal. `gdt_user_seg_ok` now checks the live
descriptor, which also handles a TLS slot nobody has filled in.

Also: the network stack now drops a packet whose *source* is a broadcast,
multicast, loopback or our own address. The destination was already checked,
but the source is what decides where a reply goes — and it let an attacker
choose which per-destination IP-ID counter a reply disclosed, which is the idle
scan the per-destination counters exist to stop. Seven new checks in
`make attacks`, all verified to fail without the fix; `make verify-tests` now
reintroduces eight bugs and requires each to be caught.

Left open and written down: the e1000 only reclaims transmit descriptors from
the receive poll, so 15 sends with no inbound traffic wedge transmit with
`EAGAIN`; `select` only handles the first 32 bits of an `fd_set`; and there is
still no hostile-disk-image fuzz target, which is the gap that would have found
the ext2 bugs instead of a reading.


## 2026-10-03 — Threads (for GLib)
- A process is now a Linux thread group. `clone(CLONE_THREAD|CLONE_VM|CLONE_SIGHAND
  |CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID)` — exactly what musl's
  `pthread_create` asks for — adds a thread to the calling process: same memory,
  files and handlers, its own registers, stack, TLS, signal mask and tid
  (`gettid`, 224). Tids come from the pid counter.
- `futex` (240) and `futex_time64` (422) in the new `kernel/futex.c`: WAIT, WAKE,
  REQUEUE, CMP_REQUEUE, private or not, with timeouts. A hash of wait queues
  keyed by (address space, address); interrupts are off for the whole call, so
  check-and-sleep is atomic on one CPU.
- `set_tid_address` and `CLONE_CHILD_CLEARTID` work: a dying thread writes 0 to
  its tid word and futex-wakes it, which is how `pthread_join` returns.
- `exit` (1) ends one thread; `exit_group` (252) and fatal signals end them all
  (the others get a SIGKILL and die on their way back to user mode; the last one
  out tears the process down). `execve` kills the siblings and waits for them;
  `fork` copies only the calling thread.
- Signals: masks are per thread; `kill` marks the process and wakes one thread
  that doesn't block the signal; `tkill`/`tgkill` and CPU faults go to one
  thread. A one-word `sig_check` flag per thread keeps the return-to-user check
  at a single load (a null syscall got 3 cycles cheaper, see performance.md).
- `mm_fault` now survives a sibling thread's `munmap` while it sleeps reading a
  file page (it holds the file and looks the area up again).
- Test: `user/musl/pthreadtest.c`, the first test linked against Alpine's real
  musl (`musl-dev` added to `tools/fetch-alpine.sh`; new Makefile rule for
  `user/musl/*.c`). 12 checks: mutex under contention, condvar, `__thread`,
  join values, 20 detached threads, `pthread_kill`, process-directed `kill` with
  masks, `sem_timedwait` timeout, fork/exit/exit-from-thread/main-exits-first/
  execve from a threaded process. `make test` (115/115), `make test-shell` pass.
- Still missing (`docs/gaps.md`): cross-process shared futexes, WAIT_BITSET /
  WAKE_OP / PI / robust lists, and `MM_AREAS` = 32 caps a process at ~13 threads.

## 2026-10-03 — The live disk gets a normal userland (no BusyBox)
- Rodger: BusyBox is right for testing the kernel, not for the live disk; do it
  the way other distributions do. They all ship the real tools on the root file
  system (Debian, Fedora, Void, Arch); BusyBox shows up only in some initramfs
  images (Ubuntu's initramfs-tools, Arch's mkinitcpio) for early boot. MyOS now
  does the same: BusyBox stays in the test initramfs (`make test`, unchanged);
  the live disk has none.
- `live/packages.txt`: GNU coreutils, findutils, grep, sed, gawk, diffutils, tar,
  util-linux (+ mount, lsblk, fdisk...), procps-ng, psmisc, htop, iproute2,
  net-tools, ping; dash as /bin/sh (Debian's choice), bash for people; man pages
  for the commands people type (mandoc, man-pages and 40 -doc packages — not
  Alpine's "docs", which adds every library's API reference: 113 MiB).
  `!busybox` forbids it: the build stops and prints who wanted it. It also goes
  into `/etc/apk/world`, so apk on MyOS refuses it too.
- Init: Alpine has no BusyBox-free init for its OpenRC scripts (openrc-init is
  compiled out, no sysvinit), so process 1 is **runit**, set up like Void Linux:
  `/sbin/init` -> `runit-init`; `/etc/runit/1` (mounts, first-boot install,
  hostname), `2` (`runsvdir` on `/var/service`), `3` (stop services, sync,
  read-only); services in `/etc/sv` (`agetty-console` enabled; `agetty-tty1`,
  `agetty-ttyS0`, `xdm` ready); runit's control files and supervise folders in
  /run, so the root can be read-only. `reboot`, `poweroff`, `halt`, `shutdown` go
  through `runit-init`. Login is util-linux's agetty + login, through PAM.
- Alpine's install scripts call BusyBox's `adduser`/`addgroup`/`add-shell`:
  small front ends over shadow in `/usr/sbin` keep them (and later `apk add`s)
  working. Tested against the exact calls in Alpine's scripts.
- `tools/fetch-live.py`: `!name` forbids; a provider named in packages.txt wins
  (dash-binsh over busybox-binsh for /bin/sh); apk's `install_if` is followed
  (util-linux brings lsblk/findmnt/blkid, bash-completion the completions);
  triggers on first boot now get the folders their patterns match (they got the
  version before, so pango's wrote "1.52.2-r0.cache"), and scripts run from /
  (alpine-baselayout's `chown root:shadow etc/shadow` is relative); overlay
  symlinks are kept.
- Kernel: process 1 is `/init`, else `/sbin/init`, `/etc/init`, `/bin/init`,
  `/bin/sh` — Linux's order — so a distribution's root file system boots its
  own init.
- Result: 368 packages, 431 MiB before compression (base 32.7, init 9.8, desktop
  56.2, office 40.0, dev 253.3, docs 32.4, source 5.6). Not bootable yet: the
  kernel needs `vhangup`, `sethostname`, `reboot`, `mount`, /proc, /dev/tty1
  (ROADMAP 9c-3).

## 2026-10-03 — A Linux-shaped screen driver (step 11b)
- Display drivers now have Linux's DRM shape (`docs/DESIGN.md`): `include/drm/`
  holds the subset of Linux's headers one screen needs (modes, connector,
  simple display pipe, framebuffer, fourcc, VRAM helper), at Linux's paths,
  so a driver ported later keeps its `#include`s and callbacks.
  `drivers/gpu/drm/drm_core.c` has the VESA mode table up to 2560x1600 and
  the probe/mode-set glue.
- First driver: our own `drivers/gpu/drm/tiny/bochs.c` for QEMU/UTM's std VGA
  (PCI 1234:1111, the "Bochs DISPI" registers). Any standard mode that fits
  video memory and is a multiple of 8 wide; 1920x1200 is chosen at boot.
- The console was split: `drivers/vt.c` keeps the VT100 parser and the text
  in RAM; the old 80x25 screen and the new pixel console (`drivers/video/fbcon.c`)
  are backends. fbcon uses the VGA BIOS's 8x16 font, which stage 2 now copies
  to 0x2000 (`bootinfo.font_phys`), drawn 2x at 1920x1200: 120x37 characters.
  Scrolling pans the screen (one register write); `TIOCGWINSZ` follows the size.
- `/dev/fb0` (29:0) via fbdev emulation (`drivers/gpu/drm/drm_fbdev.c`,
  `drivers/video/fbmem.c`): Linux's `fb_var_screeninfo` / `fb_fix_screeninfo`,
  GET/PUT_VSCREENINFO (mode changes at run time), GET_FSCREENINFO, PAN_DISPLAY,
  BLANK, and `mmap`. Device mmaps are a new kind of area (`VM_PFNMAP`): pages
  are mapped on fault, shared by `fork`, never freed. `KDSETMODE` hands the
  screen to a program and back.
- Video memory is write-combining: `arch/i386/pat.c` sets PAT entry 1 to WC
  (`ioremap_wc`), mapped with 4 MiB pages. Uncached on CPUs without PAT.
- Tests: kernel self-tests for the mode helpers and the Bochs driver (`kernel/drm_test.c`),
  `user/fbtest.c` (43 checks: struct sizes, ioctls, drawing through mmap, fork,
  mode change and back, bad requests), and a QEMU screendump checked by
  `tools/check-screen.py`. `make test` passes (131 self-tests).
- Speed: see performance.md. The first fbcon made boot 27x slower (pixel-by-pixel
  glyphs); unrolled glyphs and skipping blank cells on a fresh screen got it to
  3.3 ms of CPU. The worst scroll (a full redraw) is in gaps.md.
- UTM: choose the "VGA" display (`-vga std`). Other display cards and real
  hardware keep the text screen until the VESA framebuffer step.
- 2026-10-03: The live userland's missing kernel calls. `sethostname` (uname's node name is
  now writable), `vhangup` (flush input, SIGHUP+SIGCONT to the foreground group), `reboot`
  (magic numbers checked; sync, then the 8042's reset line or a triple fault; power off on
  QEMU by writing SLP_EN to the PIIX4/ICH9 PM port, found through PCI and only when the
  subsystem vendor is QEMU's — a real board needs ACPI's `_S5`; CAD_ON/OFF switch
  Ctrl-Alt-Del between "restart now" and SIGINT to init, which runit asks for).
  `mount -t tmpfs` mounts a fresh RAM directory (`vfs_mount_ram`); proc/sysfs say ENODEV,
  ext2/vfat ENOTBLK (no block device files yet); `umount2` only reports. Keyboard driver
  tracks Alt now. New musl test `boottest` (22 checks) in `make test`; 131/131 pass.
  By hand in QEMU: `hostname livebox` works, `poweroff -f` switches QEMU off, `reboot -f`
  resets it. No hot path changed (two table entries, one key check in the keyboard IRQ),
  so no benchmark. Lesson: Linux allows stacking mounts on one directory; so do we.

### 2026-10-03 — hostile disk images, and the copies that sleep

Two jobs, and each turned up bugs the other could not have.

**A fuzz target for hostile disk images.** `storage_init` mounts anything labelled
`myos-root` or `myos-data` automatically, and read-write, so plugging in a USB stick is
the whole attack — no port to close, no user to deny. `make diskattacks` builds the real
`fs/ext2.c`, `fs/bcache.c`, `fs/pcache.c`, `fs/partition.c` and the VFS on the host
against stubs, under AddressSanitizer and UBSan, and mounts a byte array: 30 cases that
each corrupt one field, then walk, read, write, rename and symlink. `make diskfuzz`
mutates bytes at random on top of that, biased at the superblock, the group descriptors,
the inode table and the directory blocks, forking per run so a crash is evidence rather
than the end.

Most of the work was not writing cases, it was making the harness able to *see* one.
**Six of the first eight measured nothing**, all for the same reason: `bcache` gives
every buffer a whole page, so with 1 KiB blocks a four-byte overrun lands in 3 KiB of
slack and nobody notices. The base image is now built with `mke2fs -b 4096` so a block
exactly fills its allocation and the next byte is a poisoned redzone. `kmalloc` maps to
plain `malloc` (a size class would round 100 bytes to 128 and hide a 20-byte overflow),
and fresh allocations are filled with `0xA5` rather than zeroed, which is the only way to
catch the bug no sanitizer can see: memory validly allocated, never written, handed to
user space. That is how the fast-symlink disclosure shows up.

Then the fuzzer found three bugs in minutes that two careful readings had missed. Two
were alignment: `rec_len` was not required to be a multiple of four, so the walk read a
`uint16_t` at an odd address, and the `..` fixup — whose *bounds* I had just fixed — did
a misaligned four-byte **write**. Harmless on x86, which is exactly why reading missed
them, and not harmless on the Raspberry Pi that is on the roadmap. The third is the one
worth generalising from: `fs->groups` divided by `blocks_per_group` three lines **above**
the check for `blocks_per_group == 0`. The check existed and was correct. The rule it
broke is not "validate your inputs" but **nothing may be derived from a field before that
field is checked**, which is an ordering property — the thing reading code is worst at and
running it is best at. Eleven ext2 bugs now, each with a case, and
`make verify-disktests` reintroduces all eleven one at a time and requires that the
matching case, and not a different one, fails.

**The copies that sleep.** `copy_to_user` can sleep, because a fault on an `mmap`'d file
page waits for the disk — so "a syscall is atomic because interrupts are off", written
out in `net/netdevice.c` and assumed all over, is false. A second pass that worked from
*shared mutable state* rather than from the 126 copy sites found three more bugs beyond
the four already fixed. The worst is `sys_getdents64`: it held raw `struct inode *`
pointers across the copy with no reference, and `detach()` frees an unlinked inode the
moment `refs` hits 0 — so a sibling's `unlink` during the sleep left us reading a freed
node's `name` pointer and length and `memcpy`ing that to user space. Also `tty_read`
(every reader shares one ring; the underflow makes `--nlines` turn a `memmove` into a
four-billion-byte one) and `wait4` (two threads reap one zombie, the slot is reused by
`fork`, and the stale reap then drops a *live* process's `cwd` reference).

The fix is `fault_in_user()`, which touches every page of a user range before any shared
state is read, applied centrally in `rw()` — the one door `read`/`write`/`readv`/`writev`
now go through — plus `getdents64` and the socket calls. **+34 cycles on a 4 KiB read**
(1506 → 1540, measured twice each way). But `clone(CLONE_THREAD)` landed in this tree
today, so a sibling thread can unmap the buffer while the pre-fault is still probing
later pages: it removes the sleep that happens with *no attacker present*, and nothing
more. So each site is also correct across a sleep by construction — commit the index
first, detach first, reap first, or hold a reference.

Three near-misses worth keeping. I wrote a comment claiming a race that could not happen,
then rewrote it to deny one that could, before getting it right — the file now records
the wrong argument *as* wrong, because it is a tempting one. Piping `make verify-disktests`
into `head` killed it with SIGPIPE before its restore trap ran, leaving a deliberately
reintroduced bug sitting in the tree for half an hour; both verify scripts ignore SIGPIPE
now. And a patch that no longer matched the source made the same script rebuild the
*unmodified* file, watch every case pass, and report "NOT CAUGHT" — a script written to
catch tests that measure nothing, measuring nothing. It says "PATCH DID NOT APPLY" now,
and it deletes the binary on the way out, because the one it leaves behind is built from
the last reintroduced bug and I spent twenty minutes investigating a "new" bug that was
exactly that.

Lesson, twice over: a test that passes against the broken code measures nothing, and the
machinery that checks for *that* needs the same suspicion pointed at it.
- 2026-10-03: Attack-path tests for the sleeping copy. The previous entry ended with seven
  fixes whose whole justification was an interleaving nobody had ever made happen, which is
  not evidence. `make test` now makes it happen. `include/uaccess_test.h` + syscall 423 let a
  thread arm the nth `copy_to_user`/`copy_from_user` it performs to `thread_yield()` before
  copying; a yield is what a sleep looks like from the sibling's side, and is deterministic,
  and `wq_sleep` leaves the kernel in exactly that state anyway (inside a syscall, interrupts
  off), so it is faithful rather than merely convenient. Both halves are `#ifdef SELFTEST`:
  no branch and no symbol in a normal build. `user/musl/racetest.c` races a sibling against a
  sleeping copy in `read`, `write`, `wait4` and `getdents64`, asserting conservation laws
  (eight bytes in a pipe are read exactly once in total; one child is reaped by one waiter;
  every name `readdir` returns is really there) rather than implementation details.
  `make verify-racetests` puts each of the six fixes back and checks racetest notices.

  The first run found an eighth bug, in my own fix. `sys_getdents64` pinned the entry it was
  copying — the pointer the code obviously holds — but `next` is fetched before the loop and
  the copies that emit "." and ".." sleep before it is ever used, so a sibling frees it and
  the first real iteration reads a name pointer and length out of a recycled inode and copies
  that name to user space. The test printed `unexpected entry "\xe9b"`. Both pointers are held
  now, with one exit path. Two careful readings of that function produced a fix that was still
  memory-unsafe; the first run of the test caught it.

  Then verify-racetests caught the test itself measuring nothing, five times, each for a
  different reason, and each of them a thing I would not have thought to check: the buffer was
  too small, so the call returned after "." and ".." and never touched an entry; the single
  arming always landed on the same copy, so two of three bugs were out of reach (it sweeps
  1..10 now); a directory lists in reverse creation order, so the entry I kept alive was the
  *first* one reported and the pointer held across the sleep stayed valid; freed memory still
  held the old name, so a walk reading a dangling pointer read "aaa" back and saw nothing
  wrong (the sibling grooms the heap now — creating files with names of its own choosing after
  deleting, which is what an exploit does, and the test was not an attack path until it did);
  and the deletion order mattered, because `sibling_remove` leaves the removed node's own
  `next_sibling` alone, so deleting in creation order leaves every detached node pointing at a
  *live* entry and the bug gets away with it. Deleting newest-first makes them dangle.

  Lesson: writing a test for a race is mostly discovering the reasons it cannot fire. Five of
  the six reasons here were invisible from the test's source and obvious the moment something
  reintroduced the bug and the test stayed green. An attacker chooses buffer sizes, timing,
  heap contents and deletion order; a test that fixes all four picks the easy combination
  roughly always.
- 2026-10-03: The live disk boots to a login and logs in. `make live-hd` builds
  `build/myos-live.img` (boot area + 1.5 GiB ext2 root from `rootfs.tar`; the tar is
  unpacked with Python onto a case-sensitive disk image because Homebrew's mke2fs
  can't read tars, then debugfs sets the owners), `make utm-live` the UTM VM. Each
  step of the boot stopped at the next missing piece, in order: musl's "RELRO
  protection failed" was really 32 memory areas running out (now a growable array,
  binary search, up to 1024; bench unchanged); runsv needed `flock`, then named pipes;
  `su` needed real users (uid/euid/suid/fsuid, groups, setuid exec, `AT_SECURE`) and
  then `getpriority`/`setpriority` for PAM's limits; `useradd` needs hard links, so
  the user `myos` is written into the tar at build time. `login` exited silently:
  musl's `ttyname` reads `/proc/self/fd/0`, so `mount -t proc` now gives
  `/proc/self/fd/N` — links whose target is the caller's own open file, looked up in
  O(1) (`fs/procfs.c`). Then login crashed writing past the top of its stack:
  util-linux rewrites its process title over argv[0]..end of the environment,
  assuming Linux's layout (strings ascending from argv[0]); ours were pushed downward,
  so the "length" was negative. Now laid out as Linux does, with a boottest check.
  Lesson: "runs unmodified Linux programs" includes layouts nobody documents — the
  order of strings on the start-up stack is ABI because sendmail-era code depends on it.

## 2026-10-03 — DHCP, and two bugs in `execve`

The address stopped being a `#define`. `net/ipv4.c` holds four words — address,
netmask, gateway, nameserver — and `net/dhcp.c` fills them in at boot:
DISCOVER, OFFER, REQUEST, ACK, with a CSPRNG transaction id, 1/2/4/8-second
backoff, and the old static 10.0.2.15 as a fallback. This was a prerequisite,
not a feature: UTM's `Shared` mode is Apple's vmnet, which hands out addresses
by DHCP and has none to hard-code, so until now a VM built for real hardware
had no working network.

Two things were awkward. A machine with no address has to send from 0.0.0.0 to
255.255.255.255, and has to *receive* a reply addressed to 255.255.255.255 —
both of which every other path in the stack refuses, on purpose, because
broadcast is how a machine becomes an amplifier. Each is now one exception,
gated as narrowly as it can be: the send is one function with one caller; the
receive requires an open transaction (a few seconds at boot, never again), UDP,
and a bound port, of which there is one. Details in `docs/security.md`.

The `make test` grep is worth a note. It checks the `ipv4: eth0 is 10.0.2.15/24`
line *and* a `dhcp: lease from 10.0.2.2` line, because under QEMU the fallback
produces exactly the same four addresses as a real lease — so the address alone
cannot distinguish a working client from one that never got a reply. Confirmed
by negating the xid check and watching the log say `dhcp: no answer` with an
identical `ipv4:` line. A test that cannot tell the two apart is not a test.

Separately, a review of the ELF and signal paths turned up two bugs in
`copy_strings`, which copies `argv` and `envp` into the kernel. An `execve`
with 1024 arguments and two environment variables ran the envp loop with no
bound at all and ended up storing user strings through a corrupted kernel
pointer. And a NULL `argv` — legal, meaning "empty" — was not terminated, so
the kernel walked uninitialised heap as a vector of `char *` and copied it onto
the new program's stack.

The first was easy to test. The second taught the more useful lesson: three
tests for it passed with the bug in place. `/bin/true` ignores `argv`, so
nothing showed; `/bin/args` exits with its own `argc`, and still nothing
showed, because the recycled heap block read back as zeroes and the walk
stopped at the first word with the right answer. **Zero-filled memory hides
every uninitialised-memory bug** — the wrong code produces the right result.
So `make test` builds now fill every `kmalloc` with `0xA5` (`mm/kheap.c`,
`#ifdef SELFTEST`), a value that is neither a NUL nor a mapped address. The bug
is caught immediately, and turning poisoning on broke nothing else, which is
evidence no kernel code was relying on `kmalloc` returning zeroes.

Lesson, for the third time this week: a fix needs a test that fails against the
unfixed code, and the reason a test silently measures nothing is almost always
that the broken path still produces a valid-looking answer. The same week's
other two: the fuzz harness over-allocating its packet buffer so a four-byte
overread stayed inside the allocation, and a readdir race test whose deleted
entries still pointed at live memory.
