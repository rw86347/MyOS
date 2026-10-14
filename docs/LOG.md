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

## 2026-10-04 — `ping google.com`, on a real network

The goal was a UTM machine on the Desktop that can ping google.com, and it
does. What stood in the way was not MyOS.

QEMU's SLIRP backend, which every `make run` and `make test` uses, does not
forward ICMP to anywhere except the gateway: doing so needs a raw socket and
therefore root. A packet capture settled it — MyOS emits three correct echo
requests and nothing comes back, in a boot where `wget` works. So `ping
8.8.8.8` is not testable on the command line at all. UTM's `Shared` mode is
Apple's vmnet, a real NAT in the macOS kernel, which does forward ICMP; QEMU
can use it directly but not without root (`cannot create vmnet interface:
possibly not enough privileges`). UTM is signed for it. That is the only
reason to leave the command line, and `docs/utm.md` now says so, along with
everything it took to drive UTM from a script — chiefly that its QEMU runs in
the App Sandbox, which refuses to bind a unix socket or write a file outside
the VM bundle, so the monitor has to go over localhost TCP.

Verified in a probe VM: a lease from 192.168.64.1, `/etc/resolv.conf` holding
that nameserver, and `ping google.com` 3 of 3 at 260–311 ms.

Two things had to be fixed to get there, and the second was the interesting
one. `/etc/resolv.conf` was a file in the image naming QEMU's 10.0.2.3 — right
in QEMU and wrong everywhere else, which is precisely the machine DHCP was
added for. The kernel is the DHCP client, so it is the only thing that knows;
it now reports the nameserver as `/proc/net/dns`, already in resolv.conf's
format, and both inits copy it.

Testing that copy turned up three things nobody had looked at. runit's stage 1
mounted /proc, /sys and /run without creating the mount points, with errors
sent to /dev/null — all three had been failing silently. `initramfs/init` never
mounted /proc at all, so musl's `ttyname()` had been working by luck. And the
check had to compare `/proc/net/dns` against `/etc/resolv.conf` byte for byte
with comments in the shipped file, because under QEMU the kernel's answer is
the same address the image guessed: comparing addresses would pass whether or
not the copy happened. The same trap as the DHCP lease line, one day apart.

Also fixed: `make test-shell` had three checks that failed at random. The
commands are typed on a timer, so a slow one — ping waits a second between
packets — is still printing when the next is typed into it, and `^wget ok`
stops matching. Those three are now unanchored, which is still exact because
each pattern contains something the echoed command line cannot: a received
count, a resolved address, a byte count. Worth saying plainly: a test that
fails at random is worse than no test, because it trains you to ignore it.

## 2026-10-04 — reviewing the fd-sharing work

Read through the new terminal, input and epoll code and found one bug class running
underneath all of it: `fd_get` returns the `struct file` without taking a reference, and
threads share the fd table, so a sibling's `close()` frees it while another thread is
asleep inside a blocking read. Fixed in `rw()` — which covers every `read` and `write`, and
with it the new pty, evdev and unix-socket readers — and separately in the two places in
`fs/epoll.c` that are not reached through `rw()`. `docs/security.md` has the detail,
including the parts that are still open: sockets have no reference count at all.

Two things worth remembering from doing it. First, the test for the `read`/`write` bug
passed against the bug twice before it measured anything, once because it closed a
different fd than the one the readers were asleep in, and once because `close()` hands back
the fd number *and* the heap block, so the fixed and broken kernels agree. `dup2` separates
them. Second, the test suite had been failing about one run in ten with a kernel page fault
in `memcpy`, and it was not in the kernel at all: `kernel/user_test.c` read the program's
name through `p->threads` after `proc_spawn` had already made the program runnable, so a
program that exited immediately had its thread freed first and the read came from address
zero. The name comes from `path` now, which cannot race. A stack scan in the fault handler
found it in one run after an afternoon of guessing at register values — worth building a
real backtrace next time rather than reading `nm` output by hand.

Followed up by extending the same fix to everything else that holds an object from an fd
across a sleep: the thirteen socket calls (`SOCK_SYSCALL` in `net/socket.c`, one wrapper
rather than a `file_put` on each of fifty return paths), and the terminal ioctls, which had
no permission checks at all — `KDSKBMODE K_OFF` let any process turn the console keyboard
off, and turning it back on means typing. `epoll` was identifying a registered file by its
address, which a closed-and-reopened fd can reuse, so `struct file` now carries a per-open
serial number. Two percent on `read`, 1.6% on `open`, numbers in `docs/performance.md`.

The socket part has no test, and `verify-racetests.sh` is why: one was written and it passed
against the unfixed kernel, so it went in the bin rather than the tree. The fix keeps
Linux's semantics — a call whose fd is taken away still finishes against the socket it
started with — so there is no wrong answer to assert on, only corruption of whoever took the
freed block.

## 2026-10-04 — the graphical login, and two ways to draw nothing

The live disk boots to a login screen now: `live/myos-login`, about 650 lines of C over
Xlib, Xft and PAM, which starts the X server, draws the Golden Hour login from
`assets/themes/golden-hour`, checks the password the way `/bin/login` does and runs the
session. Underneath it went everything X needs that we didn't have: evdev input, AF_UNIX
sockets, epoll, pseudo-terminals, and one file in `/sys`. Writing our own greeter instead
of configuring xdm was the right call for a second reason: xdm had already worked (the
stock login box appeared and logged in), so the hard parts were proven before any of the
drawing existed, and the drawing is ours to match the mockup with.

Then the thing worth writing down. The first boot of the finished login screen showed the
text and nothing else — no photograph, no bands, no tiles — on black. IceWM, booted the
same way, ignored every picture in its theme and fell back to its own. Two programs, same
symptom, one cause: the X server announces MIT-SHM, MyOS has no System V shared memory at
all, and Imlib2 — which both programs use — takes the shared-memory path on sight and
quietly draws nothing. No error, no log line. Turning the extension off in `xorg.conf.d`
fixed both at once. The lesson is about *advertising*: a kernel that doesn't implement
something must not let a server tell clients that it does. We got away with the X server
itself failing loudly on missing syscalls; this failed silently because the client-side
library decided the feature was there and then handled its own failure by doing nothing.

The second false trail cost longer than it should have. IceWM's theme was written with
`Look="flat"`, and flat is precisely the look that draws rectangles from the colour list
and never opens a picture. The colours *were* ours, so the desktop looked plausibly themed
while every image was being ignored — a half-right result is much slower to diagnose than a
wrong one. The way out was comparing a crop of the screen against the PNG we had generated,
pixel for pixel, instead of judging by eye: the stock Start button and ours are both green
XP-style buttons and I had been calling ours "stock" for an hour.

Smaller things learned: IceWM asks for every theme picture by its `.xpm` name and then
tries `.png`, so `Gradients=` must list the `.xpm` names whatever the files are called;
a picture it repeats must have no border of its own, or every repeat shows as a seam; and
it reduces alpha to a one-bit mask, so a round button needs its background baked in. Xft
sizes are points and the theme's are pixels — `pixelsize=` is the only honest way to ask
for "the size it was drawn". And musl's strftime has no `%l`.

## 2026-10-04 — e1000 transmit no longer wedges on a quiet link

The card raises its interrupt on receive and link change, never on
transmit-done, and the only code that reclaimed finished transmit slots lived
in the NAPI poll that an interrupt schedules. So on a link with no inbound
traffic — a point-to-point cable, or the kernel's own first ARP before any
reply exists — nothing ever reclaimed a slot, and the 16th transmit found the
ring full and returned NETDEV_TX_BUSY for good. `tools/` fuzzing and the audit
both flagged it; it was the last open item in docs/gaps.md's Drivers section.

Fixed by reaping finished slots at the top of `e1000_xmit` (a bounded walk of
at most the 15-slot ring), so the transmit path is self-sufficient. I did not
enable the transmit-done interrupt: it would add an interrupt per batch on the
slow P4/Pentium M we tune for, and the completed slots are not needed until
the next transmit anyway, which is exactly when the reap runs. The reap in the
poll stays, so slots are also freed promptly whenever the stack is already
polling for receives.

The test sends twice a full ring on a quiet link with interrupts off, which is
the one arrangement where the poll cannot run and the transmit path is the
only thing that can reclaim a slot. With the fix removed it fails "wedged after
15 transmits" — the ring size minus one, exactly as predicted.

## 2026-10-04 — the Start menu, and IceWM's two hidden rules

The mockup's Start menu has two columns under a header with your name, and Log off /
Turn off along the bottom. IceWM's is one plain list and can't be told to run anything
else when Start is clicked. So the menu is now our own program, `live/myos-menu`
(Xlib + Xft, Imlib2 only to decode PNGs), and IceWM keeps the taskbar, the window
buttons and the clock.

Everything you see is drawn at build time by `live/theme/make-live-theme.py`, with the
theme's own drawing functions and fonts: the whole menu, each entry as it looks under
the mouse (the menu drawn again with that entry lit, cut down to its rectangle), the
Start button in three states, and the Run box. `menu.conf` beside them says where each
entry is and what it runs. The program only adds the user's name, swaps the lit pieces
in, and draws what you type in Run.... Opening the menu reads the screen under it once
(so the round corners and shadow blend into whatever is there) and sends it back once;
moving over entries copies small pixmaps made at start-up. Only working entries are
listed: Terminal, AbiWord, Run..., Log off (`icesh logout`), Turn off
(`doas /sbin/poweroff`).

How it hooks in is the interesting part. The first try was an invisible window kept on
top of IceWM's Start button. It worked, but the button underneath still looked wrong,
and that turned out to be the "break in the gradient" Rodger had spotted. IceWM 3.5's
`YButton::paint` draws a Windows-style bevel around every taskbar button in pixmap look,
with no setting to turn it off, and next to Start sat an empty quick-launch pane (IceWM's
default list names an xterm and a browser whose icons we don't have). Now IceWM's
Start picture is just a plain piece of bar, and `myos-menu` puts a window of its own
*inside* IceWM's button (found by its name, "TaskBarMenu"), the way a tray icon sits in
a panel. As IceWM's child it moves, stacks and hides with the taskbar for free, its
clicks come to us, and there is no fight over who is on top. A button is its picture
plus 5 px each way, so a 151x35 placeholder makes the button exactly 156x40, and the
bar 40 px instead of 46. An empty `/etc/icewm/toolbar` removes the empty pane. IceWM
still showed its "Favorite Applications" tooltip; sending its button a LeaveNotify when
the pointer enters ours cancels just that one. Ctrl+Esc and the Windows key are
grabbed on the root window, with IceWM's `KeySysMenu` and `Win95Keys` switched off.

The second hidden rule: the black patch under every window's title buttons. In pixmap
look a title-button picture holds two buttons stacked, resting on top and pressed below,
and IceWM shows half the picture's height. Ours held one, so the lower half of each
button was never painted. They are now 30x60 with the theme's pressed look underneath.

Checked in QEMU: Ctrl+Esc and a mouse click both open the menu, Start shows held down,
arrow keys light the entries, Run... starts `xterm`, Log off returns to the login
screen, Turn off powers the machine down. One bug on the way: `sscanf(line, "run %s")`
also matches `runbox ...` (a space in a scanf format matches zero spaces), so the
config reader now compares the first word whole.

Later the same day: the window icon at the left of each title bar. The theme had no
left title button; IceWM draws the window's icon in its window-menu button ("s"), so
`TitleButtonsLeft="s"` with a plain title-bar background behind it. The icons are the
theme's own, copied into the IceWM theme as `name_18x18.png` and so on (IceWM searches
the theme before its stock icons), `/etc/icewm/winoptions` maps xterm and AbiWord to
them, and the fallback ("icewm") is the MyOS logo. Two more IceWM details: the left
buttons start at the width of the **J** piece (the left end), not L; and the icon
button is as wide as the bar is high whatever its picture is, so a narrower picture
leaves a black stripe. With J, L, S and P 1 px wide the icon sits 9 px in and the title
about 37 (the mockup: 8 and 32).

## 2026-10-04 — Settings, and one scale for the whole desktop

At 1920x1200 a desktop drawn for 96 dpi is a third too small to read. Now the session
has a scale, like the Mac's Displays pane: 100% to 200%, automatic by default (150% at
1920x1200, 100% on the 1024x768 to 1440x900 screens of an old PC).

- `live/myos-settings/myos-settings.c`: Settings, laid out like the Mac's System
  Settings (search, the user's card, grouped panes with coloured icons, white boxes of
  rows, back/forward). Network, General (About, Storage, Date & Time), Displays,
  Users & Groups. GTK3, already on the disk for AbiWord. Its icons are drawn with cairo,
  so they stay sharp at any scale.
- Displays shows five pictures of the desktop, from "Larger Text" to "More Space",
  with "looks like 1280 x 800" for each. Sizes that leave less than 800x600 aren't
  offered. The choice goes to `~/.config/myos/display.conf` and the root window.
- `live/myos-settings/myos-xsettings.c`: a 200-line XSETTINGS manager (Alpine has no
  xsettingsd for x86). Started by `/etc/myos/xsession` before IceWM; it publishes the
  font DPI (and a 2x window scale at 200%) to GTK, sets `Xft.dpi` for xterm and IceWM,
  and on a change restarts IceWM. GTK windows that are open change size at once.
- xterm gets DejaVu Sans Mono 11 pt (`/etc/myos/Xresources`): its default bitmap font
  can't grow.
- GTK's headers come from a new `[build-only]` group in `live/packages.txt`:
  unpacked into the sysroot, never put on the disk, and without their dependencies
  (`gtk+3.0-dev` with them is 261 MiB, LLVM included). The disk stays at 468.9 MiB.
- Tested in QEMU at 1920x1200: login, 150% by default, a switch to 200% (Settings
  and IceWM's text follow), About, Users. Settings opens with Ctrl+Alt+S for now.
- Found on the way: AbiWord dies in musl's `free` (a group header pointing at
  0x80000000) with or without the scale; the X server died the same way once.
  In `docs/gaps.md`, not fixed.

## 2026-10-04 — POSIX timers, so Vim's timeouts work

Vim in xterm printed `E1286: Could not set timeout: Function not implemented`. Its
'redrawtime' and regexp timeouts call `timer_create(CLOCK_MONOTONIC)` with
SIGEV_THREAD, and the kernel had only `alarm`/`setitimer`.

SIGEV_THREAD is done by musl, not the kernel, and needed more than the timer calls.
musl starts a helper thread with the signal blocked and creates a kernel timer that
signals that thread alone (SIGEV_THREAD_ID). The helper waits in `sigwaitinfo`, and it
calls Vim's function only when the signal's `si_code` is SI_TIMER. So the kernel had to
provide three things:

- `timer_create`, `timer_settime`, `timer_gettime`, `timer_getoverrun` and
  `timer_delete` (259-263, plus the time64 calls 408/409), in the new
  `kernel/posix_timer.c`. There is one fixed table, and a timer's id is its slot.
  Armed timers sit on one sorted list, and the tick only looks at its head, as for
  `alarm`. They support relative and TIMER_ABSTIME times, intervals that don't drift,
  overrun counting, and timers longer than the 32-bit ms clock (taken in steps).
- `rt_sigtimedwait` (177/421). Before this, musl's helper thread would have spun
  forever on ENOSYS.
- A real siginfo for timer signals: SI_TIMER, the timer id, the overrun count and
  si_value. These reach both SA_SIGINFO handlers and `sigwaitinfo`. Delivery tests
  one bit (`p->timer_sigs`) first, so other signals pay a single load.

Found while testing: a periodic timer deleted with its signal still pending left the
signal behind, and unblocking it then ended the process, because the default action
for a real-time signal is to terminate. `timer_delete` now takes its pending signal
back, as current Linux does.

`pthreadtest` gained 9 checks, starting with Vim's exact call. Three deliberate breaks
are each caught: no `timer_create` (Vim's bug), no SI_TIMER in the siginfo (the timer
fires but Vim's function never runs), and a delete that leaves the signal pending.
`make test` passed 6 of 6 runs. `make bench` shows no change on the shared paths
(`docs/performance.md`).

## 2026-10-04 — WritePerfect on the live disk

Rodger's WordPerfect 5.1 work-alike (`../wp51/wp51c`) is now on the live disk, in the
Start menu with its own icon. `make live-root` builds it from its own tree, together
with its grammar checker (`languagetool-native`, about 5,000 lines of C++) and the
checker's English rules (19 MiB, in `/usr/share/writeperfect`). Only ICU was new on
the disk (`icu-libs`, English data, +7 MiB); GTK, ncurses, libxml2, pcre2 and
libstdc++ were there already.

The lesson was about C++ and the ABI. `i686-elf-g++` compiled the C++ but could never
have linked it: the bare-metal target makes `size_t` an `unsigned long`, Linux i386
makes it an `unsigned int`, and C++ puts argument types into symbol names. So
`operator new` is `_Znwm` in our objects and `_Znwj` in Alpine's libstdc++. C doesn't
put types in names, which is why the C programs never noticed. The WP objects are
compiled by clang with `--target=i386-alpine-linux-musl` (exactly Linux's types)
and linked by our GNU ld, using Alpine's own crtbegin/crtend and libgcc.

WP is a terminal program at heart, like the DOS original, so the menu opens it in an
80x25 xterm in WP 5.1's blue (`/usr/bin/writeperfect`). Tested in QEMU: it starts from
the menu, typing works, and the icon shows in the title bar and taskbar.

Also: the hot corner's curl, top right. `make-live-theme.py` draws it with the design's
`curl()` at both sizes; `myos-menu` shows it in a 44x44 window of type
`_NET_WM_WINDOW_TYPE_DESKTOP`, which IceWM keeps below every window, so it is above
the wallpaper and under windows as `docs/ui-theme.md` says. The wallpaper behind it
is read from the pixmap icewmbg publishes (`_XROOTPMAP_ID`), not from the screen, so a
window passing over the corner never gets baked in, and it is redrawn when that
property changes. Checked in QEMU: 24 px at rest, 40 px with the mouse in the corner,
hidden by a maximized window. The window overview it should open is still to come.

## 2026-10-04 — a PDF viewer, and pread64/pwrite64

Evince (GNOME's Document Viewer) is on the live disk and in the Start menu as
"Document Viewer", with its own icon. It is the closest Alpine has to Apple's Preview:
page thumbnails, search, zoom, annotations. It adds about 40 MiB.

The first PDF it opened showed "PDF document is damaged", although the file had
arrived intact (same MD5 as on the Mac). The serial log gave the reason:
`syscall 180 not implemented (evince)`. That is `pread64`, a read at an offset that
leaves the file's position alone, and poppler reads every PDF that way. So every read
failed, and poppler took the empty result for a broken file.

`pread64` and `pwrite64` now go through the same door as read and write (`rw_at` in
`kernel/syscall.c`). The file systems all read at `f->pos`, so pread hands them a
copy of the open file on the stack, positioned at the offset. The real position
doesn't move, even if another thread uses the same open file at the time. Pipes,
terminals and sockets give ESPIPE and directories EISDIR, as on Linux. `boottest`
gained 5 checks. `make test` passes, and `make bench` shows read unchanged
(`docs/performance.md`). Evince then showed the Declaration of Independence, page 1 of 4.

## 2026-10-04 — the desktop's pictures follow the scale

The taskbar, title bars and their buttons looked small next to the mockup. In pixels
they were the mockup's (40, 30 and 21), but the mockup is a 1024x768 screen and the VM
is 1920x1200, so they filled two thirds of the share of the screen they should. The
desktop scale from Settings (automatic: 150% here) already made the fonts bigger
through `Xft.dpi`, but IceWM's theme is pictures of a fixed size, and so is the Start
menu.

`make-live-theme.py` now draws both at 100, 125, 150, 175 and 200%. The drawings keep
the mockup's numbers; `render()` makes the PNG K times as many pixels, and the theme
file's sizes (title bar, borders, icon sizes) and `menu.conf`'s rectangles are scaled
the same way. Two details: the title buttons hold two states stacked, so they are
made exactly twice the bar's height in whole pixels (at 125% the bar is 37.5 → 38 px);
and each menu highlight is cut at a whole pixel of the scaled menu, so it lands on the
picture underneath. Each scale is its own IceWM theme (`GoldenHour`,
`GoldenHour-150`...). IceWM reads `~/.icewm/theme` after `/etc/icewm/preferences`, so
`myos-xsettings` writes the matching name there before IceWM starts and before the
restart it already does when the scale changes. `myos-menu` reads the same root
properties (`_MYOS_SCALE`, `_MYOS_SCALE_AUTO`), loads `/usr/share/myos/menu/<scale>/`
and starts itself over when the scale changes. Cost: 2.3 MiB of PNGs on disk.

Checked in QEMU at 1920x1200: at 150% the taskbar is 60 px (5% of the height; the
mockup's is 5.2%), title bars 45 px, the Start button 234x60, and the Start menu grows
with them. With `scale=100` in `~/.config/myos/display.conf`, logging in again gives
the old sizes.

## 2026-10-04 — System V shared memory

`shmget`, `shmat`, `shmdt` and `shmctl` work now (`kernel/shm.c`), for the X server's
MIT-SHM. A program puts a picture in a segment the server has attached too, and only
says "draw from there". musl on i386 makes all four calls through the old `ipc()`
multiplexer (117), so that's the one syscall added. The direct calls Linux gained in
5.1 (395-398) have no caller here.

Almost nothing in it is new machinery. A segment is an inode that's in no directory,
its pages held like a RAM file's and made on first touch. An attach is a memory area
with a new flag, `VM_SHARED`, whose fault maps the segment's own page with
`PTE_SHARED`, the bit device memory already used. Fork then shares the page instead of
making it copy-on-write, and unmapping doesn't free it. Because the area holds the
inode the way a file mapping does, the inode's reference count *is* the attach count,
and fork, exit and exec keep it right without any code in `shm.c`. IPC_RMID on a
segment still in use marks the inode deleted, and the last detach frees it through the
same path as a deleted file whose last user lets go. Ids name their slot, so
everything except `shmget` by key is O(1).

Two details were fixed on the way. A test child that only calls
`setuid(1000)` keeps group 0, and 0640 lets group 0 read, so `IPC_STAT` was rightly
allowed: Linux's rule, `ipcperms`, checks the groups too. And `shmdt` has to leave
alone a second attach of the same segment placed right after the first; Linux tells
them apart by offset, and so does `mm_shm_detach` now.

`user/musl/shmtest.c` has 41 checks: attaches in two processes, fork, read-only
attaches, `mprotect` splits, SHM_RND and SHM_REMAP, keys, permissions as another user,
IPC_RMID while attached, and the RAM coming back at the last detach. It takes 36 ms.
`make test` passes. The page-fault path has one more flag test; `make bench` has no
page-fault benchmark, so that cost isn't measured. MIT-SHM is still switched off in
the live disk's X config, until a check on the live disk shows Imlib2 drawing
through it (`docs/gaps.md`).

## 2026-10-04 — a bigger mouse pointer, and WritePerfect's own window

The pointer was X's 16-pixel arrow at every size: X only uses a pointer theme when
told to, and nothing told it. Adwaita's pointers were already on the disk (GTK brings
them), drawn at 24, 32, 48, 64 and 96 pixels. `/usr/share/icons/default/index.theme`
now makes Adwaita the theme for every X program, and Settings has a **Mouse &
Keyboard** page: the pointer in four sizes (Normal, Large, Larger, Largest = 1, 1.5, 2
and 3 times the normal 24 px, which itself grows with the desktop scale), and the
keyboard shortcuts. It works like the scale: Settings writes `~/.config/myos/mouse.conf`
and the root property `_MYOS_POINTER`; `myos-xsettings` turns that into
`Gtk/CursorThemeSize` (XSETTINGS, GTK follows at once) and `Xcursor.size` (xrdb, for
everything else) and restarts IceWM so the desktop and window frames follow. Checked in
QEMU: Adwaita's arrow at 32 px at 150%; clicking Largest makes it big over Settings
and the desktop straight away.

The Start menu's WritePerfect now opens its GTK window (`wp --gui`) instead of the
terminal editor in an xterm. WritePerfect called `gtk_init_check(NULL, NULL)`, so its
window had no WM_CLASS name and IceWM showed its red-X fallback icon;
`g_set_prgname("writeperfect")` in `linux/gui_gtk.c` (the WritePerfect tree) names it.
`/etc/icewm/winoptions` now writes every entry as `instance.Class`, the form that
matches, and gives Settings its icon too.

## 2026-10-04 — My Files, the mockup's file explorer

`live/myos-files/myos-files.c` is the "My Files" window from
`assets/themes/golden-hour/mockup.png`, made to match it: menu bar, toolbar with
Back/Forward/Up and the address box, the sand task pane with its two cards (File
tasks, Other places), large icons in 120x100 cells with the teal selection, and the
status line ("6 objects, 283 KB"). It is one GTK window with one drawing area; every
pixel is drawn with cairo at the mockup's own coordinates, multiplied by the desktop
scale, and the icons are the theme's SVGs (now installed in `/usr/share/myos/icons`,
drawn by librsvg, so they are sharp at 150% and 200%). Menus, the right-click menu and
the "are you sure?" box are drawn in the same style. Text is DejaVu Sans (the mockup
uses Verdana and Trebuchet, which we can't ship).

What it does: open folders and files (PDFs in Evince, `.wpd` in WritePerfect, `.doc`
in AbiWord, text in Geany), Back/Forward/Up with history, type a path (Ctrl+L), new
folder and rename in place (F2), copy and paste, delete to the Recycle Bin, Disks,
a terminal in the current folder, hidden files (Ctrl+H). It is in the Start menu on
both sides, like the mockup, with Disks under it. Checked in QEMU at 150%: the window
side by side with the mockup has the same layout, icons and colours; new folder,
rename, delete, Disks, Back and the menus work, and files made in a terminal appear
within 2 s.

Testing note: the helper scripts in `/tmp` now take the QEMU monitor socket from
`$MON`, because two chats' VMs both listening on `/tmp/myos-mon.sock` sent one
chat's clicks to the other's VM.

## 2026-10-04 — rounded window corners

The mockup's windows are rounded (8 px) with a 1.5 px dark edge, and a soft
shadow that shows on the left and right of the green bar. IceWM's title pieces
were flat strips, so every window was a sharp rectangle. `make-live-theme.py`
now writes the frame pictures (`frameATL` and the rest, and the same for
dialogs): a quarter-circle in each corner, transparent outside it, and solid
strips for the edges. IceWM clips the window to that mask, which is how the
green bar's top corners come out round, and sets the border to the 1.5 px
edge. A maximized window drops the border, so the cut doesn't show the desktop
in the screen's corners.

The soft shadow is not in those pictures. The mask is one bit, a pixel in the
window or a hole, and a hole does not darken the wallpaper. That blur is the
compositor's (`docs/DESIGN.md`, `docs/gaps.md`).

The bottom corners were still square. The corner pictures do round them, but
IceWM's default is to fill the program's part of the corner back in, so only
the thin border followed the curve and the window's contents stayed a
rectangle. Turning that off (`ShapesProtectClientWindow`) has to be in
`/etc/icewm/preferences`. IceWM does not read it from the theme file, which
is where it was.

## 2026-10-04 — the taskbar tray

The mockup's tray, the last 150 px of the bar, is a different color from the
rest of it (teal, top to bottom) with a dark line on its left, then the
network and volume icons and the time. The live bar was one mixed color, and
IceWM's clock was a flat rectangle only as wide as the text.

IceWM stretches the taskbar picture across the whole bar and paints the clock
itself, so the tray is a picture (`tray.png`, from the same Tray style as the
mockup) and a window `myos-menu` puts at the right of the bar, above IceWM's
clock. The clock stays underneath so task buttons still stop short of the
time; the icons can have a button slide under them when the bar is full. The
time is drawn each minute, with the leading zero taken off, since musl has no
`%l`.

The first cut of that window never appeared: it looked the bar up by the name
"TaskBar", and `XFetchName` doesn't return the name IceWM set with
`XSetWMProperties`. The Start button's name does come back (that's how the
button is found already), so the bar is the first wide strip above that
button, or above the clock. The tray stays raised, because IceWM puts its
clock back on top when it restacks the bar. The picture is one pixel wider
than the 150 px tray, so the mockup's divider — a 1.5 px stroke centered on
the tray's left edge — darkens the bar and the tray, instead of sitting
entirely inside the tray.

## 2026-10-04 — Settings: Wi-Fi, the Mac's groups, and Add User…

Settings' sidebar now has the Mac's groups in the Mac's order: **Wi-Fi** and Network;
General and Displays; Users & Groups; Mouse & Keyboard (where the Mac has Keyboard and
Trackpad). The Wi-Fi pane is where the Mac's is, but MyOS has no Wi-Fi driver, so it
shows Wi-Fi off and greyed out, "No Wi-Fi hardware found", and points at Ethernet.
Network lists Wi-Fi as "Not available".

Users & Groups has an **Add User…** button. As on the Mac, it first asks for an
administrator's name and password (members of wheel), then for the new user's full
name, account name (filled in from the full name), password twice, and whether they
are an administrator. shadow's `useradd` can't run on MyOS (it locks with a hard link,
and there is no `link()`), so `/usr/libexec/myos-adduser` does the work: setuid root,
it checks the administrator with PAM (`/etc/pam.d/myos-admin`, the login screen's
stack), then writes shadow, group and passwd under an flock, each written whole,
synced and renamed into place, passwd last. The home is 0700 with `/etc/skel` copied
in; groups audio, video and input, plus wheel for administrators.

Checked in QEMU on the live disk: the new sidebar, the Wi-Fi pane, and adding "Ann
Lee": `annlee` got uid 1001, its groups, a `$6$` hash and a 0700 home with
`.xsession`; `su annlee` takes the new password and refuses a wrong one. The check
also found that `root` (empty password on the live disk) unlocked with any password:
pam_unix's `nullok` accepts anything for an empty password. The helper now accepts
only an empty password for such accounts (`docs/gaps.md`).

## 2026-10-04 — Firefox on the live disk

The mockup's Web Browser is now Firefox: Alpine 3.20's 32-bit package, the only
real browser that architecture still ships. Chromium is x86_64 only. The extra
packages are 255 MiB, almost all Firefox itself plus ffmpeg's codecs; GTK was
already there for AbiWord.

It is on the Start menu, IceWM's window icon is the theme's compass, and My Files
opens `.html` / `.htm` with it. Two things it needs that we didn't have: `/dev/shm`
as a tmpfs (`shm_open` is a file there, not System V `shmget`), and the content
sandbox turned off (`MOZ_DISABLE_CONTENT_SANDBOX`, `MOZ_DISABLE_GMP_SANDBOX`)
because this kernel has no seccomp. WebRender's GL path is off too (`MOZ_WEBRENDER=0`):
the only display is `/dev/fb0`.

It has not been started on MyOS. Missing `/proc` files or other syscalls may still
stop it; `docs/gaps.md` says where to look (`~/.xsession-errors`).

## 2026-10-04 — `/proc/self/exe`, so Firefox can find itself

Booted the live disk in QEMU and typed `firefox` in an xterm. The files are
there (`/usr/bin/firefox` is the 45-byte wrapper, `/usr/lib/firefox/firefox`
the binary), but it exits at once with "Couldn't find the application
directory." Firefox `readlink`s `/proc/self/exe` to find `omni.ja` next to
itself; we only had `/proc/self/fd`.

`/proc/self/exe` is now a magic link to the ELF `execve` loaded (the
interpreter, for a `#!` script), kept as an inode on the process so a rename
or delete still names it. `open` follows it. `boottest` checks both; `make
test` is OK.

With that, Firefox got further and died two more ways. Alpine's build uses
SSE2, which a real Pentium 4 / Pentium M has and QEMU's plain `pentium3` does
not: `Illegal instruction`. `make run-live` and the UTM machine now add
`+sse2`. Then it printed `fallocate failed to set shm size` and segfaulted.
`fallocate` (grow a file) and `memfd_create` (an anonymous RAM file) are in;
`boottest` checks both. Firefox then tried to reopen the memfd through
`/proc/self/fd/N` for a read-only dup; we treated any inode without a
directory (pipes, and also a memfd) as `ENXIO`. Regular files now go
through; pipes and sockets still don't. Sealing the memfd
(`fcntl F_ADD_SEALS`) was `EINVAL`; that is stored now.

The first UTM live machine still had 256 MiB of RAM, which is enough for IceWM and
not for Firefox. `make utm-live` and `make run-live` now give it 1024 MiB, and
`utm-live` replaces the disk instead of keeping the old one (your files on that
VM were the reason it used to keep it).

Then it ran out of file descriptors (`PROC_FILES` was 32) creating a
read-only memfd, so the table is 64 and `cloexec` is 64 bits. `select()`
still only sees fds 0–31. After that: `mremap` (grow/shrink/MAYMOVE),
`prctl` (thread names and a few flags), `eventfd2`, and a uniprocessor
`membarrier`. `firefox --version` prints 132.0.2. Opening a window still
dies at once: page fault, write of NULL, in `libxul.so`. No text besides
"Segmentation fault".

## 2026-10-04 — My Files: the Mac's four layouts

The toolbar has a View button to the right of Up, the way Finder does: a picture
of the current layout and a pair of chevrons. Its menu is as Icons, as List, as
Columns and as Gallery, with a tick on the one in use (also View in the menu bar,
and Ctrl+1 to Ctrl+4). The choice is kept in `~/.config/myos/files.conf`.

Icons is the mockup. List is Name, Date Modified, Size and Kind (narrow windows
drop columns from the right). Columns is one folder per column with a `>` on
folders, and a preview of a selected file. Gallery is a big preview (the picture
itself for JPEG/PNG/…) over a strip of icons. Checked in QEMU at 150%.

The status bar's bottom-right corner has two short lines at 45 degrees, the
usual mark for a corner you can drag to resize. My Files draws that bar, so
the lines are part of it; they sit clear of the window's round corner.

## 2026-10-04 — My Files: picture icons, and why it felt slow

Pictures now show a thumbnail of the file, not the generic document icon. The
first look decodes the picture in the background (so the window does not wait)
and writes a small PNG next to it named `._file` — the sidecar name the Mac
uses on a disk that isn't HFS. The next visit reads that. The explorer never
lists `._*` files. A read-only folder still shows the thumb this session.

It felt slow for two reasons. The bigger one is QEMU on Apple Silicon: i386 is
emulated (TCG), not virtualised, and the CPU is a Pentium III, so everything is
tens of times slower than a real PC. The smaller one was us: every unknown file
was opened to sniff whether it was text, every redraw rebuilt the column layout,
and every 2 s we stated up to 1000 files. Those three are gone. The window will
still feel slower in QEMU than Finder on the Mac; on the old hardware it should
be closer.

## 2026-10-04 — WritePerfect Settings scroll, and the Mac plugins

The Linux Settings window grew to the height of its content (color wells at
150% scale) and ran off the screen with no scrollbar. It now matches the Mac:
the body is 856 px, the window is at most 80% of the screen, and it scrolls.

HTML reveal plugins are on Linux too. `linux/plugin_host.c` is the Mac's
`plugin_host.m`: it seeds ZHwriter, Thesaurus, Writer and Reveal Codes into
`~/.config/writeperfect/plugins`, lists them in Settings and Alt+R, and loads
the chosen page in a WebKitGTK pane over the reveal strip. The live disk
ships the plugin files and WebKitGTK 4.1. Touch Bar and the shell pane stay
Mac-only.

## 2026-10-04 — UEFI loader: OVMF boots the 32-bit kernel

A UEFI-only PC skipped the BIOS USB image. `make uefi` now builds
`build/myos-uefi.img`: GPT + FAT16 ESP, `/EFI/BOOT/BOOTX64.EFI`. The
loader (64-bit, `arch/i386/boot/efi/`) copies kernel + initramfs, fills
BOOTINFO from GetMemoryMap and GOP, drops to 32-bit protected mode, jumps
to 1 MiB. `make test-uefi` (QEMU + OVMF, serial only) reached
"Hello world from MyOS!" and the bash prompt. Not the x86_64 port.
How to write the stick: `docs/UEFI-USB.md`.

## 2026-10-05 — Live UEFI disk, tested in OVMF first

`make live-uefi` builds `build/myos-live-uefi.img` (ESP + 1536 MiB
myos-root). The kernel now reads a GPT disk (type 0xEE) far enough to
register the Linux partition. `make test-live-uefi` reached
"Hello world" and `ext2: ... mounted at /`.

## 2026-10-04 — My Files: PDF preview in Columns

Selecting a PDF in Columns now shows its first page on the far right,
from the same `._file` sidecar pictures use. Poppler paints page 1 into
that PNG the first time the file is seen; later visits just read it.
The icon in Icons / List / Gallery uses the same cache.

## 2026-10-05 — Real UEFI PC: kernel and framebuffer, not the desktop

An MSI Tiger Lake-H laptop (UEFI only, no CSM) started `BOOTX64.EFI` after
the ESP was FAT32 (64 MiB), the protective MBR grew a 0xEF slice, the PE
kept a `.reloc` directory, and `ImageBase` moved down to 256 MiB. Firmware
that put GOP at `0x400000000` used to leave the kernel blind; PSE-36
(`vmm_map_big` + `fb_phys_hi`) maps that bar, and `simplefb` drew
1920×1080. `make test` on the Pentium path is still 135 passed, 0 failed.

The live ext2 root on the stick is invisible: the only USB controller is
xHCI (`8086:43ed`) and this kernel has no USB mass-storage driver. So
process 1 was the RAM BusyBox `/init`, not runit / `myos-login`. `/init`
then died with SIGSEGV (read of `0x10`); TLS GS is now saved on
`set_thread_area`.

The live UEFI ESP now carries a login-only initramfs (Alpine base +
runit + X + IceWM + `myos-login`, not Firefox/gcc/office). The UEFI
loader sizes that file instead of capping it at 4 MiB. The Pentium
`make test` image is still the small BusyBox ramdisk.

On the MSI, runit reached stage 2 (6746 files) but the login never
replaced the kernel text. GOP is at `0x400000000`; `mmap` of `/dev/fb0`
truncated that to physical 0. A RAM shadow plus a 16 ms blit thread
gives X a 32-bit address it can write. Putting fbcon on that shadow
made the next boot look stuck on "calling ExitBootServices": the
kernel was drawing in RAM, the GOP still showed the firmware splash.
fbcon stays on the real bar (PSE-36); the shadow starts when X mmaps.
The loader no longer prints to ConOut before ExitBootServices (it
paints a blue bar, then gold).

## 2026-10-05 — dualChip: one tree for i386 and x86_64

Branched `dualChip`. The Pentium path is unchanged (`ARCH=i386`, same
lock file, same `make test`). The 64-bit kernel is not written yet:
`make ARCH=x86_64` stops with a pointer to `arch/x86_64/PORT`.
`fetch-live.py --arch x86_64` will merge `live/packages.x86_64.txt`
(drop Firefox, add Chromium) when someone runs `--update`. Physical
addresses are `phys_addr_t` in `include/types.h`. Design: "Two chips,
one tree" in `docs/DESIGN.md`.

## 2026-10-05 — x86_64 kernel: hello in long mode

`make ARCH=x86_64` builds `arch/x86_64/early.c` and a UEFI loader that
stays in long mode. The loader leaves GOP + an 8×16 font in bootinfo at
`0x1000`. The kernel writes COM1 (QEMU) and paints the framebuffer
(real PC). QEMU+OVMF prints "Hello world from MyOS!  (long mode, UEFI)".
The same 66 MiB image on the MSI Tiger Lake-H painted the gold bar and
those two lines — GOP at `0x400000000` is just a pointer in long mode.
The Pentium path is unchanged. Next: paging and a higher half, then
link portable `kernel/*.c`.

## 2026-10-05 — x86_64 live rootfs tarball

`tools/fetch-live.py --arch x86_64 --update` wrote
`live/packages.x86_64.lock` (563 packages). `make ARCH=x86_64 live-extra`
cross-compiles myos-login, menu, Settings, My Files, Preview and
WritePerfect as ELF64 musl (`ld-musl-x86_64.so.1`) into
`build/live/extra.x86_64`. `make ARCH=x86_64 live-uefi` then writes
`build/x86_64/myos-live-uefi.img` (2.2 GiB: 152 MiB ESP with a 120 MiB
login initrd + 2048 MiB ext2 myos-root, Chromium not Firefox). The
64-bit kernel is still hello-only, so that image paints hello, not the
login.

## 2026-10-05 — x86_64 full kernel port begun

Rodger asked to run the x86_64 live userland on a real x86_64 kernel.
`docs/x86_64-PORT-PLAN.md` breaks the work into phases A–G. Makefile now
builds the portable kernel when `ARCH=x86_64`; the UEFI loader loads
initrd + E820; `include/user.h` adds 64-bit user addresses. `arch/x86_64/`
still needs real paging, IDT, syscall, and ELF64 — `port_stubs.c` is
temporary. Pentium `make test` unchanged.

## 2026-10-05 — x86_64: higher half, and BusyBox runs

Rodger asked for a review of the x86_64 port another AI started, and fixes
until it runs on QEMU. It does: under OVMF the 64-bit kernel starts `/init`
(64-bit busybox-static), which mounts, prints the motd and starts a shell;
`ls`, pipes, `mkdir`, files, `sleep`, exit statuses all work.
`make ARCH=x86_64 test-uefi-shell` types commands and checks the output.

What it took:
- The kernel moved to the top half, like Linux: direct map at
  `0xFFFF800000000000`, image at -2 GiB (`-mcmodel=kernel`), user space from 0
  (busybox is linked at 0x400000). The kernel half is two PML4 entries,
  shared by pointer with every process. Fork is copy-on-write on 4 levels.
- The syscall entry, `user_enter`, the GDT order (for sysret), EFER.SCE and
  every routine in uaccess.asm had bugs; each would have crashed the first
  program (details in docs/x86_64-PORT-PLAN.md).
- Syscall handlers take register-sized arguments (`uintptr_t`; on i386
  that is `uint32_t`, so nothing changes there). The x86_64 table is indexed
  by x86_64 numbers; stat, mmap, clone, arch_prctl (TLS) have adapters.

i386 `make test`, `test-shell` and `test-hd` pass. Next on x86_64: signals.

## 2026-10-05 — the missing system calls, on both CPUs

Rodger asked for the system calls that weren't implemented, for i386 and x86_64.
What "missing" meant was measured, not guessed: the syscall numbers loaded before
every `int 0x80` / `syscall` in both BusyBox binaries, minus the two tables, minus the
legacy fallbacks DESIGN.md drops.

- **Signals on x86_64** (were stubs): kernel/signal.c is shared; the frame and
  sigreturn moved to arch/<cpu>/signal.c (x86_64: Linux's rt_sigframe). Faults are
  signals. `pause` was missing on both (and i386's `waitpid` passed garbage as the
  rusage pointer once wait4 learnt to fill it).
- **New on both:** sendfile (page cache -> destination, through `file_copy_in`),
  times / getrusage / wait4's rusage (the tick charges user or system time),
  clock_settime / adjtimex / clock_adjtime (a settable 64-bit wall clock), syslog
  (dmesg: a 16 KiB ring of kprintf/klog), chroot (".." stops at it), pivot_root,
  syncfs, personality (`linux32` makes uname say i686), capget/capset,
  unshare/setns, swapon/off (-ENOSYS: no swap by design), inotify, fcntl record locks
  (POSIX and OFD, with deadlock detection), System V semaphores (with SEM_UNDO) and
  message queues, fadvise; the direct shm/sem/msg calls.
- **x86_64 layouts:** statfs, select, itimerval, sigevent, timeval, sysinfo, msghdr,
  rlimit, shmid/semid/msqid_ds.
- `user/sys/systest.c` (no C library, raw syscalls) checks 32 of them on each CPU,
  in `make test-uefi-shell`. i386 `make test`, `test-shell`, `test-hd` pass.
  `make bench`: unchanged except +40 cycles on open+close (docs/performance.md).

## 2026-10-05 — x86_64 live userland rebuild (Cursor)

Refreshed `live/packages.x86_64.lock` (563 packages; stale gcc pin had broken
sysroot fetch). Rebuilt sysroot, `live-extra` (ELF64 myos-login, menu, Settings,
Files, Preview, WritePerfect, Golden Hour theme), `login-initrd-x86_64.cpio`
(~120 MiB), `myos-live-root.img` (2 GiB), and `myos-live-uefi.img`.
`make ARCH=x86_64 test-live-uefi` mounts myos-root; `test-uefi-login` reaches
runit stage 1 but not myos-login — kernel lacks syscalls 430/326/190 (handoff:
`docs/x86_64-LIVE-HANDOFF.md`). No kernel changes in this pass.


## 2026-10-05 — x86_64 live desktop; the first real-laptop boot

**Desktop in QEMU.** myos-login, Xorg and IceWM now run on x86_64, from the RAM
login initrd and from the disk image. What it took:
- `mincore` was a no-op returning 0, and grep (gnulib) probes the stack's extent
  with it: 55 million calls, and first boot looked hung. It is real now
  (`mm_mincore`; ENOMEM for unmapped pages), on both CPUs.
- evdev's `struct input_event` was 16 bytes on x86_64 (should be 24: the timeval is
  two longs), so X never saw Enter.
- `/dev/fb0` mmap cut the framebuffer's physical address to 32 bits;
  `vma.phys` is `phys_addr_t` now (GOP is above 4 GiB on real machines).
- New on both: `copy_file_range` (shares sendfile's loop), the 12 xattr calls
  (no xattrs: set/get -EOPNOTSUPP, list 0). systest checks them.

**Real hardware: rebooted in a loop.** The laptop showed the loader's first two
lines and reset. Reproduced in QEMU just by giving it more RAM: with 2 GiB or more
the kernel died before its first line. Three bugs, none visible at 1 GiB:
1. `entry.asm` switched to its own page tables (first 1 GiB only) while RSP was
   still the firmware's stack — at 2.6 GiB. The first `push` faulted, the
   firmware's IDT was unmapped too: triple fault, reset. Now RSP moves to
   `boot_stack` before CR3 changes.
2. The x86_64 loader's fallback put the initrd "anywhere below 4 GiB"; firmware
   hands out the highest fit, above the kernel's 2 GiB direct map. Now below
   2 GiB (`KERNEL_RAM_LIMIT`, = `DIRECTMAP_LIMIT`). QEMU with 4 GiB takes this
   path too: the space after the kernel is busy.
3. Both loaders read the 120 MiB initrd with one `Read()`. Some firmware hangs or
   resets on that (Linux's EFI stub reads 1 MiB at a time for the same reason).
   Now 1 MiB pieces, a dot per 8 MiB, the 5-minute UEFI watchdog off (as GRUB
   does), and an error stays on screen 30 s.
QEMU: boots at 1, 2, 2.7, 4 and 8 GiB; the live image reaches the login screen
with 4 GiB. `test-uefi-shell` passes on both CPUs.

**Then the MSI booted to runit, but X did not start.** QEMU's screen is the
Bochs card (drivers/gpu/drm/tiny/bochs.c); a laptop's is the firmware's
framebuffer (simplefb), which X had never run on. `-vga cirrus` (no driver of
ours, so simplefb) reproduced it. Three bugs on that path:
1. `vm_iomap_memory` compared bytes: 800x600x4 is 468.75 pages, X maps 469,
   EINVAL. Linux rounds the area up to whole pages; so do we now (1366x768
   and 1600x900 laptops would have hit it too).
2. simplefb refused every FBIOPUT_VSCREENINFO, even of the current mode,
   which X sets at start. Like Linux, a driver with no `fb_check_var` now
   gets its one mode back as success.
3. The x86_64 loader had UEFI's pixel format backwards (UEFI's "BlueGreenRed"
   names bytes in memory order, i.e. XRGB): red and blue swapped in X.
With `-vga cirrus` the login screen and the IceWM desktop now come up with the
right colours; `make test`, `test-uefi-shell` (both CPUs) pass.

**Still stuck at runit stage 2 on the MSI: no COM1.** Console output goes to
the screen and to a serial ring that COM1's interrupt empties. The MSI has no
UART, so nothing ever emptied it: about 8 KiB into the boot it was full, and
the console then refused programs' writes (a write returns 0 when the ring has
no room, so the screen waits for the serial port). Getty, X and myos-login
stalled writing their first lines. QEMU `-serial none` reproduced it: `seq
3000` stopped at 1246. serial_init now checks the UART's scratch register
(no UART reads 0xFF) and switches serial output off without one.
The login service also says more on the screen: a line when it starts X, and
a runit `finish` script that shows the X log's (EE) lines when it fails.

**Still stuck at stage 2: the PIT does not tick.** (Rodger's
docs/hardware-msi-katana-gf66.md: a Tiger Lake board need not have a usable
PIT; Intel chipsets gate its clock to save power.) No IRQ 0 means no time
slot ever ends and no timed sleep wakes: stage 1 is scripts that never sleep,
so it finished; stage 2 is services that do (runsv waits 1 s before a
restart, myos-login polls X every 100 ms), so it stopped. QEMU
`-machine q35,pit=off` reproduced it: X started, then everything froze.
Now timer_check() (after `sti`) waits for the first tick, at most 50 million
TSC cycles; old PCs pass within 1 ms. Without one, drivers/hpet.c starts the
HPET (fixed address 0xFED00000; ACPI not read yet) in legacy replacement
mode, where its timer 0 raises IRQ 0 on the 8259 just as the PIT did, so the
tick handler is unchanged. The boot says which: `timer: the PIT does not
tick; the HPET gives the 1 ms tick`, and `serial: COM1` or `no COM1`.
`make TRACE=1` builds a kernel that prints every exec and exit on the screen
(build/x86_64/trace/). `test-uefi-login` now runs with `pit=off`; all tests
pass on both CPUs. `LIVE_STALE_LOCK=1` builds the live disk from the lock
when packages.txt has moved on (new packages are left out, with a warning).

## 2026-10-06 — ssh, a static web server, and compilers on the live disk
OpenSSH client and server are on both live disks. `sshd` is a runit service
(on by default). Empty passwords and root logins are refused
(`PermitEmptyPasswords no`); set one with `passwd` before expecting a remote
login. Host keys are made on first boot.

`darkhttpd` serves `/var/www/html` on port 80. It is installed and off.
`doas myos-httpd enable` turns it on; `disable` / `status` are the other two
verbs. No CGI.

`g++` moved from [build-only] (Mac sysroot only) onto the disk. Python now
has headers and pip. Java is OpenJDK 8 on i386 (Alpine has nothing newer
there) and OpenJDK 17 on x86_64. Incoming TCP still needs listen/accept
(`docs/gaps.md`); the daemons ship anyway.

## 2026-10-06 — Wi-Fi plan; W0a: MSI and MSI-X

Rodger wants Wi-Fi on the MSI (Intel AX201) with a Windows-style tray icon,
and the rule for it: **the kernel is ours, the interfaces are Linux's**. So
the 802.11 stack will be ours (nl80211 up, mac80211's driver API down) and
Intel's `iwlwifi` plugs in unchanged under its BSD-3-Clause terms, as on
FreeBSD. Plan and steps: `docs/WIFI-PLAN.md`; DESIGN.md records the rule.

W0a, the first step, is what every modern device needs: the AX201 (like the
MSI's NVMe and xHCI) has no line the 8259 sees ("irq 255"); it signals with
MSI-X, a memory write the chipset turns into an interrupt at the CPU's
local APIC. New:
- `arch/*/apic.c`: the local APIC (memory-mapped or x2APIC MSRs), switched on
  only when a driver first asks for MSI, so old PCs run as before. It masks
  the APIC's own timer/thermal/perf/error entries and leaves LINT0 (the 8259)
  as the firmware set it.
- IRQs 16-31 are MSI vectors 48-63 (the IRQ = vector - 32 rule just goes on);
  `interrupt_dispatch` acknowledges them at the APIC, not the 8259. Vector
  0xFF is the APIC's spurious one: an `iret`, nothing else.
- `drivers/pci.c`: Linux's names — `pci_find_capability`,
  `pci_alloc_irq_vectors` (MSI-X, else MSI, else the line), `pci_irq_vector`;
  `pci_bar_phys` reads 64-bit BARs; `pci_map_mmio` takes a 64-bit address.
- `nvme.c` is the first user: MSI-X when there is one, and then it never
  touches INTMS/INTMC (the spec forbids it under MSI-X).
- Partitions are named as Linux names them: `nvme0n1p1`, not `nvme0n11`.

`make ARCH=x86_64 test-uefi-shell` now attaches an NVMe disk: OVMF puts its
registers at 768 GiB, it runs on `MSI-X irq 16`, and its FAT32 partition is
read and mounted through it. The i386 kernel says it cannot map registers
above 4 GiB (no PAE) — under SeaBIOS the same disk is below 4 GiB and works
on MSI-X too. All tests pass on both CPUs.

## 2026-10-06 — W0b: netlink and the interface ioctls

Programs talk to the kernel's network code through netlink sockets: `ip`,
musl's getifaddrs(), GLib's network monitor, and for Wi-Fi wpa_supplicant
and iw (nl80211 is a generic netlink family). New `net/netlink.c`:
AF_NETLINK sockets (SOCK_RAW/DGRAM; NETLINK_ROUTE and NETLINK_GENERIC);
bind/getsockname with Linux's port ids; requests answered inside the
sendmsg into a queue of <= 4 KiB datagrams (musl reads with MSG_DONTWAIT);
MSG_PEEK/MSG_TRUNC; SO_PASSCRED credentials (GLib believes a netlink message
only with them). rtnetlink: RTM_GETLINK (dump or one link), RTM_GETADDR and
RTM_GETROUTE dumps. Generic netlink: the controller, CTRL_CMD_GETFAMILY by
name, id or dump, with multicast groups — the directory nl80211 joins in W2.
`net/dev_ioctl.c` (Linux's net/core/dev_ioctl.c): SIOCGIFNAME/INDEX/FLAGS/
SIFFLAGS (root: up/down)/ADDR/NETMASK/BRDADDR/MTU/HWADDR/TXQLEN/CONF on any
socket; `dev_get_flags` and `dev_change_flags` are shared with netlink.
Devices now have an `ifindex` (lo is 1).

BusyBox in QEMU prints what Linux prints: `ip addr` shows eth0 with its MAC
and `inet 10.0.2.15/24 brd 10.0.2.255 scope global eth0`; `ip route` shows
`default via 10.0.2.2 dev eth0` and the subnet route; `ifconfig` works (it
misses /proc/net/dev's counters). `make test-shell` checks both; systest
checks a link dump, the nlctrl lookup, an unknown family's ENOENT and
SIOCGIFINDEX on a UNIX socket, on both CPUs. The desktop still logs in with
no program crashing (trace build). All tests pass.

## 2026-10-06 — W0c: firmware files

Intel's Wi-Fi chip forgets its program at power-off; the driver hands it
1-2 MiB of "ucode" from /lib/firmware at every start. `kernel/firmware.c`
gives drivers Linux's `request_firmware` / `firmware_request_nowarn` /
`release_firmware` (`include/linux/firmware.h`): /lib/firmware/updates/
then /lib/firmware/. A file still in the initramfs archive is lent where it
lies (CLAUDE.md rule 5: no copy); a disk file, or one changed since boot, is
copied into the heap once. The file is held until it is released. Linux
also decompresses .xz/.zst firmware; we want plain files, so the live disk
build will unpack them. Self-test (make test): `firmware: myos-test.fw, 19
bytes, from the archive (no copy)`, and ENOENT for a missing name.

## 2026-10-06 — W1a: the Linux driver API, kernel services

Intel's driver (166 files, 118,000 lines for the AX201) calls about 220
Linux kernel functions below 802.11. The first third of them, written for
MyOS with Linux's names: `include/linux/` (types, list, bitops, bitfield,
byteorder, unaligned, err, printk, atomic, refcount, spinlock, mutex,
rcupdate, completion, wait, jiffies, ktime, delay, timer, workqueue, slab,
string, module, kconfig) and `kernel/linux_lib.c`, `linux_slab.c`,
`linux_sync.c`, `timer_list.c`, `workqueue.c`.

The design follows from how this kernel runs: one CPU, and kernel code is
only switched away when it sleeps (interrupts are off otherwise). That is
Linux's non-SMP, non-preemptive case, so a spinlock is "interrupts off",
RCU readers need no bookkeeping (Tiny RCU; frees are still deferred to a
worker), and `jiffies` is a real variable the 1 ms tick counts. Timers run
in one thread ("ktimers") from a sorted list — it sleeps exactly until the
first is due, and the tick itself does no timer work. Each workqueue is a
thread; system_wq is "events". `IS_ENABLED(CONFIG_…)` works as in Linux.
printk sends errors and warnings to the screen, info to the log, and
compiles debug out.

`make test` checks 15 things: Linux printf formats (%pM, %*ph, precision,
%zu, %llx), kstrto*, strscpy, lists, bits and fields, a 128-bit
mul-then-divide, kzalloc/krealloc/kstrdup, timers firing in order (a
deleted one not), work, delayed work (a cancelled one not), completions
with and without timeout, a mutex holding off another thread, and a
workqueue of its own. 154 self-tests pass; all suites pass on both CPUs.

## 2026-10-06 — W1b: devices, PCI drivers, IRQs, DMA (the Linux driver API)

The part of Linux's driver API that touches hardware: `struct device`
(names like "0000:00:14.3", drvdata, devm_* resources released newest first
when a driver lets go), the PCI driver model (an id table, probe/remove,
config space in bytes/words/dwords, BAR sizes found the standard way with
decoding off, pcim_iomap, MSI/MSI-X on top of W0a), request_irq and
request_threaded_irq (one native entry runs a line's Linux handlers; a hard
handler that answers IRQ_WAKE_THREAD gets its thread part run by
"irq/N-name", where it may sleep), and the DMA API (no IOMMU: the device
address is the physical one; dma_alloc_coherent, dma_map_single/page,
dma_pool, struct page = its direct-map address with pmm share counts as its
owners). Linux's kmalloc memory is physically contiguous and drivers DMA
from it, so the shim's kmalloc above 1 KiB now takes a page run through the
direct map (`kmalloc_contig`; kfree knows it by a 16-byte header, where a
slab page keeps its own).

Native renames to make room for Linux's names: the ISR type is `isr_fn_t`
(Linux's irq_handler_t has another shape), pci_dev's bus number is `busnr`
(Linux's `bus` is a struct pci_bus pointer, which iwlwifi follows to
`->self`). New `pic_mask`.

`make test` adds QEMU's "edu" card, a teaching PCI device, and binds a
driver written only against Linux's API to it: probe by id table, BAR size
1 MiB, a register that inverts what is written, an MSI interrupt through a
hard and a threaded handler, DMA from RAM into the card and back (a
coherent buffer out, a kmalloc'd one mapped back in, compared), and
remove with devres switching decoding off. 164 self-tests pass; all suites
pass on both CPUs.

## 2026-10-06 — W1c: packets and network devices (the Linux driver API)

One packet type for the network stack and for Linux drivers: struct sk_buff
now has Linux's layout — next/prev first (a queue head poses as a packet,
as in Linux), the 48-byte control buffer (mac80211 keeps its tx info there),
header offsets, checksum fields, and after the data the "shared info": up to
17 page fragments (a Wi-Fi card hands over received pages without a copy)
and the count of clones sharing the block. Blocks over 1 KiB are contiguous
pages, so a driver may DMA skb->data. `linux/skbuff.h` +
`kernel/linux_skbuff.c`: queues, put/push/pull/trim, frags, clone, copy,
pskb_expand_head, linearize, skb_header_pointer across the boundary.
Order-n pages are Linux's compound pages: the last put_page frees all 2^n
frames (a small hash table keeps their order).

`linux/netdevice.h` + `kernel/linux_netdev.c`: alloc_netdev (private area
32-byte aligned, as Linux guarantees), register_netdevice (listed, still
down — bringing it up is dev_open, which wpa_supplicant will do), stopped
transmit queues (dev_queue_xmit leaves the packet with its sender), carrier,
NAPI on a dummy device (napi_schedule now wakes the network thread), rtnl.
The native stack changed with it: alloc_skb(size, gfp), eth_type_trans
returns network order, netif_napi_add has Linux's 3 arguments (+ _weight),
and an ifindex slot is never reused while its device lives.

`make test` checks it all and that every skb and page comes back: 183
self-tests pass, and every suite passes on both CPUs. W1 is done: the
driver API below 802.11.


## 2026-10-06 — W2a: the 802.11 headers; Intel's iwlwifi compiles on them

The driver interface of our 802.11 stack, written down: `linux/ieee80211.h`
(the frame header and its frame-control tests, management and action
frames, element ids, cipher suites, the HT/VHT/HE/EHT/UHR capability bits),
`net/cfg80211.h` (wiphy, bands, channels, chandefs, scan and scheduled-scan
requests, found networks, station statistics, regulatory calls) and
`net/mac80211.h` (ieee80211_hw, vif, sta, txq, the tx and rx info that live
in skb->cb, the 76 callbacks iwlwifi fills in, and the core functions it
calls). Ours, written from what the driver uses — about 770 names out of
Linux's 3,400; a script compared every one of our 400 constants with
Linux's values (one wrong bit found and fixed). Three ISC-licensed Linux
headers are copied with their notices: the nl80211 UAPI, `net/regulatory.h`
and `net/ieee80211_radiotap.h`.

Four iwlwifi headers are GPL-only, so they are ours: `iwl-debug.h` (same
macros; debug lines compile to nothing unless CONFIG_IWLWIFI_DEBUG),
`iwl-devtrace.h` (empty trace points — every register access passes one, so
they must cost nothing), `mvm/rs.h` with `mvm/rs-myos.c` (the AX201's
firmware chooses rates; the driver-side algorithm isn't needed), and
`mei/iwl-mei.h` ("no Management Engine", what Linux has without
CONFIG_IWLMEI).

The rest of the Linux API the driver reaches for: scatter lists (with
their chain encoding), Linux 7's typed allocations (`kzalloc_obj`),
overflow-checked sizes, `__free(kfree)` and lock guards, struct_group,
DEFINE_RAW_FLEX, per-CPU pointers (one CPU: plain pointers), DMI tables,
ACPI object types, PTP clock and cross-timestamp types, kobject events,
rfkill, cache alignment. Two of Linux's choices now hold for drivers here
too: `u64` is `unsigned long long` on both CPUs (drivers print it with
%llu), and PAGE_SIZE is an unsigned long; the driver builds with
`-funsigned-char`, as Linux does. BUILD_BUG_ON became Linux's kind (also
accepts a non-constant inside a `__builtin_constant_p` test).

*Check:* `tools/iwlwifi-check.py` — 72 of 72 iwlwifi files compile with
-Wall and no warnings. `make test` 183 self-tests (one run of four failed
racetest's "epoll_ctl while the fd is replaced"; the next three passed —
watching it), `make test-shell`, `make ARCH=x86_64 test-uefi-shell` pass.
Next, W2b: our 802.11 core behind these headers and a simulated radio.

## 2026-10-06 — W2b: our 802.11 core finds networks (on a simulated radio)

`net/wireless/` is MyOS's 802.11 core, behind the W2a headers. Linux splits
this into cfg80211 and mac80211 so that "full MAC" cards can skip the
second; we have only soft-MAC drivers (iwlwifi, the simulated radio), so it
is one core, and the configuration half calls the frame half directly —
no ops table with a single implementation.

A driver allocates its radio (`ieee80211_alloc_hw`: the wiphy, the core's
state and the driver's private data in one block), fills in bands and
flags, and registers: it becomes phy0 and a station interface `wlan0`
appears with the card's address. `ip link set wlan0 up` (or the test)
starts the driver and adds the interface; down undoes both. Frames the
driver receives in its interrupt are queued in O(1) for a work thread.
Scans are the radio's own (`hw_scan`), as on Intel's cards: the core
builds the probe-request elements per band (rates, HT, VHT) and hands the
request down; beacons and probe responses heard meanwhile fill a table of
64 networks per radio (looked up by BSSID and channel, the channel taken
from the AP's own DS or HT element, the oldest entry replaced when full);
the driver reports the end and the core frees the request.

To test it without a card, `drivers/net/wireless/sim.c` (test builds
only): a radio with 13 channels at 2.4 GHz and 4 at 5 GHz, written to the
same driver API as iwlwifi, scanning an "air" with two access points that
answer with probe responses (one WPA2 with an RSN element, one open).

`make test`: 194 self-tests pass, 10 new — wlan0 appears and comes up, a
17-channel scan finds both networks with the right channel, signal and
security, a second scan while one runs is refused, down stops the driver.
On the way: skb queues link through a two-pointer view of the head (as
Linux's sk_buff_list), which GCC 15's bounds checks wanted.

## 2026-10-06 — W2c: nl80211; Alpine's iw scans on MyOS

Wi-Fi's user-space interface: `net/wireless/nl80211.c` answers the
generic-netlink family "nl80211" with Linux's messages and numbers —
protocol features, the radio (GET_WIPHY: everything in one message, or
split one band per message when the program says it can put them back
together, which large Intel radios need), the interfaces, starting and
stopping a scan, the scan results (each network with its BSSID, channel,
signal, TSF, beacon interval, capabilities and elements), and the "scan"
events. For it, `net/netlink.c` now lets other files build messages, answer
requests and send multicast events (`include/genetlink.h`).

Two things learned from running the real programs: libnl reads one
datagram, stops at the answer it wanted and drops the rest, then waits for
the ack in the *next* datagram — so the ack must travel alone, as Linux
sends it (a GetFamily answer and its ack shared a datagram until now). And
iw starts listening for "scan done" only after it has asked for the scan
(its source warns about that race); on hardware a scan takes seconds, so
the simulated radio now dwells 10 ms on each channel too.

The `make test` harness used to type its keys 8 seconds after boot; the
suite outgrew that and was cut short. It now waits for the "Self-tests:"
line. Two page checks learned to allow the kernel heap's growth, and the
raw syscall test (now also in `make test`, as /bin/rawsystest) removes the
files it left in /tmp — their pages counted as leaked.

*Check:* `make test`, 196 self-tests: rawsystest drives nl80211 by hand,
then Alpine's iw 6.9 and libnl 3.9 (pinned in fetch-alpine.sh, test
builds only) run `iw dev` and `iw dev wlan0 scan` against the simulated
radio and print both networks — MyOS-Test, 2437 MHz, −42 dBm, RSN CCMP/PSK;
MyOS-Open-5G, 5180 MHz — as on Linux. UEFI shell tests pass on both CPUs.

## 2026-10-06 — W3a: joining a Wi-Fi network

`net/wireless/mlme.c` joins a network as a station, the way Linux's
mac80211 and cfg80211 do it between them: find the network (scanning for it
by name if the table hasn't heard it), take the strongest access point of
that name, tune a channel context to it, create the AP as a peer in the
driver, send Authentication (open system) and Association Request, each
tried three times 300 ms apart, and on success take the association id and
put the carrier up. The driver hears every step in mac80211's order —
sta_state one state at a time, channel-context add/assign, bss_info /
vif_cfg / link_info changes, mgd_prepare_tx/complete_tx around each
exchange — so iwlwifi will see the sequence it expects. An open network is
usable at once; a protected one waits for wpa_supplicant (W3c).

nl80211 gained CONNECT, DISCONNECT and GET_STATION, the CONNECT and
DISCONNECT events, the "associated" mark in scan results and the SSID and
channel of a connected interface — what `iw dev wlan0 link` reads. The
simulated radio grew an access point: on its channel it answers
Authentication and Association after 2 ms "in the air", and forgets us when
deauthenticated.

`make test`: 199 self-tests pass. The kernel joins MyOS-Open-5G (AID 1, the
driver's peer authorized) and leaves it (the peer gone); Alpine's iw runs
`connect -w`, `link` and `disconnect`. Found on the way, not Wi-Fi: each
busybox `timeout` leaves an orphaned watcher whose pages stay held for
seconds, which tripped the page-leak check; the test script doesn't use
`timeout`, and the orphan question is a separate task. user_test.c now
prints any program that leaves pages behind.

## 2026-10-06 — W3b: IP over Wi-Fi

`net/wireless/data.c` carries Ethernet frames over the air: outgoing ones
become 802.11 data frames (to the AP, the RFC 1042 LLC/SNAP header in
front of the Ethernet type) and go to the driver's queue for their
priority — the TXQ model iwlwifi uses (wake_tx_queue, then the driver
pulls with ieee80211_tx_dequeue) — or to its tx(); incoming ones from our
AP turn back into Ethernet frames for the network stack. Until the link is
authorized only EAPOL passes, both ways (wpa_supplicant's control port,
for W3c). netif_rx now wakes the network thread, as Linux raises its
receive softirq.

The IPv4 stack, which knew one card (eth0), now runs on one device at a
time: the last link to become usable takes it over, each device keeps its
lease for when it is active again, and the kernel's DHCP client runs for a
new device in a thread of its own. When Wi-Fi leaves, IPv4 goes back to
eth0 and its old lease. The packet handlers are registered even without
eth0 — the MSI laptop has no Ethernet driver, only Wi-Fi.

The simulated access point got a small network behind it: it answers ARP
for 192.168.77.1, runs a DHCP server that hands out 192.168.77.10, and
answers ping. One test lesson: a kernel thread pinging with interrupts on
never lets the work threads that carry Wi-Fi frames run (net_sleep halts
instead of sleeping), so the self-test pings as a syscall would, with
interrupts off.

`make test`, 202 self-tests: IPv4 moves to wlan0, leases 192.168.77.10,
pings 192.168.77.1, and is back on eth0 with 10.0.2.15 afterwards; from
user space `ifconfig wlan0` shows the lease and busybox ping answers in
4 ms. test-shell and both UEFI shell tests still pass.

## 2026-10-06 — W3c: WPA2 with wpa_supplicant

Alpine's wpa_supplicant 2.10, unchanged, now joins the simulated WPA2
network: it scans, connects through nl80211, runs the 4-way handshake
with the simulated access point, installs the pairwise and group keys,
and opens the port; then the kernel's DHCP client gets 192.168.77.10 over
the protected link and ping answers.

What the kernel needed:
- **Keys** (`net/wireless/key.c`): NEW_KEY / DEL_KEY / SET_KEY go to the
  driver's set_key, as mac80211 does; we have no software crypto, so the
  hardware encrypts and decrypts. SET_STATION "authorized" opens the
  control port and starts IPv4.
- **AF_PACKET sockets** (`net/packet.c`): wpa_supplicant sends and
  receives EAPOL frames on one.
- **/dev/random and /dev/urandom**: wpa_supplicant reads its nonces
  there and gave up without them ("Could not open /dev/urandom").
- **EAPOL during association**: the AP sends handshake message 1 right
  behind its association answer. Our data path saw it before the MLME
  work had handled the answer and dropped it, so the handshake timed out
  after 10 s. Linux's mac80211 lets EAPOL from the AP through before the
  association is marked; now ours does too.

The simulated AP is a WPA2 authenticator now (test builds only): SHA-1,
HMAC, PBKDF2, the 802.11 PRF, AES and AES key wrap in
`drivers/net/wireless/sim_crypto.c`, checked at boot against published
test vectors so a failed handshake points at the kernel and not the test
AP. Its frames aren't really encrypted — "the air" delivers a protected
frame only if the receiving hardware holds the key the AP derived, which
is what a wrong key would look like on a real radio.

`make test`: 207 self-tests pass; test-shell and both UEFI shell tests
still pass.

## 2026-10-06 — Unix datagrams; the Wi-Fi icon; Wi-Fi on the live disk; iwlwifi links

**wpa_cli and Unix datagrams.** wpa_supplicant's control socket — how
wpa_cli and the tray icon talk to it — is a Unix *datagram* socket, which
MyOS didn't have (`connect` said ECONNREFUSED). `net/unix.c` has them now:
each datagram is one kmalloc'd record in the receiver's queue (at most 16
and 64 KiB waiting), with the sender's name, so `recvfrom` can answer it;
`connect` just names where `send` goes; a socket connected elsewhere
refuses strangers (EPERM, Linux's rule); a closed one refuses everything
(ECONNREFUSED); `socketpair` works too. boottest checks it the way wpa_cli
uses it, and `make test` now runs `wpa_cli status` against the real
wpa_supplicant: COMPLETED, MyOS-Test, WPA2-PSK.

**The tray icon** (`live/myos-wifi/myos-wifi.c`): signal bars, a list of
networks that drops up from the tray, Connect, the network security key.
Built for the 64-bit disk; its screendump test is still to do.

**The live disk** gets wpa_supplicant, iw and wireless-regdb (64-bit: the
MSI), the AX201's firmware from linux-firmware (pinned by tag and SHA-256),
a runit service that waits for wlan0, and the icon in IceWM's startup.
`fetch-live.py --add` pins new packages and leaves every existing pin as
it is (`--update` re-resolves all of them).

**iwlwifi links.** Intel's driver, all 72 files, is now part of the x86_64
kernel and its init runs at boot. Linking it showed 64 functions we lacked;
none needed a design change. Most were mac80211 calls for modes we don't
offer (being an access point, TDLS, block-ack sessions we never start),
which do nothing and say why. The ones that matter:
- finding our AP and our keys (one peer per interface, so no searching);
- losing the connection (we leave; wpa_supplicant looks again);
- a firmware restart: we drop the connections without telling the driver
  (it has forgotten them), start it again and give it back its
  interfaces;
- the firmware's country rules applied to our channels;
- loading firmware in the background;
- PCI function-level reset, remove and rescan.

The kernel grows from 374 KB of code to 851 KB; `IWLWIFI=0` leaves the
driver out. In QEMU the driver registers and finds no card, which is all
QEMU can show; the card itself is W7, on the MSI.

## 2026-10-06 — The Wi-Fi icon on the desktop; the MSI image

The tray icon now works on the 64-bit desktop, checked like a person would:
`make ARCH=x86_64 WIFISIM=1 test-wifi-desktop` (`tools/qemu-desktop.py`,
script `tools/wifi-desktop.qs`) logs in, clicks the icon, picks MyOS-Test,
types its key, and the kernel gets its lease. Getting there needed:
- **GTK in the RAM login image.** It had none, so no GTK program could
  start there (the Wi-Fi icon, and Settings and My Files too). gtk+3.0 is
  in the [desktop] group now: +17 MiB.
- **A notification area.** The theme's tray is myos-menu's picture over
  IceWM's taskbar, with a fixed network icon in it. myos-menu is now the
  system tray (the freedesktop.org protocol): the real icon docks in that
  slot.
- **Its window showing at all.** Its window is a child of IceWM's taskbar,
  and IceWM ignores map requests for windows it didn't make, so the tray
  was never drawn on the 64-bit desktop. It is override-redirect now.
- **SIGNAL_POLL.** wpa_supplicant asks for the channel survey with every
  signal poll and failed without one; nl80211 answers GET_SURVEY now. The
  icon shows real bars.

`build/x86_64/myos-uefi-login.img` is the MSI's image: iwlwifi in the
kernel, the AX201 firmware, wpa_supplicant, the icon, and the home network
from `live/overlay.local/` (git-ignored; only the derived key, not the
password). In QEMU it reaches the login screen; the card itself is W7.

## 2026-10-06 — W7: iwlwifi on the real AX201, first run

On the MSI, Intel's driver came up on our kernel: it found the card
(00:14.3), loaded its firmware (QuZ-a0-hr-b0-77, the right revision), read
the firmware's regulatory rules (world, "00", 28 rules) and registered
wlan0 with the card's own address and 51 channels. Then the firmware
crashed on the first scan: UMAC ADVANCED_SYSASSERT 0x2000278C, with "last
host cmd" 0x0013010D (group 1, command 0x0D: SCAN_REQ_UMAC). Our restart
brought it back once; the second crash made iwlwifi reset the card and
make a new wlan0, which wpa_supplicant never noticed.

Found by reading what our scan request gives the driver against what
cfg80211 gives it: the BSSID was all zeros (cfg80211: the wildcard,
ff:ff:ff:ff:ff:ff), and mvm copies it into the probe request it hands the
firmware. Fixed in the core's wifi_scan, with cfg80211's other defaults
(every rate, no TSF link), and nl80211 now checks scan flags against what
the radio offers, as cfg80211 does. The simulated APs now ignore probe
requests to any other BSSID, as real ones do, so the self-tests catch it.
Whether the zero BSSID is what the firmware choked on, the next run says.

For that run: `make IWLDEBUG=1` builds iwlwifi's own debug lines in (every
command it sends the firmware, scans, joining) into a 256 KiB kernel log;
the normal kernel log is 64 KiB now (one crash dump is ~5 KiB). The
wpa_supplicant service starts over when wlan0 is made again. xterm has a
scrollbar and 10000 lines of scrollback.

## 2026-10-06 — W7, second run: scanning works; joining crashed

With the BSSID fixed the firmware scanned without trouble and the list
filled. Joining wilson1 then crashed it: iwlwifi's debug lines showed the
channel (PHY_CONTEXT_CMD) accepted and the firmware asserting on the
LINK_CONFIG_CMD that activates the link. That command is built from the
link's settings, and two of them were never given to the driver:
- the link's own address (bss_conf.addr): mac80211 copies the interface's
  address there when it makes the interface; we sent 00:00:00:00:00:00;
- the access categories' timing (conf_tx): mac80211 gives the standard's
  defaults when an interface comes up; ours stayed zero (AIFS 0 isn't a
  valid setting).
Both are set now (`wifi_default_edca`: 802.11-2020 Table 9-155; plain
DCF after joining an AP we don't use WMM with), the AP's beacon interval,
DTIM, rates, slot and preamble are in place before the link is activated,
and the self-test checks the address and the EDCA values the simulated
radio receives. A firmware restart that leaves the interface out of the
driver no longer means every scan fails ("network down") until a reboot:
the next scan hands it back.

Third run: the screen froze right after logging in, so no photos, with X
covering whatever the kernel printed. So the next image boots without X:
the kernel has a command line now, /proc/cmdline, built in for the moment
(`make CMDLINE=nodesktop`), and the login service skips X when it says
nodesktop. With TRACE=1 and IWLDEBUG=1 the text console then shows the
kernel's lines and iwlwifi's as they happen: if it hangs again, the last
screenful says where.

Fourth run, text console: the kernel itself crashed — a page fault in
iwlwifi's queue allocation (iwl_mvm_sta_alloc_queue), on a station pointer
that pointed at freed memory. Our restart after a firmware error had freed
the AP's peer without telling the driver (its firmware was dead), while
iwlwifi still had that peer's queue on its to-do list. It also showed why
the restart never worked: iwlwifi looks for the interface it is re-adding
among the ones the stack kept, and we had marked it gone ("-16" and a
warning from mvm/mac-ctxt.c).

The restart is mac80211's now: nothing is freed; the driver is started
again and told the whole connection once more, in mac80211's order (the
interface, the channel context, queue timing and link settings, the AP as
a peer state by state, the keys), and frames that waited are sent. A peer
dropped while the driver is truly lost is kept until the driver is gone. The
simulated radio can now crash its "firmware", and the self-test checks the
connection survives it and ping still answers (209 self-tests).

The desktop test was flaky (1 in 3): Enter went to the login screen before
X took keys, and the pointer sometimes landed a dozen pixels off (QEMU
hands X two moves at once and X speeds them up). myos-login now says on
the console when its screen is up and when someone logged in; the test
presses Enter until it has; the tray's list is driven by the keyboard,
which the list now supports properly (choosing a network puts the focus on
its Connect button, as on Windows). Three runs out of three pass.

## 2026-10-07 — W7, fifth run: a crash in memcpy; crash messages name the callers

The firmware failed again while restarting (FW_ERROR_RECOVERY_CMD), iwlwifi
gave up on the card ("Device error - reprobe!"), commands failed with -5,
and then the kernel read address 0 inside memcpy. memcpy has hundreds of
callers, and the crash message showed only where it stopped. Kernel crash
messages now also list the return addresses found on the thread's stack
(no frame pointers, so a scan, as Linux does without them): newest first,
for addr2line. On i386 `make test`'s F10 fault shows selftest_key called
from kernel_main.

What makes the firmware fail in the first place is still unseen: each time
the restarts after it scrolled it away, and the crash prevented dmesg. So
`make IWLDEBUG=1` now also sets iwlwifi's fw_restart off: after a firmware
error the card just stops, the system stays up, and dmesg has the first
error with the commands before it.

## 2026-10-07 — W7, sixth run, and mac80211's order, read from its source

The sixth run (no restarts) finally showed the first firmware error:
LINK_CONFIG_CMD, a link *modify*, before authentication — the slot-time
change our core sent with the BSSID. The crash after it was ours: iwlwifi
copies link_conf->bssid on every change, and our teardown set it NULL
(mac80211 keeps it pointing at a buffer and zeroes that).

Then, as Rodger suggested, the whole connection path compared step by step
with mac80211's own code (net/mac80211/mlme.c and chan.c of Linux 7.2.9,
read for the order, not copied). Differences found and fixed: the AP's
BSSID, beacon interval, basic rates and beacon timing (sync_tsf, the card's
receive time, DTIM count) are filled in before the channel and announced
after it, before the AP becomes a peer; the transmit power and "not idle"
follow the channel; the DTIM period is set only at association, with slot
time and preamble; an open network's peer is authorized before the
association is announced; mgd_prepare_tx comes before every try, with no
duration.

The simulated radio is now strict about the rules the MSI's firmware
enforced (NULL bssid, slot/preamble before association, BSSID before the
peer, link address and EDCA before activation, zero BSSID in scans):
`make test` fails if the core breaks one, the user-space wpa_supplicant runs
included. It caught one at once (the restart replay activated the link
before re-sending EDCA). The desktop test's pointer search was fooled by
the tray icon's black tooltip; it looks close to the expected place first.

Seventh run: no kernel crash; the firmware asserted on TLC_MNG_CONFIG_CMD
(its rate control for a peer) — "last host cmd" 0x001F050F, group 5,
command 0x0F. iwlwifi sends it the moment the AP becomes a peer, "already
from what the AP advertised in the beacon/probe response" (its comment);
our peer had no rates until the association answer. mac80211 fills them
from the beacon before adding the peer (ieee80211_mgd_setup_link_sta); so
does our core now, and the strict simulated driver checks it.


## 2026-10-07 — USB sticks: the live system runs from the stick

Rodger's MSI could only run the small login initramfs: once MyOS was up it
could not see the stick it came from ("storage: no disks"). Now it can.
Our own drivers (Linux's xhci-hcd and usb-storage are GPL-only, so the
licence rule keeps them out), written from the xHCI 1.2 and USB
mass-storage bulk-only specs; plan and steps in `docs/USB-PLAN.md`.

- `drivers/usb/xhci.c`: the controller. Rings of 16-byte TRBs shared with
  it, like NVMe's queues: a command ring, one transfer ring per endpoint,
  an event ring for completions. Firmware hand-off, reset, MSI-X; a "usb"
  thread resets each connected port, gives the device an address, reads
  its descriptors and finds it a driver, while the IDE/AHCI/NVMe drivers
  start. Later plugs come in through the port-change interrupt.
- `drivers/usb/usb_storage.c`: a stick is a SCSI disk behind two bulk
  pipes. Command block and data are queued together, straight from the
  page cache's pages (scatter-gather, no copy); then the status.
- `sda`, `sdb`... are shared with AHCI now (they both used "sda").
- The x86_64 loader says when it was started from USB; the kernel then
  waits up to 10 s for that stick's `myos-root` before falling back to RAM.

First run: a read of sector 0 timed out. QEMU's trace showed why: its
emulated stick answered the data, and the status request I had already
queued behind it went "async" and never completed (QEMU starts the next
transfer before it has finished the last). Linux and Windows ask for the
status only after the data, so that is what we do too: two interrupts per
command instead of one (Linux takes three).

*Check:* `make ARCH=x86_64 test-usb` — OVMF boots the 2.2 GiB live image
from a QEMU USB stick (no other disk); our driver finds it, `sda2` becomes
`/`, the login screen is up 18 s after QEMU starts. Also by hand: a USB 2
port (480 Mb/s), QEMU's NEC controller, and the stick pulled out before the
kernel looked and plugged back in 3 s later (the kernel waited, found it,
mounted it). `tools/test-disks.sh` gained a USB run: ext2test 24/24, fstest
158/158 through the stick (the 32-bit kernel). Its other runs had been
failing on the Linux-shim PCI tests, which need QEMU's `edu` card: now added.
One of the four first runs failed racetest's known flaky "epoll_ctl while
the fd is replaced"; three more passed. `make test` 210/210,
`test-uefi-shell` and `test-wifi-desktop` pass.
Benchmark (`tools/diskbench.sh usb`, `docs/performance.md`): cold 64 KiB
read 31 311 instructions vs Linux 6.1's 117 431; 64 KiB write 103 322 vs
752 527.

Also: the Wi-Fi core now calls the driver's prepare_multicast before
configure_filter, as mac80211 does; iwlwifi warned at mac80211.c:1948
(no multicast filter command) on every association.

U4, the same day on the MSI: it works. Both Intel controllers started on
MSI (8086:9a17, the CPU's Thunderbolt xHCI, 5 ports; 8086:43ed, the
chipset's, 24 ports, USB 3 on 17-24). The stick, 24a9:205a "USB DISK 3.2",
60 000 MiB, became sda; `ext2: sda2 mounted at /` — the desktop and xterm
ran from the stick. The webcam (30c9:0042) and the AX201's Bluetooth
(8087:0026) were listed. The stick connected at 480 Mb/s on USB 2 port 4:
still to find out whether that was the USB 2.0 socket or a USB 3 link that
did not come back after our reset.

## 2026-10-07 — Chromium's first two walls, and a gap audit

Chromium (x86_64, Alpine's 220 MB binary) wouldn't start on the MSI: "Out of
memory" from execve. The 64-bit program loader was a port shortcut that
read the whole file into one kmalloc block, then copied every segment.
`kernel/elf64.c`'s elf_map now works like the 32-bit one: it reads the
headers, adds each segment as a file-backed area (page runs with equal
rights go in as one area), and pages come in on first touch.

Next it died with "CPU exception 13" at a deliberate `int3` (Chromium's
IMMEDIATE_CRASH) after PartitionAlloc failed to reserve its 32 GiB pool:
the mmap family took 32-bit lengths. They are address-sized now (mmap,
munmap, mremap, mprotect, the gap finder); the i386 build is unchanged.
Not yet seen with a window. Why an `int3` shows as #GP: every x86_64 IDT
gate is DPL 0 (now in docs/gaps.md).

Then, at Rodger's request, `docs/gaps.md` was checked against the code and
given an "At a glance" section: what a Linux program trips over, TCP/IP in
one list. Stale entries fixed (USB, Wi-Fi, sockets, getrandom, GPT, FIFOs,
inotify, eventfd, SysV semaphores/queues, TCP bind); new ones found
(select sees fds 0-31 of 64, PROT_NONE and NX not enforced on x86_64,
madvise a no-op, serial input never read, 2 GiB RAM on x86_64, no
loopback, setsockopt ignored). `tools/syscall-coverage.py` counts system
calls against Linux's own tables: x86_64 has 218 of 369, 3 ignored, 148
missing.

*Check:* `make ARCH=x86_64 test-uefi-shell` and `test-usb` pass with the new
loader; both kernels build without warnings.

Later the same day (docs/WORKLIST.md, 8/72): serial input (the UART's
receive interrupt feeds the console, so a login on ttyS0 works and QEMU
tests can type), SOCK_SEQPACKET and SCM_RIGHTS for Unix sockets,
/proc/self/task (link count 2 + threads), /proc/self/fd listing only open
fds, and 512 threads / 256 processes on x86_64 (256 / 128 on i386). The
64-bit mmap entry still refused lengths over 4 GiB on its own; fixed.
`make test` 215/215, `test-uefi-shell` pass.

## 2026-10-07 — Installer wizard and helper

`myos-install` (GTK, nine pages from the Golden Hour mockups; also `--text`)
writes a plan; `myos-install-helper` (setuid root) checks it and would
partition, format, copy, configure and make the disk bootable. On this
kernel it stops before writing: no block device nodes, and the MSI's SSD
is still hidden. Start menu: Install MyOS. `make test-install` is not
built yet (WORKLIST C6).

## 2026-10-07 — All Programs

The Start menu's left column ends with All Programs, as on Windows XP: rest
the pointer on it (or click it) and the Programs folders open beside it.
A folder opens another menu to the right; a program runs. The tree is
`/usr/share/myos/programs`, with `~/.local/share/myos/programs` merged on
top (Hidden=true removes a name), and any `.desktop` file under
`/usr/share/applications` that is not already listed, filed by category.

## 2026-10-07 — WritePerfect Preview opens MyOS Preview

File > Preview (Ctrl+Shift+P) built a PDF and then asked GTK to open it.
Nothing on the disk claimed `application/pdf` (no mimeapps.list), and the
fallback was Evince, which we no longer ship. The Mac opens that PDF in
Preview.app by name. Linux now starts `/usr/bin/myos-preview` the same way,
writes the PDF to `/tmp` if the document's folder is not writable, and
`/usr/share/applications/mimeapps.list` makes Preview the default for PDFs
and pictures.

The first ship still failed in the VM: xsession has no D-Bus, so
GtkApplication's `HANDLES_OPEN` never delivered the file and Preview
showed its Open dialog (or quit). Preview now opens command-line paths
itself and does not ask GApplication to carry them.

## 2026-10-09 — Claude Code on the 64-bit disk

Claude Code has no 32-bit build, so it is not on `build/myos-live.img`.
The 64-bit disks get the musl binary (2.1.295) at `/usr/lib/claude/claude`,
with `/usr/bin/claude` telling it to use Alpine's `rg` instead of the
glibc ripgrep packed inside it. `ripgrep` is pinned in
`live/packages.x86_64.lock`. It prints `2.1.295 (Claude Code)` on a CPU
with SSSE3 and POPCNT (the MSI and the Pocket 4; a Pentium 4 does not
have those, and it is not the machine this binary runs on). Python,
gcc, g++ and Java already ran on both disks: `python 42`, `c-ok`,
`cpp-ok`, `java-ok` (OpenJDK 8 on i386, OpenJDK 17 on x86_64).

## 2026-10-07 — files on the desktop, and Wallpaper

Twenty more nature pictures live in `/usr/share/myos/wallpapers` and show under
Desktop Pictures, next to the theme's own landscape.

`~/Desktop` is created if it isn't there, and drawn on the wallpaper: one icon per file, down the left,
the same size and spacing as the Recycle can, which keeps the fifth place.
Double-click opens the file. Right-click on the wallpaper has New Folder
and Change Desktop Background…, the Mac's item. That opens Settings on
Wallpaper: the picture as it will look, Fill Screen and the other fits,
Add Photo…, the pictures you have, and a row of colours. myos-desktop
paints the choice, because the root pixmap has to outlive Settings.

## 2026-10-07 — rm goes to ~/Trash

Unix `rm` unlinks the name. Ours (`live/myos-rm`, installed as both
`/bin/rm` and `/usr/bin/rm`) is the only `rm`. A logged-in user gets a
rename into `~/Trash` plus `._name.where` (path, uid, gid, mode, mtime)
so Put Back can restore it. Root and `--reallyDelete` exec Alpine's
`/usr/bin/coreutils` as `rm`, so first-boot scripts see GNU rm exactly.

The first overlay of `/bin/rm` followed Alpine's symlink into the one
coreutils binary and replaced `mkdir`, `chmod` and `touch` with our rm
(the boot lines `invalid option '-p'` / `'-m'`). Unpack now unlinks
before writing. That clobber also left `/etc/shadow` half-written, so
the live login (Enter, empty password) failed. The Recycle can
(`myos-trash`) takes drops and its right-click Empty Trash runs
`rm --emptyTrash`. My Files Delete and drag use the same `rm`.


Later still (WORKLIST 12/80): sigaltstack (A11; frames on the alternate
stack, SS_AUTODISARM, uc_stack restored by sigreturn), SO_PASSCRED and
SCM_CREDENTIALS (A10), /proc/cpuinfo and /proc/meminfo made up when read,
the inotify limits, /sys/devices/system/cpu (A12), rt_sigqueueinfo and
rt_tgsigqueueinfo (A13). systest checks each, on both CPUs. The flaky
racetest is fixed (the armed copy yields until the sibling has acted), and
verify-racetests catches all eight reintroduced bugs again (three of its
patches had gone stale on 2026-10-05). The Makefile's overlay dependency
list keeps file names with spaces whole ("My Files.desktop").
Chromium in QEMU now ends in a segmentation fault (139) instead of exit 191.

## 2026-10-07 — Two rm's: people's goes to the Trash, root's is GNU's

`/usr/bin/myos-rm` is people's rm (uid 1000-59999); `/bin/rm` and
`/usr/bin/rm` are symlinks to it, so `rm` by any name or path arrives there.
Root and system accounts are handed to GNU rm (`/usr/bin/coreutils` as
`rm`), with a guard: if coreutils turns out to be this program (the
2026-10-07 clobber), it says so instead of calling itself forever. Files in
the scratch folders (`/tmp`, `/var/tmp`, `/dev/shm`, `/run`) are deleted,
not kept: scripts clean up there all the time. GNU's options now work
(`-i`, `-I`, `-v`, `-d` empty folders only, `--interactive=`), unknown
ones are errors, messages read like GNU's, and the record in `._NAME.where`
is written before the move, so a file never lands in the Trash without it.
An apk commit hook re-points `/bin/rm` when a coreutils upgrade replaces it.
Because ours are symlinks, unpacking the tar replaces Alpine's link instead
of writing through it into coreutils (checked with a plain `extractall`).
33 host tests (scratchpad) pass; it builds for both CPUs. Not yet run on
MyOS itself: that needs the live image rebuilt.

The live user's Documents now has `Sound-test.wav` (`live/overlay/etc/skel`):
48 kHz 16-bit stereo, made with Python's `wave` module — 440 Hz in the left
speaker, then the right, then C-E-G and the C major chord in both, at
-10 dBFS. Nothing plays it yet; it is for docs/SOUND-PLAN.md S2 onward.

## 2026-10-07 — Sound plays (S1–S3 of docs/SOUND-PLAN.md)

The HD Audio controller driver (`drivers/sound/hda_intel.c`) resets the
controller, runs the CORB/RIRB command rings, finds the codecs, and plays and
records through DMA streams: one buffer descriptor list entry per period, an
MSI interrupt per period, the DMA position read from the position buffer.
The generic codec driver (`hda_codec.c`) walks the codec's widget graph from
each speaker, headphone and line-out pin back to a DAC, and from the ADC to
the mics, turns the paths on (power, selectors, amps, pins, EAPD), makes
Linux's controls (Master and Capture volume and switch, Mic Boost, Headphone
and Mic Jack) with dB scales, and moves sound between speaker and headphones
on the codec's jack messages. The sound core (`core.c`, `pcm.c`) is Linux's
ALSA interface: `/dev/snd/controlC0`, `pcmC0D0p`, `pcmC0D0c`, the control and
PCM ioctls, hw_params negotiation (our own interval arithmetic), read/write,
mmap of the buffer, poll. `include/sound/asound.h` matches Linux's header
exactly on both CPUs (`tools/check-asound.sh` compares 103 sizes, offsets and
ioctl numbers each). `make test-sound` plays 440 Hz in QEMU through write()
and through the mapped buffer, and finds the tone in what QEMU recorded; it
passes on i386 and x86_64. Bugs found on the way: QEMU's controller holds the
command ring until the response status is cleared (the RIRB interrupt is now
on from the start, as in Linux), and the VFS keeps device names by pointer.
The kernel's string library took over strncmp, strncpy, strlcpy, strchr and
the rest from the Linux shim. `/etc/asound.conf` sends `default` to the card
through `plug` (no dmix yet: docs/gaps.md).

## 2026-10-08 — Chromium runs (headless, in QEMU)

`chromium --headless --dump-dom` prints the page and exits 0. Six kernel bugs
stood between it and that, found one at a time by running it in QEMU over the
serial console:

- `recvmsg` on a message socket at end of file left its "truncated" flag
  unset (garbage): MSG_TRUNC on EOF, so the zygote spun on EMSGSIZE.
- `int3` from a program was a protection fault (SIGSEGV): the breakpoint gate
  is now ring 3 on both CPUs, so it is SIGTRAP, as on Linux (WORKLIST A14).
  Chromium's CHECK failures end in int3: that showed which ones failed.
- At most 1024 memory areas per process: Chromium's allocator splits its
  reservations with guard pages and needs more; `mprotect` said ENOMEM. Now
  Linux's 65536 (the array still grows only as needed).
- 64 open files per process: now 1024, Linux's default (WORKLIST A8). The table
  is built in for 64 and doubles on demand, like Linux's fdtable; close-on-exec
  is a bitmap beside it. `select` handles all 1024 fds (it stopped at 31).
- `stat()` through `/proc/self/fd/N` of a pipe or socket was ENXIO; it now
  describes the pipe or socket, as Linux does (`open()` still refuses).
- MAP_SHARED of a file was private. For files in RAM (memfd, /dev/shm) it now
  maps the file's own pages, so processes and read()/write() see the same
  bytes; MAP_SHARED|MAP_ANONYMOUS is an unnamed RAM file of its own. Disk files
  are still private (docs/gaps.md).

A GPU helper process still dies of a page fault and Chromium carries on
without it (software drawing). Not yet tried: the windowed browser on the MSI.

Later the same day: Chromium in a window. It crashed in its allocator while
Mesa parsed its XML settings: `madvise` was a no-op, and MADV_DONTNEED is a
promise (the pages go; the next touch reads zeros), which Chromium's
allocator builds on when it gives memory back and takes it again. Now
MADV_DONTNEED and MADV_FREE drop the pages (`mm_madvise`). With that, Chromium
opens its first-run window on our X display (QEMU screenshot). The Start
menu's "Web Browser", My Files and the desktop now start Chromium (they ran
Firefox, which does not start yet); `/etc/chromium/myos.conf` adds
`--no-sandbox --disable-gpu` to every start.

## 2026-10-08 — The installer's time zone map is the Earth

The time zone page drew three green blobs (copied from the mockup) with the
city dots placed by eye. It now draws the real coastlines: Natural Earth's
public-domain 1:110m land, made into a C table by `tools/make-worldmap.py`
(`live/myos-install/worldmap.h`: 125 outlines, 2956 points, about 12 KB;
cut off at 84°S, detail under 0.2° dropped). The projection is
equirectangular, so each city's dot is one multiply from its latitude and
longitude; the map keeps its shape at any window size.

The installer offered 21 hand-typed cities. It now reads tzdata's own list
when it starts (`/usr/share/zoneinfo/zone.tab`, on the live disk already):
all 418 zones of tzdata 2026b, each with its city's coordinates and its
country (`iso3166.tab`: "Kolkata, India"), so every inhabited country has a
city and every zone a dot; only Bouvet Island and Heard & McDonald Islands,
which nobody lives on, have no zone. Plus UTC. Region, then City; `--text`
asks the same two questions. A host test read the image's tzdata through
the installer's own code: no zone missing, every coordinate equal to
zone.tab's. Traditional Chinese now defaults to Asia/Taipei (it was Hong
Kong, for want of Taipei in the old list).

Bugs on the page went with it: a click on the map changed City but left
Region showing the old region; picking a language reset Region to America
(German gave New York); and the setuid helper put whatever the plan said
after `/usr/share/zoneinfo/` and copied that path as root, with only an
`access()` check (the real user's rights, racy) between a name with `..` in
it and other files. `plan_check` now takes only tzdata's characters (no
`.`). Builds for both CPUs, no warnings. Not yet seen in
the installer itself on MyOS (the live image needs rebuilding).

The Start menu's pinned "Web Browser" still ran Firefox: the left column is
made by live/theme/make-live-theme.py (MENU_LEFT), apart from the All Programs
entries. Now `chromium`; the overlay's firefox.desktop is called "Firefox"
again. Checked as the user does it, in QEMU: log in, Start, Web Browser
(clicks through QEMU's monitor): Chromium's window opens. A program killed by
a CPU exception now gets the file it was in, and the offset, in the kernel's
message ("in libfoo.so at file offset ..."), as Linux's "segfault ... in".

## 2026-10-08 — MPlayer on the 64-bit live disk

Alpine 3.20's `mplayer` (community) is in `live/packages.x86_64.txt` under
`[desktop]`. There is no i386 package. All Programs has Entertainment →
MPlayer; My Files opens the usual video and audio names with it.

## 2026-10-08 — LibreOffice on the 64-bit live disk

Alpine has no Apache OpenOffice package. The suite on the stick is
LibreOffice 7.6 (`libreoffice` + `libreoffice-gtk` in
`live/packages.x86_64.txt` [office]). The live root grew to 3072 MiB so
it fits. All Programs files Writer, Calc, Impress and the rest under
Office; My Files opens .odt/.ods/.odp/.xlsx/.pptx with `libreoffice`.

## 2026-10-08 — The network on the MSI: DNS, the route, loopback

"baidu.com can't be reached" was three things. /etc/resolv.conf was copied
from /proc/net/dns at early boot, and /proc/net/dns itself was filled in
when /proc was mounted: on the MSI both came before Wi-Fi's DHCP lease, so
neither ever named a nameserver. /proc/net/dns is now made when it is read,
from the kernel's nameserver of the moment, and /etc/resolv.conf is a link
to it (musl reads it afresh for every lookup). And UDP to 127.0.0.1 failed
(ENETUNREACH: no loopback at all): musl's AI_ADDRCONFIG test connects a UDP
socket to 127.0.0.1, took the failure for "no IPv4", and refused every
lookup with that flag — all of Chromium's. UDP to 127/8 is now delivered
locally (udp.c), and bind() to 127/8 works. New: /proc/net/route in Linux's
format (`route -n` shows the default route), and the kernel log names the
gateway and nameserver of a Wi-Fi lease. Chromium uses musl's lookup
(--disable-features=AsyncDns, /etc/chromium/myos.conf). In QEMU: musl's
AI_ADDRCONFIG lookup, a UDP echo over 127.0.0.1, and headless Chromium
loading https://example.com all work.


## 2026-10-08 — Two kernel crashes found, and the holes behind them closed

**Kernel stacks have guard pages.** First boot of the x86_64 live disk once
died in the write-back thread ("flush") on a pointer that was really the
start of an ext2 inode: another thread's kernel stack had run past its
8 KiB and written over the stack below it. Kernel stacks were kmalloc
blocks packed end to end, so an overflow was silent — and in a kernel,
whatever can be made to overflow a stack can write kernel memory with it.
Now (kernel/thread.c, "kernel stacks") every stack lives in a window of
the new KSTACK area (memlayout.h) with an unmapped guard page beneath it;
running off the end stops the CPU at the guard page, the double fault is
taken on a stack of its own (x86_64: IST1; i386: a task gate, gdt.c), and
the report says "KERNEL STACK OVERFLOW: thread X". The windows are a pool
(O(1) take and give back; pages kept, so the pages in use are the most
threads alive at once). x86_64 stacks are 16 KiB as on Linux; i386 stays 8.
`make STACKDEPTH=1` measures: first boot reached 5992 bytes (73% of the old
8 KiB) before Chromium even started. Tested by a temporary kernel thread
that recursed forever, on both CPUs: stopped at its guard page, named.

**The direction flag.** Starting the desktop then sometimes reset the
machine outright (a triple fault). With QEMU set to pause instead of reset,
the registers showed memcpy copying *backwards* (DF set) with a length of
-480 bytes, from the console's grid down over the whole kernel image and
its IDT. Two bugs: x86_64's isr.asm did not clear the direction flag on
entry (a program may have it set when an interrupt or fault arrives —
musl's memmove does — and then every kernel memcpy/memset runs backwards:
a way for any program to scribble on the kernel), and the console accepted
a screen with 0 rows, whose scroll() moved -1 rows. isr.asm now starts with
`cld` (i386's always did; syscall and sysenter clear it already), and a
screen with no room for a character detaches the console. systest checks
it: page faults taken with DF set must still zero-fill the page.

Also new, as hardening and for hunting: switching to or freeing a page
table checks that its kernel half is the kernel's (two compares; a freed
or overwritten table stops with "PAGE TABLE CHECK" instead of a reset), and
`make MEMDEBUG=1` fills freed pages and kmalloc objects with 0xDEADBEEF and
checks them when they are handed out again (Rodger's suggestion, after
Solaris's kmem debugging). Neither fired here; both stay for the next one.

The Web Browser launchers run `myos-browser` (Chromium if the disk has it,
else Firefox): this morning's `chromium` would have done nothing on the
32-bit disk, where Alpine has no Chromium.

## 2026-10-09 — Closer to Linux: /proc per process, disk files, 22 system calls

`tools/linux-audit.sh` found 86 things Linux programs use that MyOS lacked
(docs/LINUX-COMPAT.md). Fixed in one go:

**/proc.** One folder per process, `/proc/<pid>/` (stat, status, statm,
cmdline, comm, environ, maps, limits, mounts, mountinfo, cgroup,
oom_score_adj, io, wchan; `/proc/self/` has the same files for whoever
reads). The folder is made with the process and removed when it is
reaped, and every file is written out when it is read, in Linux's format
(52 fields in stat). So `ps`, `top`, `pgrep` and `pkill` have something
to read. Also uptime, loadavg, stat, mounts, partitions, filesystems,
swaps, devices, interrupts, net/{dev,tcp,udp,unix},
sys/kernel/{hostname,osrelease,ostype,pid_max,random/uuid,boot_id}, and
the rest of the small fixed files. Only the owner or root may read another
process's `maps`, as on Linux: it would give away where that program's
memory is.

**Disks as files.** `/dev/sda`, `/dev/sda2`, and so on (root:disk, 0660),
with Linux's device numbers. Each open keeps its own 64-bit position
(struct file's is 32-bit; mke2fs writes across a whole 32 GB partition).
Bytes go through a sector-sized bounce buffer. The ioctls disk tools use:
BLKGETSIZE64, BLKSSZGET, HDIO_GETGEO, BLKFLSBUF, and BLKRRPART, which
reads the table again and updates the partitions in place (one that is
gone gets 0 sectors, so every read or write of it fails). Safety rules:
- A mounted disk can't be opened for writing (EBUSY), and a disk open for
  writing can't be mounted.
- The block cache forgets the disk when a writer opens it and again when it
  closes.
- `mount -t ext2|vfat /dev/sdXN` works now (root only).
- `/sys/block` gained `dev` files, plus `/sys/dev/block` and
  `/sys/class/block` links.

**Security holes closed on the way:**
- `mount` had no root check. Any user could mount an empty tmpfs over
  `/etc`, write a `passwd` of their own there, and become root. It is now
  root only, as on Linux.
- `mknod` of a device node is root only too. Anyone could otherwise make a
  node for the raw disk in their own folder.

**/dev and /sys.** `/dev/stdin`, `stdout`, `stderr` and `fd` link into
`/proc/self/fd` (bash's `<(…)`), and `/dev/full` exists. In `/sys`:
- `class/net/<if>/` (address, operstate, carrier, mtu, …), read live;
- `firmware/efi` on the 64-bit kernel, so the installer knows it is a
  UEFI machine;
- `devices/system/cpu/cpu0`, `fs/cgroup`, and
  `kernel/mm/transparent_hugepage`.

`/etc/mtab` links to `/proc/self/mounts`.

**System calls** (x86_64 222 → 244 of Linux's 369):
- `close_range`, `waitid`, `preadv`/`pwritev` (and the "2" versions);
- `getcpu`, the `sched_*` policy calls (everyone is SCHED_OTHER;
  real-time gets EPERM, as for a normal user on Linux);
- `mlock*` and `msync` (no swap, and shared mappings are the file's own
  pages: nothing to do);
- `vhangup` on x86_64.

`wait4(0, …)` and `wait4(-pgid, …)` now wait for that process group, where
before they took any child.

Found while testing, and fixed:
- **Any program could crash the kernel by reading a write-only file.**
  `/proc/sysrq-trigger` was one; writing a read-only one like
  `/proc/uptime` did it too. `read`/`write` called the file's function
  without checking it existed. It answers EINVAL now, at no cost: the
  pointer is loaded for the call anyway. sshd found it through the new
  `preadv`.
- **Disk-file permissions.** The VFS doesn't check permissions yet
  (WORKLIST B30), so the disk files check their own mode bits. Until then,
  user `myos` could read the raw disk.
- `/proc/cmdline` is never empty. `ps` names a process after its main
  thread (Xorg was showing as "InputThread"). The load average counts
  program threads only (an idle machine read 2.00).

Checked on the 64-bit live disk in QEMU:
- `tools/linux-audit.sh`: **85 present, 25 missing** (was 24 / 86).
- `ps -e`, `top -b`, `uptime`, `free`, `df`, `mount` and `lsblk` all work.
- `mount` as `myos`: "must be superuser".
- `/proc/1/maps` as `myos`: permission denied.

i386 `make test`: 215/215.

## 2026-10-09 — "LibreOffice froze": the X server was moving 19 MB, 4 KiB at a time

In UTM, LibreOffice seemed to hang while the OS kept running. QEMU's
monitor (read-only `info registers`, plus the kernel's `current` thread
name read with `x`) showed **Xorg**, not LibreOffice, inside musl's memcpy.
Every sample had RSI = RDI + 4096, over a buffer of about 19 MB.

That is X's FlushClient. After a partial non-blocking write to a client, it
moves all the unsent rest to the front of its buffer. Our Unix stream
sockets held one page, so every write was 4 KiB, and a 19 MB reply became
about 4,700 moves of up to 19 MB each: tens of GB of copying, minutes under
emulation. Linux lets about 208 KiB wait in a Unix socket.

The fix: a stream socket's ring starts at one page and **doubles while a
writer finds it full, up to 256 KiB** (net/unix.c, ring_grow). Quiet sockets
(most of them) keep their page. It never grows while a copy is in or out of
the ring, because a copy can sleep on a page fault holding a pointer into
it. Bytes keep their absolute positions, which files in flight are pinned
to. systest checks it: 3 MB through a socketpair in 100,003-byte writes,
with a child checking every byte. That took 52 ms through the old 4 KiB
ring and 23 ms now (i386, QEMU). For X's case the moves drop 64-fold. i386
`make test` 215/215.

## 2026-10-09 — "Save As" hung LibreOffice: libmount's edge-triggered epoll

LibreOffice's Save As dialog stopped answering ("Kill Client?"). The VM's
monitor showed a thread named `gmain` (GLib's worker) going around
`epoll_pwait` at 100% CPU. `gio mount -o` reproduced it with no GUI:
15.7 s of CPU in 16 s. A temporary syscall trace (taken out again) showed
the cause. libmount's mount monitor, which GLib uses for every file
dialog, opens `/proc/self/mountinfo` and adds it to an epoll with
**EPOLLIN | EPOLLET**. That file is always readable. On Linux an
edge-triggered entry fires only when the mount table changes. Our epoll
treats EPOLLET as level-triggered, so it said "changed" on every call,
and libmount re-read the table forever. `/proc/self/mountinfo` only
appeared this morning, so this was new.

Fixes:
- **Edge-triggered epoll for change-count files** (new
  `file_ops.change_count`, used only by the mount tables for now): an edge
  is a change of the count. Pipes and sockets keep the level behavior
  their readers expect.
- **`poll()` on an epoll file** reports readable only while an entry is
  ready. Before, it always said readable.
- **The mount tables are pollable as on Linux**: always readable, plus
  POLLPRI | POLLERR once after the table changed. Mounting bumps a count
  and wakes pollers.
- **O_TRUNC is ignored on the kernel's own files.** `echo t >
  /proc/sysrq-trigger` failed with EINVAL, because bash opens with
  O_TRUNC.

After: `gio mount -o` uses 0.05 s of CPU in 16 s. systest checks the
epoll poll. i386 `make test` 215/215.

## 2026-10-09 — Chromium's crash: caught, not yet fixed

Reproduced in QEMU: `chromium --no-first-run https://example.com` with
networking dies about 2 s in, every time with today's 256 KiB socket rings.
Headless, and the first-run page without networking, survive. The browser
process faults in a C++ virtual call on a garbage vtable pointer (an object
freed and reused). Two runs faulted in different places. One bad address
looked like a pointer with bit 44 set, which is how PartitionAlloc
scrambles its free-list pointers. Chromium's crash reporter catches the
fault and kills the process itself, so nothing was logged.

Bisected: with the socket ring held at 4 KiB, one run of two survived. The
bigger ring makes the crash more frequent but didn't cause it. It is the
intermittent crash seen before. `madvise(MADV_DONTNEED)` already zeroes
pages as Chromium expects. Still open.

On the way:
- x86_64 now logs a fault that the program's own handler catches, like
  i386 does (first 50 per boot). The log shows the thread, pid, RIP,
  address, `in <file> at file offset`, and sp.
- `/proc/<pid>/oom_score_adj` takes writes (0644), as on Linux. Chromium
  sets it for every renderer.

## 2026-10-09 — Fonts for every language, and tests that click where they mean

**Fonts.** The installer's first page showed 中文 and 日本語 as boxes: the
live disks had DejaVu, Open Sans and bitmap fonts, none with a CJK letter.
A new `[fonts]` group in live/packages.txt brings Noto Sans CJK and Noto
Serif CJK (font-noto-cjk, 93 MB, regular and bold; Simplified and
Traditional Chinese, Japanese and Korean shapes, chosen by the text's
language) and Open Sans (3 MB, the free stand-in for the mockups' Trebuchet
MS headings; the 64-bit disk had it only through Chromium). Not in the RAM
login image. Pinned in both locks with `--add`; the 32-bit lock had fallen
behind packages.txt (group moves, five Python extras) and caught up with no
version changed. The 64-bit disk is 2237 MiB of 3072.

**xdotool, and the desktop tests over ssh.** tools/qemu-desktop.py drove a
PS/2 mouse: relative moves that X speeds up, so `moveto` crept 4 px at a
time and checked screenshots, and `type` knew letters, digits and '-'. Now:
- `type` sends any text tools/qemu-type.sh has a key for;
- `sshkey` types a one-time public key into the open terminal and waits for
  sshd; then `sh CMD` runs a command in the machine, `xdo ARGS` runs
  xdotool on `:0` (in [desktop], so in the login image too), `text TEXT`
  types any characters, and `get PATH` copies a file out;
- a script that uses them gets an e1000 on QEMU's user network with ssh
  forwarded to a free port; the serial and monitor ports are free ones too;
- the disk is opened with snapshot=on, so a run never writes the image (the
  test key can't end up on a stick).

`xdotool type` drops every non-ASCII character here, even slowed down or
with LC_ALL=C.UTF-8, while `xdotool key udiaeresis` works: so `text` sends
ASCII runs with `type` and every other character as its Unicode keysym
(`key U4E16`), 200 ms apart. Without the pause, xterm read some keys before
it saw the spare key code's new mapping and lost them.

Checked on the 64-bit live disk in QEMU: `xdo mousemove 300 205 click 1`
selected Deutsch in the installer's list in one step;
`text echo "Grüße, 世界! It's 5 > 4 & x=$((2+3))" > /tmp/t8` arrived exactly
(bash evaluated the 5); `xdo search --name 'Install MyOS'
getwindowgeometry` gave the window; `get` brought files out.

Found on the way: the X server has no access control (no `-auth`), so any
local program can read the desktop's keys. In docs/gaps.md, with the fix.

## 2026-10-09 — Hunting Chromium's crash: where it is, what it isn't

Where it is: the call chain (new: the fault log lists return addresses on
the user stack as file + offset) goes through Chromium's History database
(`CREATE TABLE visits`), sql::Database turning on SQLite's memory-mapped
I/O, SQLite B-tree code, and finally Chromium's SQLite VFS wrapper calling
`xFetch` (slot 0x88 of sqlite3_io_methods) through a file whose method
pointer is garbage. A freed or overwritten SQLite file structure.

How it behaves: the address is the same every time (MyOS has no
randomisation), but whether it happens is timing. It crashed nearly every
time on the normal kernel and 2 of 3 under MEMDEBUG. It did not crash in
4 runs under SYSTRACE, 3 under PTAUDIT, or with Chromium's own logging on.

Ruled out, with evidence:
- FS-base/TLS switching: the cache is right on every path.
- Global TLB entries on user pages: global_for() gives the bit only to
  kernel addresses, and the software PTE bits are 9–11.
- CR0.WP: it is set.
- The FPU save area: 512 bytes, FXSAVE.
- madvise: DONTNEED zeroes.
- System calls returning with interrupts on (the red-zone window before
  sysret): a check on every return never fired.
- Repeated randomness: boot_id and urandom differ every boot.
- Inode numbers reused while a deleted file is still open: they aren't.
- Pages written after being freed: MEMDEBUG reported nothing.

The `MAP_SHARED` theory looked confirmed by two clean runs, then failed on
the next two. Its fix stays anyway, because the bug it closes is real: a
disk file opened for writing can no longer be mapped shared (ENODEV). Its
"shared" map was a private copy that read stale pages and dropped writes.

New debug builds: `make SYSTRACE=1` (each thread's last 32 system calls,
printed when it faults) and `make PTAUDIT=1` (registers, the memory they
point at, and every page table checked against the allocator when a
program faults).

Small differences found:
- A deleted file that is still open reports 1 link; Linux reports 0.

## 2026-10-09 — Workspace buttons on the taskbar, for good this time

They came back because of our own program. myos-desktop saved the
wallpaper for IceWM's background helper as a `~/.icewm/preferences`
holding only the three wallpaper lines. IceWM reads the *first*
preferences file it finds, and `~/.icewm` comes before `/etc/icewm`. So
once a wallpaper had been set, every MyOS IceWM setting was gone,
`TaskBarShowWorkspaces=0` included, and IceWM's defaults put the workspace
buttons back.

Fixes:
- myos-desktop now builds the user's file from `/etc/icewm/preferences`
  at every login, swapping in only the wallpaper lines.
- The new `/etc/icewm/prefoverride` forces `TaskBarShowWorkspaces=0`.
  IceWM applies it after the preferences and the theme.

Checked in QEMU: logged in graphically with a hostile user file
(`TaskBarShowWorkspaces=1`) already in place. The taskbar showed no
workspace buttons, and the user file was rebuilt from the system file
(52 lines).

The workspaces themselves are still there, for Mission Control above the
screen later. The one remaining hole is a `~/.icewm/prefoverride`, which
would shadow the system one. Closing that needs a patched IceWM (its
source, 3.5.0, isn't in the tree).

## 2026-10-09 — "WP's .docx looks corrupted": AbiWord can't read any .docx here

A `.docx` opened from the desktop showed as `PK????…`, 43 pages, in
AbiWord. The file itself (copied out of the VM over the serial console)
was valid: 10 parts, every CRC good, written by LibreOffice 7.6, text
"test". WP's own `.docx` output (wp51 `tests/out`: font, plain, round,
style) is valid too, and macOS's textutil reads every one correctly. (The
other four files there are minimal inputs the tests write for WP's
importer.)

The cause: Alpine's AbiWord on our disks has only its OpenDocument plugin,
no Word importer. It loads any `.docx` as plain text. The desktop and My
Files both sent `.doc`/`.docx`/`.odt`/`.rtf` straight to `abiword`. They
now run `myos-office`, a launcher like `myos-browser`: LibreOffice where
the disk has it (64-bit), else AbiWord. The 32-bit disk has no LibreOffice,
so `.docx` there still needs a Word importer for AbiWord.

## 2026-10-09 — security holes, written down

`docs/SECURITY-HOLES.md` is the open list: no permission checks, empty
live passwords that accept anything, X with no cookie, unsandboxed
Chromium, no NX/ASLR/`PROT_NONE`, raw ICMP for every user, USB auto-mount,
and what is still open on the network after the work in
`docs/security.md`. Not a claim the machine is safe. A map of where it
is not.

## 2026-10-09 — The MSI's SSD was invisible: PCI stopped at bus 0

On the MSI (over SSH, 192.168.1.69), MyOS listed only bus-0 PCI devices.
No NVMe device of class 010802 at all, and only the USB stick in
`/proc/partitions`. Intel's RAID mode (VMD) is off and the SATA controller
is in AHCI mode, so the firmware wasn't hiding the disk. drivers/pci.c
said it plainly: "We walk bus 0 only". The SSD, the Ethernet chip and the
NVIDIA GPU sit behind PCI Express root ports (00:01.0, 00:1d.0, 00:1d.4),
on buses the firmware has already numbered.

pci_init now follows every PCI-to-PCI bridge: register 0x18 gives the
secondary and subordinate bus. Each bus is walked before the buses behind
it, so bus 0 keeps its old order. A bridge with devices behind it gets
memory, I/O and bus-master on, so their DMA is forwarded. Each device
points at its own struct pci_bus (number, bridge, parent). The table
holds 64 devices, up from 32.

Checked in QEMU, with an NVMe behind a pcie-root-port (OVMF numbers the
bus as a PC's firmware does): "pci: bridge 00:04.0 -> bus 01-01", then
nvme0n1 with MSI-X. i386 `make test` 215/215. Next: the MSI itself.

## 2026-10-09 — The installer can't pick the disk it runs from

The disk list used to let you select the live disk and only complained
after Next. It also called *every* removable disk "the stick", so a
second USB disk could never be the target. Now:
- A disk is "in use" when anything is mounted from it (`/proc/mounts`:
  the live "/", a boot partition, /data). That also covers UTM's live
  disk, which isn't removable.
- Its row says "MyOS is running from this disk" and is faded, not
  selectable and inactive, with a tooltip saying why.
- Text mode marks it `[can't choose]` and asks again if picked.
- The install helper still refuses it, as a last check.

The old `removable` guess remains only if `/proc/mounts` can't be read.

Checked in QEMU: in the GUI, clicking the live disk's row leaves the
selection on the empty disk. In text mode, choosing it re-asks.

## 2026-10-09 — The MSI's SSD shows up; the installer names it right

On the MSI with the bridge-following kernel: GPU 01:00.0, Kingston NVMe
02:00.0 (2646:500c), Realtek 03:00.0. nvme0n1 is 488 GB with MSI-X, and
its partitions are EFI, "recovery", a large Linux one and swap: Pop!_OS.
The installer listed it, but only as `/dev/nvme0n1`, "Windows likely".
Next to "USB DISK 3.2" (the stick) it didn't look like the main disk.

- NVMe: the driver now asks the controller for its model (Identify
  Controller, CNS 1, bytes 24–63), so `/sys/block/nvme0n1/device/model`,
  the boot log and the installer show the drive's name.
- The installer says "Windows likely" only when a partition carries
  Windows' own names ("Basic data partition", "Microsoft reserved");
  otherwise an EFI partition plus a large one is "another system is
  installed". Either way the EFI partition is kept (has_system).

QEMU: "QEMU NVMe Ctrl (NVMe)" in the installer's list. i386 `make test`
215/215.

## 2026-10-09 — "MSI" is a computer name

The installer refused "MSI": plan_good_hostname took only lowercase
letters, while its message said "letters". Host names are case-insensitive
(RFC 1123) and Linux keeps them as typed, so ASCII letters of either case,
digits and - are accepted now; still not starting or ending with -, and
nothing else (spaces, _, non-ASCII). Checked on the Mac: MSI, Rodgers-MSI
ok; -MSI, MSI-, "MS I", MSI_1, empty refused.

## 2026-10-09 — Installer: drive pictures, a big check, the partition manager

- **Chinese fonts are always installed; the question is gone** (text and
  GUI). `font-noto-cjk` is on both live disks and installs copy the system.
  The "and pinyin" in the summary went too: no input method is on the disk.
- **The disk picker:**
  - Each disk has a picture (live/overlay/usr/share/myos/icons/installer:
    a drive, or a thumb drive for a removable disk), its name in bold,
    size and state, and a big green check on the chosen one.
  - The suggestion is the first internal disk not in use, never a USB stick
    when there is anything else.
- **The Partitions page follows docs/installer/partition-manager-mockup.jpg:**
  - The disk's picture and name.
  - Before and After bars. After is tall named blocks: EFI slate, Windows
    blue, unallocated hatched, myos-root orange, myos-data green, and no
    block narrower than a readable minimum.
  - A table (Partition, Type, Size, Mount, Action) with the selected row
    teal.
  - Tools: Resize (the system's size, in a popover), Delete and Add
    partition (the separate myos-data), and Format (off: new partitions are
    always formatted).
  - The note "Nothing is written until you click Install".
  - The layout follows the helper's own rules.
- **Shared with the other pages:** circled steps in the sidebar with the
  current one on a teal pill, a teal Next, and the page in the window title.
- **Fixes on the way:**
  - Partitions are listed in disk order (`readdir` order put the MSI's p2
    first).
  - Windows' "Basic data partition" shows as "Windows (C:)".
  - The installer now links gdk-pixbuf and pango.

Checked in QEMU with a 64 GB GPT test disk (EFI 260 MiB, Windows 24 GiB,
free space): EFI kept, Windows kept, myos-root 32 GB, myos-data 11 GB.

## 2026-10-09 — The installer's format failed: disk writes were short

On the MSI the install stopped at formatting. In QEMU, `mke2fs` on a disk
file failed at once: "Could not write 509 blocks in inode table ... short
write". It writes 2 MB at a time. A disk file took 64 KiB per call, and
every read or write was cut to RW_MAX (256 KiB). Linux never writes a
disk short, and e2fsprogs counts a short write as an error.

Now a disk file does the whole request (up to Linux's MAX_RW_COUNT), 64
KiB through the bounce buffer at a time. rw_at sends block devices
straight there: no RW_MAX, no fault-in first, because their copies sit
between disk transfers that sleep anyway. On the read/write path that is
one compare of the inode's mode.

QEMU: `mke2fs -F` of a 24 GB partition completes in 2.2 s (192 groups).

Also: Resize on the Partitions page has "Use all (N GB)", which gives
myos-root all of MyOS's space with no separate myos-data. The size field
can't ask for more than the disk has.

## 2026-10-10 — The installer installs: erase a disk, boot it, log in

On the MSI the install had spun forever. The helper's steps were mostly
log lines, and some real steps were wrong. Erase-with-EFI-kept wrote a
table with no EFI partition at all. Partitions were found by guessing
their numbers, and the EFI partition was to be made with `mkfs.vfat`,
which the live disk doesn't have. Now `myos-install-helper --apply` does
the whole job:

1. **partition**: sfdisk writes a new GPT ("EFI System" 512 MiB,
   myos-root, myos-data). Free-space installs append the same three
   (`--append`), only on a disk with no EFI partition yet (MyOS can't
   write FAT, so it can't add itself to Windows' EFI partition; the
   window says so). BLKRRPART must succeed, and the new partitions are
   found by GPT name among the ones that weren't there before.
2. **filesystems**: mke2fs with the features our ext2 reads, and fewer
   inodes than mke2fs's default (they are all zeroed).
3. **boot**: `myos-mkesp` (new, Python, in /usr/libexec) writes FAT32
   straight onto the new EFI partition. It holds a copy of the stick's
   own EFI partition, mounted read-only. The firmware starts
   EFI/BOOT/BOOTX64.EFI by itself, so no boot entry is needed.
4. **copy**: myos-root at /mnt/myos-root, myos-data at its /data, then
   `rsync -ax` of the running system. Left out: the kernel's file
   systems, temporary files, /mnt and /home.
5. **configure**: the account is written into passwd, group and shadow
   (sha512-crypt, uid 1000, the administrator's groups). Root is
   locked, and every live account goes (`myos` has no password). /home
   is a link to /data/home. Removed: the stick's ssh host key (every
   install would share it), the build Mac's key, ssh login itself (off
   by default), the installer, and the Wi-Fi passwords unless asked.
   Then the computer name, language, keyboard and time zone.
6. **finish**: sync (it flushes the drives' own caches too).

The helper is setuid root, so it was also a way in. It ran sfdisk,
rsync and python with the caller's environment, so LD_PRELOAD would have
run their code as root. Any user could erase any disk. The plan and the
log in /tmp could be links to root's files. Now the environment is
cleared, only root or wheel may run it, and the plan must be the
caller's own: it is checked, then read from the same open file. The log
is made fresh with O_EXCL. The window writes the plan 0600 from the
start, with O_EXCL. The language and keyboard names are checked too,
because they go into a shell script every login runs.

**The FAT driver read a file in O(n²).** Each 512-byte piece of a
read() walked the cluster chain from the file's start. The stick's EFI
partition has 512-byte clusters, so copying its 240 MB INITRD.CPIO ran
at about 4 MB a minute. fat32_read now continues from where the last
read of the same file stopped, and reads runs of adjacent clusters in
one disk command (up to 64 KiB). read() goes through a 64 KiB buffer. The
FAT-sector cache was also shared unsafely between threads; it is now
read and changed only with interrupts off. QEMU: the whole EFI step
takes 3 s (docs/performance.md).

**The kernel picks the right root.** With the stick still plugged in,
both disks hold a "myos-root". The one on the kind of disk we started
from wins (USB when BOOT_FROM_USB, otherwise not USB). myos-data is
taken only from the root's own disk, so a live stick never mounts an
installed system's files.

The window: it stopped spinning when the helper finishes (a closed
pipe's watch returned TRUE forever). Erase always makes a new EFI
partition. "Free space" explains why it can't be used: the disk has an
EFI partition, or not enough room (it counted 0 bytes free as enough).
The work list follows the helper's order. A disk mounted by an earlier
try says "restart the computer"; it no longer says "MyOS is running from
it".

Checked in QEMU, live image as a USB stick plus a 64 GB NVMe:
`--apply` in 91 s (partition 1 s, file systems 1 s, EFI 3 s, copy 85 s
for 3.4 GB). Booted from the NVMe alone: myos-root at /, myos-data at
/data, login as the new account with its password, home on myos-data,
no live user, no ssh keys, `doas` asks for the password. Booted from the
stick with that NVMe attached: the stick's root, NVMe left alone.

## 2026-10-10 — Kernel configuration: the design, three mockups, a reference

The new **Settings › Kernel** screen (docs/KERNEL-CONFIG.md):
- A slider picks one of three modes. **Realtime** has exact slots and no
  power management. **Soft realtime** has preferred slots, and idles only
  in states that wake within a latency budget. **Max power savings** has
  no slots, and switches for every saving this processor and these
  devices have.
- In the two real-time modes, each of the 20 slots has a name field.
  Slots 0 and 10 are always the OS, so it runs exactly every 10 ms; the
  kernel's 1-based slots 1 and 10 are moving to this numbering.
- Save checks everything, then encodes one 512-byte block in the target
  processor's byte order and writes it to a `myos-config` partition (two
  copies, generation and CRC-32, not encrypted). The kernel finds the
  block through the partition table and reads it in `storage_init()`.

Three mockups were drawn (docs/kernel-config/mockups.html, PNGs from
headless Chrome): A timeline and list, B clock dial, C engineer's table
with the live block. **Rodger chose B**: the slots as a wheel, the power
savings as a grid of cards. A and C were deleted; the trimmed page draws B
exactly as before (the re-rendered PNGs differ by 0 pixels), and a
Realtime picture of B was added. Documentation that runs:
`tools/kconfig/kconfig.py` (layout, checks, little/big-endian encode and
decode, the A/B rule, the C struct) and `tools/kconfig/powerfeatures.py`
(every saving, per processor family). The generated struct compiles to
the documented offsets with both cross compilers.

## 2026-10-10 — Settings › Kernel, for real, on the x86_64 live disk

- **The page** (`live/myos-settings/myos-settings.c`, mockup B):
  - the three-stop slider;
  - the 20 ms wheel with a name field beside each slot;
  - a task list: program, arguments, the user it runs as, start at boot,
    restart if it stops. A new name gets `/bin/<name>`;
  - the latency budget;
  - in power savings, a grid of cards for this computer, each with its
    settings inside once switched on;
  - live checks in the footer;
  - **Save…**, with an administrator's password.
- **`kconfig.c`** is shared by the page and the helper: the text form,
  the checks, the two parts' bytes, the A/B partition write. The host
  test gives the same bytes as `kconfig.py`, little- and big-endian; the
  power-saving table is generated from `powerfeatures.py --c` by the
  build.
- **`myos-kconfig`** (setuid root, `/usr/libexec`): `read`, and `save`.
  Save checks the password with PAM, as `myos-adduser` does, and checks
  the config again, the programs' files too. It then writes the copy not
  in use (task list, flush, boot block, flush) and reads it back.
- **`tools/mkuefi.py --config-mb 1`** adds the `myos-config` partition,
  with its own type GUID, 9390AFB2-C545-487E-AF50-3648E56B736C. The
  first try showed the kernel reads only the MBR of these hybrid-MBR
  images, so the partition is also MBR entry 3 (type 0xDA). The helper
  then finds it through the GPT and its first sector.

Checked in QEMU (q35 AHCI, as UTM):
- read gave "nothing saved yet";
- Save went to copy A (generation 1), and again to copy B (generation 2);
- read gave the config back;
- a wrong password and a task with no program were both refused.

The partition QEMU wrote decodes with `kconfig.py` on the Mac to the same
config. A screenshot of the page went into docs/kernel-config/. The UTM
machine `~/Desktop/MyOS 64-bit.utm` got the new boot disk (its install
disk kept).

The kernel in that image is the 07:20 build. At 13:30 another session
was editing kernel files (`kprintf`, `xhci`, `partition.c`, `main.c` and
more), and `xhci.c` didn't compile then. The image was assembled from the
new userland and the last good kernel, without touching their work.

New: **docs/TIME-SLOTS.md**:
- how a program is bound to its slots (`myos-taskd`, then the check at
  `execve`: by file, user, and one process per task);
- how the OS slots run the kernel and the desktop. OS threads go first
  everywhere and run at once on wake-up, and a fence stops a runaway
  among them. A runaway program can't freeze the GUI: a click reaches X
  within microseconds, or at most 9 ms with realtime tasks;
- how to write a program for a slot.

## 2026-10-10 — Settings › Kernel on the MSI: power savings by CPUID

On the MSI (i7-11800H) most power-saving cards were greyed out. Settings
knew processors only by name (Pentium 4, Pentium M, Ryzen), so an Intel
Core was "not known". Three more were greyed because a program can't see
them ("MyOS can't tell yet").

Now:
- **The processor's own bits** (CPUID, read by Settings; MyOS's
  `/proc/cpuinfo` lists no `est`, `acpi` or `tm`):
  - SpeedStep or CPPC means it can change speed;
  - AMD CPB or Intel Turbo means boost;
  - MWAIT means deeper idle;
  - ACPI + TM means clock modulation;
  - the hypervisor bit means a virtual machine: the page says the host
    manages the processor's power.
- **What only the kernel can confirm** (deep idle, C1E, PCIe ASPM, USB,
  SATA link power, disk spin-down) is switchable, marked "checked when
  MyOS starts".
- **USB sticks** (removable) no longer count as hard disks or SATA links.
- `myos-kconfig detect` prints what Settings sees, as the caller (no
  root).

On the MSI, over ssh: 10 of 17 savings are switchable. The rest are
right to be off:
- two need SMP;
- there is no spinning disk and no SATA disk;
- MyOS has no sound driver for this machine yet;
- there is no Ethernet driver yet;
- there is no graphics driver with power management.

The new stick image keeps the kernel the first one had (the 13:36 build).

## 2026-10-10 — The touchpad plan

docs/TOUCHPAD-PLAN.md: two-finger scrolling and palm rejection for the
MSI's I2C HID touchpad, which MyOS can't see at all yet. The kernel will
deliver every finger, with palms marked, through this chain:
1. the ACPI tables, to find the pad (no AML yet: a per-machine table
   from the MSI's decompiled DSDT);
2. the LPSS DesignWare I2C controller;
3. HID over I2C, a general HID parser, and precision-touchpad mode;
4. polling every 8 ms, then a GPIO interrupt;
5. Linux's multitouch protocol on `/dev/input/event2`.

libinput (with `libudev-zero`, since MyOS has no udev) then gives the Mac
feel: smooth natural scrolling, tapping, and palm rejection in four
layers. Those are the pad's own confidence bit, ignoring the pad while
typing, edge zones, and size. Seven steps, about 2,000–3,000 lines of
kernel C. WORKLIST B52 points to it.

- 2026-10-11: Roadmap: step **14** is now ARM64 (QEMU `virt`, then Raspberry Pi bare metal);
  new step **15** is **Apple Silicon bare metal** (MacBook Air M1+ as MyOS-only, boot chain,
  Apple NVMe/input/display, aarch64 live userland). `CLAUDE.md` lists three CPU families.
  UTM/x86 on a Mac stays dev workflow, not step 15.
- 2026-10-11: **ARM64 hello** (`ARCH=arm64`): `arch/aarch64/entry.S`, `early.c`, `linker.ld`;
  `make ARCH=arm64 test` boots `qemu-system-aarch64 -M virt` and prints on PL011. Toolchain:
  `brew install aarch64-elf-gcc`. Gotchas: AAPCS64 requires 16-byte-aligned SP before `bl`;
  QEMU 11 PL011 needs CR UARTEN|TXE before TX; do not combine `-nographic` with a second
  `-serial file:` (empty log). M1 bare metal still needs m1n1 hand-off (step 15).
- 2026-10-11: **ARM64 step 14a (started):** PL011 `uart.c`, embedded `virt.dtb`, minimal `fdt.c`,
  EL1 `vectors.S` + sync fault print, kernel @ 0x40100000. `make ARCH=arm64 test` checks FDT
  magic and RAM line. Next: fix FDT /memory walk, PMM, MMU, then syscalls toward BusyBox.
- 2026-10-11: **ARM64 step 14a (FDT + PMM):** Host `test-arm64-fdt` parses virt `/memory` reg;
  `arm64_pmm_init` bitmap (2 GiB direct-map cap for RAM @ 0x40000000). Fixes: FDT 4-byte align
  with `~(uintptr_t)3` (host mmap >4 GiB); unaligned-safe `be64`; `__kernel_end` in linker.ld.
  `make ARCH=arm64 test` — FDT, RAM, PMM lines + hello. Next: EL1 MMU (map kernel, UART, RAM).
- 2026-10-11: **ARM64 MMU (14a):** `paging.c` + `mmu.S` — TTBR0, TCR/MAIR, 1 GiB block identity
  map (GiB0 MMIO, GiB1 RAM). `make ARCH=arm64 test` greps `MMU: on`. Gotcha: TG0 must be `01`
  (4 KiB); L2-only tables failed — 1 GiB L1 blocks work. Next: GIC, timer, kheap, syscalls.
- 2026-10-11: **ARM64 GIC + timer (14a):** GICv2 (virt DTB), CNTP_EL0 1 ms tick, `irq_el1` at
  vector +0x280 (was `hang`). Test waits for 5 ticks, prints `Timer: 5 ms ticks`. Next: kheap,
  EL0, syscalls toward BusyBox.
- 2026-10-11: **ARM64 kheap + kprintf (14a):** `arm64_pmm_alloc/free`, slab `kmalloc` (identity
  P2V), `kprintf`/`console_write` on UART. Test greps `kheap: kmalloc`. Next: EL0, syscalls, BusyBox.
- 2026-10-11: **ARM64 EL0 + SVC (14a→14b):** user stub, sync_el0 @ 0x400, irq_el0, preserve
  x0/x8 on entry; `EL0 svc: ping` test. Gotcha: timer IRQ @ 0x480 was `hang`; boot cannot run
  after `eret` without EL1 resume. Next: real Linux syscalls + musl `write`.
- 2026-10-11: **ARM64 Linux write (14b start):** `__NR_write` (64), `uaccess`, user image @
  0x40200000 (`.user.text`/`.user.rodata`), RAM L1 PTE `AP` for EL0. Fixed sync_el0 to lay out
  x0–x5 and x8 in `el0_regs` (ping only patched x0/x8). `make ARCH=arm64 test` greps
  `user write: hi from EL0`. Next: musl static hello, ELF64 exec, initramfs + BusyBox.
- 2026-10-11: **ARM64 musl hello (14b):** `fetch-alpine-arm64-musl.sh`, `user/arm64/musl-elf.ld`
  (link @ 0x40400000), `elf64_load.c`, embedded `hello.elf`. Static binary built with Alpine
  `libc.a`; `_start` uses `write` syscall until EL0 data faults for musl crt are fixed (gaps.md).
  Test greps `musl hello from MyOS`. Next: EL0 data PTE, initramfs, BusyBox.
- 2026-10-11: **ARM64 EL0 data + musl crt:** 4K L0→L1→L2 (kernel 2 MiB EL1-only, user AP bit 6
  only — bits 6+7 is RO). PAN/SPAN cleared. SVC skips the `svc` insn and saves x0–x30 in
  order (return value was clobbering LR). CPACR FPEN for musl SIMD. `make ARCH=arm64 test`
  greps `EL0: data access ok` and `musl hello from MyOS`. Next: initramfs + `/init`, BusyBox.
- 2026-10-11: **ARM64 initramfs + exec `/init`:** newc walker (`arch/aarch64/cpio.c`), host
  `test-arm64-cpio`, `pack-arm64-initrd.py` packs musl `hello.elf` as `/init`. Boot finds
  the archive after the EL0 data test and ELF-loads it (no VFS). `make ARCH=arm64 test`
  greps `initramfs: /init` and `musl hello from MyOS`. Next: Alpine BusyBox as `/init`.
- 2026-10-11: **ARM64 BusyBox echo:** Alpine `busybox.static` 1.36.1 (ET_EXEC @ 0x400000)
  packed as `/init`. VA 0x400000 aliased onto RAM (GiB 0 is L2, not a 1 GiB device block).
  First-wave syscalls (`mmap`/`writev`/`ioctl`/ids). Fixes: IRQ must save ELR/SPSR (do not
  `daifclr` before `eret`); SVC ELR is already the next insn; argv strings must sit above
  SP. `make ARCH=arm64 test` greps `busybox from MyOS`. Next: ash + fork/exec/pipes.
- 2026-10-11: **ARM64 BusyBox cat:** tiny fd table over the newc archive (fds 0–2
  console; ≥3 point at cpio bytes). `openat`/`read`/`close`/`lseek`/`fstat`/
  `newfstatat`/`faccessat`/`sendfile`; file-backed `mmap` copies into the bump
  arena. Packer adds `hello.txt`. `/init` argv is `cat /hello.txt`.
  `make ARCH=arm64 test` greps `cat from initramfs`. Next: ash + fork/exec/wait/pipes.
- 2026-10-11: **ARM64 bash on UTM virt:** process table (`clone`/`vfork`/`execve`/
  `wait4`/pipes), per-process page tables (eager copy), shebang + PT_INTERP,
  unique `st_ino` so musl does not skip libraries. `/init` runs ash then
  `exec /bin/bash -i`. `make ARCH=arm64 test` greps `bash-5.2`.
  `make ARCH=arm64 utm-arm64` builds `build/arm64/MyOS-ARM64.utm`.
- 2026-10-11: **Apple Silicon 15a:** `docs/hardware-apple-silicon.md` (M1 Air first,
  m1n1/Asahi boot path, constraints). `arch/aarch64/apple/machine.c` reads root
  `compatible`; serial banner `MyOS aarch64 (QEMU virt)` on virt.
  `make ARCH=arm64 test-arm64-apple` checks virt + `m1-min.dtb` fixture. Next: 15b m1n1 hand-off.
- 2026-10-11: **Apple 15b (DTB UART):** `fdt_mmio_by_compatible`, `uart_early.c` sets PL011
  base from `apple,uart` reg. `m1-qemu-harness.dtb` boots in QEMU with Apple banner + bash.
  `make ARCH=arm64 test-arm64-apple-boot`. Next: m1n1 on real M1 hardware DTB.
- 2026-10-11: **Apple 15b m1n1 payload:** `tools/mk-m1n1-payload.sh`,
  `arm64_boot_report()` (x0 vs embedded DTB). `make ARCH=arm64 test-arm64-m1n1-payload`.
  Next: chain-load on real M1 with hardware DTB in x0.
- 2026-10-11: **Apple 15b m1n1 x0 sim:** `kernel-m1n1-sim.elf` sets x0 to harness DTB in
  `_start` (`M1N1_SIMULATE_X0`); `make ARCH=arm64 test-arm64-m1n1-x0-sim` greps
  `Boot: … (x0)`. `docs/m1n1-chainload.md` for hardware steps. Next: real M1 + m1n1 client.
- 2026-10-11: **Apple 15b s5l UART:** `m1-min.dtb` (J313 RAM @ 0x80000000, serial0 @
  0x235200000); `apple/s5l_uart.c` + DTB pick in `uart_early.c`. Host:
  `test-arm64-apple` + `test-arm64-uart-early`. Next: m1n1 chain-load on M1 Air hardware.
- 2026-10-11: **Apple 15b MMU map:** `paging_init(dtb,…)` maps kernel GiB @ 0x40000000 when
  DTB RAM is elsewhere, and device-maps console MMIO above 4 GiB (M1 S5L). Host:
  `test-arm64-mmio-map`. Next: m1n1 on real M1; expect `MMU: early console MMIO @ 0x235200000`.

## 2026-10-12 — Boot speed: measured, and a plan

`tools/boottime.py` (new) boots an image in QEMU like UTM's machine and
stamps each serial line, then logs in and times the desktop's programs.
On the 64-bit live image, an everyday boot reaches the login screen at
10.2 s:

| Stage | Time |
|---|---|
| QEMU's firmware | 2.3 s |
| our loader reading the 240 MB initramfs | 1.5 s (0.02 s with a 1 MB one) |
| the kernel's DHCP wait | 1.0 s (the rest of the kernel ~50 ms) |
| runit stage 1, about a dozen small programs | 1.3 s (one start ≈ 24 ms) |
| X | 1.35 s, 0.65 s of it Mesa's software GL |
| the login screen's picture | ~2 s |

After logging in, icewm starts in 0.2 s, and the Start menu and desktop
icons follow about 3 s later. A fresh image adds 14 s of first-boot
scripts. The 32-bit loader still holds its picture 2 s at every boot.

docs/performanceSuggestions.md is the plan, losing no capability:
- **Loader:** the installer drops the live fallback initramfs; the
  stick's loader loads it only when MyOS has no driver for its disk; the
  loader writes its own boot entry; the 2 s hold goes.
- **Kernel:** DHCP in the background; wait for USB only when the root is
  on USB; mount /proc, /sys, /run and /dev/shm itself; a faster `exec`.
- **Before the login screen:** the first boot runs at image build time;
  a shorter stage 1; the login screen's picture cached; the login screen
  first in line.
- **After logging in:** the desktop's own programs, in visible order.
- **Later:** read-ahead from a boot profile.

Estimate: about 5 s instead of 10.2 s to the login screen in QEMU (24.8 s
on a first boot today), more on real hardware. Each step comes with a
before/after number.

## 2026-10-12 — Performance suggestions: graphics and the window server

docs/BOOT-SPEED.md became **docs/performanceSuggestions.md**, with a
second part: how pixels reach the screen today, and how to make that much
faster without losing programs. What the code shows:
- **MIT-SHM is off** in `10-myos.conf`, for a reason that no longer
  holds: the kernel has System V shared memory, `SO_PEERCRED`, and
  `memfd` with seals. Every picture is copied through a socket twice.
- **No GPU driver:** all drawing is on the CPU, OpenGL included.
- **No `/dev/dri`** for programs.
- **The whole-screen copy every 16 ms** happens only with the 32-bit
  kernel and a firmware framebuffer above 4 GiB. The 64-bit kernel on the
  MSI maps the screen directly.

The phases:
1. **Fewer copies in X11:** MIT-SHM on, with a test of what X needs from
   `kernel/shm.c`; the compositor chosen by its measured cost; deferred
   I/O for the 32-bit shadow.
2. **`/dev/dri/card0`** (KMS: dumb buffers, page flips, `DIRTYFB`) for
   Bochs and a simpledrm port.
3. **A Mac-like window server:** Wayland with labwc, XWayland as the
   XQuartz, and our desktop programs ported (taskbar, tray, desktop
   icons). The X11 session stays until Wayland wins on every measurement.
4. **GPU drivers** through the Linux shim: i915 for the MSI and old Intel
   laptops, then amdgpu for the Pocket 4.

With XWayland, xeyes and xfig keep working. Only tools that act on other
programs' windows (xdotool, X screenshots) change.

## 2026-10-12 — Mission Control, exactly like macOS

Rodger's goal: move the pointer into the corner and every window zooms
out, side by side, with the desktops (Spaces) live along the top. A
picture of macOS's Mission Control is now in
`docs/window-server/macos-mission-control.webp`. The design is
docs/performanceSuggestions.md §2.7:
- **In X11:** the overview of `docs/DESIGN.md` can lay out the current
  desktop. **It can't show live Spaces:** IceWM unmaps the windows of
  other workspaces, so they have no pixels.
- **In MyOS's own window server (Wayland):** every window's buffer is
  kept, on every Space, so Mission Control is a mode of the compositor,
  built in from its first version (per-window transforms and a frame
  clock from page flips).
- **On the CPU:** about 2–4 ms a frame on the MSI, 8–16 ms on a
  Pentium 4. It stays smooth with half/quarter-size copies made once,
  live miniatures at a lower rate, and an automatic "Reduce motion".
- **With GPU drivers:** free.

DESIGN.md's overview decision now points there.

## 2026-10-13 — gaps.md checked against the code; who may kill, reboot, name the host

`docs/gaps.md` was read against today's source, entry by entry. Over 50
entries changed: fixed things removed (`madvise`, `select` past fd 31,
`int3`, block device nodes, the keyboard's extended keys...), half-true
ones rewritten (/proc, /sys, mounts, the table limits, loopback), and
duplicates merged. `tools/syscall-coverage.py` read the wrong header and
counted 0 i386 calls: it says 244 (x86_64) and 264 (i386) now.

`make test` was failing before any of this: the ARM64 bring-up added
`user/musl/hello.c`, which the i386 test disk copied over the assembly
`/bin/hello` that the tests expect to exit with 42. The Makefile leaves
it out of `USER_BINS` now (the ARM64 rule names it directly).

Then the first gap: any user could signal any process, reboot and set the
host name.
- **`kill`, `tkill`, `tgkill`, `rt_tgsigqueueinfo`** follow Linux's rule
  (`signal_allowed`, `kernel/signal.c`): root may signal anyone; others,
  a process whose real or saved user is their real or effective user;
  SIGCONT also within the session. `kill(-1)` with nobody allowed is
  ESRCH, a group is EPERM, as on Linux.
- **`reboot` and `sethostname`** are root only, checked before their
  arguments as Linux does. The desktop's Turn off already goes through
  `doas`, and the login screen runs as root.
- `user/sys/systest.c` checks all of it as user 1000, on both CPUs
  (`make test`, `make ARCH=x86_64 test-uefi-shell`).

## 2026-10-13 — PROT_NONE means no access; a typed password for an empty one

- **`PROT_NONE` is enforced** (`mm/mm.c`). An area without access has no
  `VM_READ` (any of read, write or exec gives it, as x86 Linux does), and
  `mm_fault` refuses every touch of it, present page or not. `mprotect`
  keeps a present page where it is but takes its user bit away: fork, exit
  and unmap go by the present bit, so the page keeps its data and its
  owners, and comes back intact when it is made readable again. Guard
  pages now fault: musl's thread stacks have one (`pthread_create` maps
  `PROT_NONE` and opens all but the guard). Checked by `user/systest.c`
  (i386) and `user/sys/systest.c` (both CPUs), and on the x86_64 live
  disk: the desktop and Chromium start, nothing is killed.
- **`ppoll`/`pselect6` timeouts** go through one checked helper (`ts_ms`):
  a negative time or bad nanoseconds is EINVAL as on Linux, and a huge
  one is cut to ~24.8 days instead of overflowing into "wait for ever"
  (the 32-bit one at ~24 days, the 64-bit one at ~292 million years).
- **The login screen refuses a typed password for an account whose
  password is empty** (`live/myos-login`): pam_unix with `nullok`
  doesn't ask such an account, so any guess "worked". It reads
  `/etc/shadow` again at each try. Tested on the live disk: `x` + Return
  stays at the login screen, Return alone logs in.

## 2026-10-13 — File permissions are checked

Until today users and groups existed but nothing looked at a file's mode:
any user could write `/etc/shadow`. Now Linux's rules apply (new file
`fs/perm.c`; `include/vfs.h` explains them):
- **one set of rwx bits applies**: the owner's if you own the file, else
  the group's if you're in its group (fsgid or a supplementary group),
  else everyone else's. Root may read, write and search anything, and run
  anything with at least one x bit;
- **path walk**: search (x) on every directory on the way (`walk`);
- **open**: read and/or write; O_TRUNC needs write; a file the same call
  created opens however it was asked;
- **new names** (mkdir, mknod, symlink, O_CREAT, Unix socket bind): write
  and search on the directory, EEXIST first as on Linux;
- **removing and renaming**: the same, plus the sticky bit (`/tmp`: only
  the file's owner, the directory's owner or root, else EPERM); a
  directory moving to another parent needs write on itself (its "..");
- **chmod**: owner or root (EPERM); setgid dropped if you're not in the
  group. **chown**: only root gives a file away; its owner may change its
  group to one of their own; every chown of a file clears setuid (and
  setgid if the group may run it), as on Linux;
- **utimensat**: "now" needs ownership or write, any other time ownership;
  **truncate** needs write; **chdir/fchdir/chroot** need search;
  **execve** needs x for the program, its `#!` interpreter and `ld.so`;
  **inotify_add_watch** needs read; **connect/sendto** on a Unix socket
  path needs write on the socket file;
- **access()/faccessat()** answer for the REAL user, the whole path
  included (a per-thread flag, `perm_real`: our credentials are the
  process's, so swapping them would leak to sibling threads); faccessat2's
  AT_EACCESS asks for the effective one.

Device nodes the kernel makes got Alpine's modes and groups (`node_mode`,
`fs/dev.c`): the keyboard and mouse 0660 input, the framebuffer 0660
video, tty0–7 0620 tty, the console 0600 root; `/dev/pts/N` is group tty.
`/proc/<pid>/oom_score_adj` is 0666 with an owner check in its write
(its node can't follow each process's user).

Two tests had relied on there being no checks: `boottest` made a user's
file in root's 0700 tmpfs (now `/tmp`), and the x86_64 test image's `/tmp`
was 1755 (`cp -R` on macOS applies the umask; now `cp -Rp`).
`user/sys/systest.c` checks 30 rules as user 1000, plus access() with
real 1000 / effective 0, on both CPUs. On the live disk, as `myos`:
`/etc/shadow`, `/etc/passwd` and `/root` say Permission denied, the
desktop comes up, `~/.xsession-errors` has no permission errors.
Cost: open + close +38 cycles (+2.3%), stat +8 (docs/performance.md).

## 2026-10-13 — The X server wants a cookie

myos-login started X with no `-auth`, so any process on the machine, of
any user, could connect to `:0`: read the keyboard (passwords typed into
windows included), take screenshots, type into windows. Now, as Linux's
display managers do:
- a random 16-byte MIT-MAGIC-COOKIE-1 per X server (`getrandom`), in
  `/run/myos-login/X0.auth` (directory 0700, root's) for the server
  (`-auth`) and for the login screen's own connection;
- the session child, after it has become the user, writes the same
  cookie to `~/.Xauthority` (0600; to a new name with O_EXCL|O_NOFOLLOW,
  then renamed into place) and sets `XAUTHORITY`. A home it can't write
  gets `/tmp/.Xauthority-UID`;
- the entry's family is FamilyWild, so renaming the host doesn't lock the
  session out;
- runit starts a fresh login screen, server and cookie after every
  session, so what a past session left running is shut out.
`tools/qemu-desktop.py` exports `XAUTHORITY` for its ssh commands.
Checked on the live disk: without the cookie `xdotool` gets
"Authorization required", with it it works; the login and desktop are
unchanged. `tools/boottime.py` now waits for QEMU's serial port (it
connected after a fixed 0.5 s, too early once).

## 2026-10-13 — My Files: inotify and /proc/mounts

My Files was written before MyOS had inotify and /proc/mounts, so it
looked at the open folder every 2 s, and Disks tried `statfs` on every
folder in `/`. Now:
- **one inotify watch on the folder shown**, moved by `reload`. A burst of
  events (an `rm -r`) becomes one reload, 100 ms after it starts; one
  arriving while a name is being typed waits for it. If the folder itself
  goes, the window moves to its nearest parent. No inotify (an old
  kernel): the 2-second look, as before. The program now sleeps until
  something changes, where it woke every 2 s.
- **Disks reads /proc/mounts** (ext2, ext4 and vfat; `\040` in a path is
  a space), with the old scan of `/` as the fallback.
Checked on the live disk: My Files open on /tmp/ft, two files made and
one removed from a shell, and the window showed it by itself.

## 2026-10-13 — The kernel reads its config (KERNEL-CONFIG.md §10, steps 1–3)

- **Slots 0–19, the OS at 0 and 10**: exactly 10 ms apart (they were 1
  and 10: 9 ms, then 11). `sched_slot()` says -1 before the scheduler
  starts.
- **A built-in config** (`kconfig.py cdefault`): the build turns
  `config/slots.conf` into the same boot block and task list a myos-config
  partition holds, as two C arrays, and checks `include/kconfig.h`'s
  structs against `kconfig.py header`. `tools/slots2c.awk` is gone.
- **`kernel/kconfig.c`** checks a config as `kconfig.py decode` does
  (magic, version, byte order, arch, CRC, the OS's slots, owners that name
  real tasks, names and program paths a program can have, matching
  generations), picks the newer good copy of A and B (generations compare
  as serial numbers, so they may wrap), and gives the scheduler its slot
  owners. It reads the `myos-config` partition on the root's disk (a GPT
  name, or behind a hybrid MBR an unnamed type 0xDA entry), in
  storage_init, before the scheduler starts. One line says what it used:
  `kconfig: sda3 copy A, generation 3: soft realtime: audio 2 12; brakes 4 9 14 19`.
- **13 self-tests** (`kernel/kconfig_test.c`): a bad CRC, the other byte
  order, another arch, a task list from another save, a task in the OS's
  slot, a missing task, a `..` in a program path, wrapped generations.
- **On disks**: `make ARCH=x86_64 test-uefi-shell` gives its disk
  `tools/kconfig/test.conf` (`mkuefi.py --config-file`) and checks the
  kernel uses it; the live disk, with its empty partition, keeps the
  built-in config, and with a config written into a copy of it uses that.
- **The installer makes `myos-config`** (1 MiB, MyOS's type GUID, after
  myos-root) and clears it. Installed in QEMU onto an NVMe and booted:
  `nvme0n1p3`, found, empty, built-in config kept.
- Settings › Kernel says when a save takes effect (the next start) and what
  comes later. Not yet: the modes in the scheduler, binding by program,
  myos-taskd, the power savings, and the BIOS disks' copy (i386).

## 2026-10-13 — x86_64 uses the RAM above 2 GiB (for programs)

The x86_64 kernel managed only the first 2 GiB: the page allocator handed
out 32-bit addresses, and every device driver relies on that for DMA. The
MSI has 16 GiB. Now the allocator has two zones (`include/pmm.h`), as
Linux began with ZONE_DMA32:
- **low** (below 2 GiB, as before): `pmm_alloc()`, 32-bit, for the kernel,
  its caches and every DMA buffer — nothing there changed;
- **high** (the rest): `pmm_alloc_user()`, 64-bit, only for programs'
  private pages (anonymous memory, stacks, copy-on-write copies), which only
  the CPU touches. It falls back to low RAM when the high zone is full.
`pmm_init` notes the high RAM, `vmm_init` direct-maps it (2 MiB pages, RAM
ranges only, never the holes), and `pmm_refs_init` gives it its own bitmap
and share counts in low RAM. Every place a program's page passes through
— unmap, translate, unmap_range, free_tree, fork_tree, the copy-on-write
fault, mremap — now carries 64 bits (each had a `(uint32_t)` cast). The
disk DMA helpers refuse an address above 4 GiB instead of cutting it.
Checked: `make test`, `make ARCH=x86_64 test-uefi-shell` with 4 GiB (2 of
them above 4 GiB in QEMU's map) and with `QEMU_UEFI_MEM=8G` ("pmm: 6144 MiB
above 2048 MiB"), and the live desktop with Chromium: /proc/meminfo says
4 GiB (it said 2), nothing killed. Not yet on the MSI.

## 2026-10-13 — Power off on real PCs (ACPI S5)

"Turn off" switched the machine off only in QEMU: a real board's
power-off number lives in its ACPI tables, and MyOS didn't read them.
`drivers/acpi.c` reads, once at boot, just what power off needs:
- the RSDP: the UEFI loader now takes it from the firmware's
  configuration table (ACPI 2.0's GUID, else 1.0's) into `bootinfo`; on
  a BIOS PC the kernel searches the EBDA and 0xE0000–0xFFFFF;
- the XSDT or RSDT, the FADT (the PM1a/PM1b control ports, SMI_CMD and
  ACPI_ENABLE), and the DSDT, each checked by signature and checksum;
  read through the direct map when it covers them, else `ioremap`;
- `_S5`: a constant package on nearly every board, so its two sleep types
  are read from the AML bytes (NameOp "_S5_" PackageOp ...) without an
  interpreter.
Power off then writes the type, then type + SLP_EN, to PM1a (and PM1b),
after switching the firmware to ACPI mode if it wasn't. QEMU's own switch
stays as the fallback. Checked: `poweroff` makes QEMU exit by itself, the
log saying "(ACPI S5)", on x86_64 (UEFI, q35), i386 (BIOS, pc) and i386
with 1 GiB (tables above the 768 MiB direct map: the ioremap path). Found
along the way: the i386 BIOS disk doesn't start on q35 at all (gaps.md).

## 2026-10-13 — umount

`umount2` answered EBUSY to everything: the VFS didn't know whether a file
system was in use. Now, for disk file systems (ext2, vfat), `vfs_umount`
looks once at everything that can use one (nothing is counted on the hot
paths): its inodes in the tree — any reference beyond an inotify watch's,
or a mount on one of its directories — and every process's open files,
mappings, cwd, root and program (which also finds a file deleted while
open). In use: EBUSY, as before. Not: its pages are written, watchers get
IN_UNMOUNT, the directory under it comes back, and the file system's own
`unmount` finishes — ext2 marks itself cleanly unmounted and lets go of
its superblock; both forget the partition's cached blocks. Root only.
The inodes are kept, unreachable, rather than freed (a later step).
RAM mounts (tmpfs, proc, sysfs) still say EBUSY.
- `user/sys/systest.c`: EBUSY while the mount is open and while it is the
  cwd, then off, then mounted again — FAT on x86_64, ext2 /data on i386,
  where ext2test and fstest then run on the remounted disk.
- The installer unmounts what it mounted at the end ("ext2: nvme0n1p2
  unmounted cleanly"): `debugfs` on the result says "Filesystem state:
  clean" for myos-root and myos-data (always "not clean" before). After a
  failed step it unmounts too, so a second try needs no restart.

## 2026-10-13 — SA_RESTART

A signal made every sleeping call return EINTR, even when the handler was
installed with SA_RESTART (as musl's `signal()` does). Now, as Linux:
- the waits that Linux restarts return `-ERESTARTSYS` (kernel-internal,
  512): pipe, tty/pty, Unix/UDP/TCP/packet/netlink socket reads and
  writes, `accept`, `wait4`, eventfd, inotify and evdev reads, `flock`
  and `F_SETLKW`, FIFO opens. Those Linux never restarts keep EINTR
  (`poll`, `select`, `epoll_wait`, `nanosleep`, System V IPC, `connect`).
- the syscall paths note it (`signal_restart_note`); `signal_check`
  then restarts the call — the number back in EAX/RAX, the program
  counter back over the 2-byte `int 0x80` / `syscall` — if the handler
  has SA_RESTART or no handler runs, and makes it EINTR otherwise. It is
  done before the signal frame saves the registers, so the call runs
  again after `sigreturn`.
- i386's sysenter returns into the vsyscall page: an `int 0x80` now sits
  just before its return point (as in Linux's vDSO), and a restart lands
  on it, with argument 6 put back in EBP. A changed return address
  already makes the exit take the full `iret`, so ECX and EDX come back.
Checked on all three ways in: `int 0x80` (rawsystest, i386), sysenter
(musl's boottest, i386) and `syscall` (x86_64): an interrupted pipe read
runs on and gets the byte written later; without SA_RESTART, EINTR.

## 2026-10-13 — Job control: Ctrl-Z, fg, bg

The stop signals were ignored, so Ctrl-Z did nothing. Now (kernel/signal.c,
kernel/proc.c, drivers/tty.c), as Linux:
- a stop signal with its default action (SIGSTOP always) stops the whole
  process: its threads sleep until SIGCONT; the parent gets SIGCHLD
  (unless SA_NOCLDSTOP). SIGCONT continues a stopped process whatever its
  action is, and drops any stop still waiting; a stop drops a waiting
  SIGCONT; SIGKILL wakes a stopped process to die;
- `wait4` WUNTRACED and WCONTINUED, `waitid` WSTOPPED and WCONTINUED
  (CLD_STOPPED, CLD_CONTINUED), each told once; `/proc/<pid>/stat` says T;
- a group no shell can continue (orphaned) isn't stopped by the terminal;
- a background job reading its terminal gets SIGTTIN (EIO if it ignores
  or blocks it, or is orphaned).
Bash on the console still had no job control: login takes the terminal
for the new session with TIOCSCTTY, which the console ignored, so bash
found another session's group in the foreground ("no job control in
background"). The console now does what a pty does: TIOCSCTTY makes it
the session's terminal, TIOCNOTTY lets go, TIOCGPGRP/TIOCSPGRP answer only
the session that owns it (a session that has ended frees it).
Checked: `user/sys/systest.c` (stop, wait4, continue, kill) on both CPUs,
and on the live disk's console: `sleep 100`, Ctrl-Z → "[1]+ Stopped",
`ps` T, `bg` → Running, `kill`; `cat &` → "Stopped (tty input)", then
`kill -9` → "Killed".

## 2026-10-13 — NX: programs' data can't run as code (x86_64)

The x86_64 kernel never set the no-execute bit: any page a program could
read, it could run — the classic way an overflow turns into running
attacker code. Now `vmm_init` turns on EFER.NXE when CPUID says the CPU
has NX, and every page of a program gets NX unless its area may run:
`mmap`/`mprotect` with PROT_EXEC (mmap didn't record PROT_EXEC before), or
an ELF segment marked X. The flags API is 32-bit and NX is bit 63, so the
API carries `PTE_NOEXEC` (bit 31), which paging.c turns into bit 63 (and
back for `vmm_get_flags`); copy-on-write keeps it. The kernel's eager ELF
path also used to leave a program's code pages writable: each page now
gets its segments' rights (W only if one is PF_W, X only if one is PF_X).
Checked: `user/sys/systest.c` (a `ret` in a RW page and on the stack:
SIGSEGV; after PROT_EXEC: runs), and the live desktop with Chromium (its
JIT maps code with PROT_EXEC). i386 without PAE has no NX bit.

## 2026-10-13 — The kernel's own W^X (x86_64)

The x86_64 kernel image was mapped writable and runnable throughout: a
stray or malicious kernel write could change kernel code, and any kernel
data could be jumped to. Now (`vmm_init`, `map_kernel_range`): its code
is read-only and the only kernel memory that runs; read-only data is
read-only; data, `.bss`, the heap, stacks, device registers and the
direct map never run (`vmm_map` sets NX on every kernel-half address
unless told the page is code), and the direct map's view of the code is
read-only too. `linker.ld` page-aligns the three parts (a few KiB). The
first 2 MiB, where they meet, are 4 KiB pages; everything else keeps
2 MiB pages, so the TLB cost is one page table. The boot log checks
itself: "paging: kernel code read-only, data no-execute".
A Chromium crash during the live-disk check looked like it came from
this; an A/B against the committed version showed the same crash there
(1 in 3 runs, a pointer with bit 44 set): the known crash in gaps.md.

## 2026-10-13 — SMEP and SMAP (x86_64)

On CPUs that have them (Intel since Broadwell/Skylake, AMD since Zen: the
MSI, the Pocket 4), the kernel now sets CR4.SMEP and CR4.SMAP: it can't
run code from a user page, and can't read or write user memory except
inside the user-copy routines. A kernel bug tricked into jumping to, or
following a pointer into, memory a program controls then faults instead.
- `uaccess.asm` opens the window (stac) around each access and shuts it
  (clac) on every way out, the fault's included; only when `smap_on`,
  since stac/clac don't exist on older CPUs (a load and a predicted branch).
- exec writes the new program's pages and first stack directly:
  `user_access_begin/end` around those (no-ops on i386).
- every interrupt and fault entry clears AC (a program may set it, and an
  interrupt may arrive mid-copy); `syscall` already cleared it (SFMASK).
Checked with `QEMU_UEFI_CPU=max` (new: the shell test's CPU) and the live
desktop with `BOOTTIME_CPU=max` (tools/boottime.py): "paging: SMEP SMAP",
everything runs. Chromium's known crash (gaps.md) showed up there too.

## 2026-10-13 — Stack canaries in the kernel

The kernel is built with `-fstack-protector-strong` now (both CPUs):
a function with a local array keeps a secret word between the array and
its return address, and checks it before returning. A buffer overrun that
reaches the return address changes the word first, and the kernel stops
with "KERNEL STACK SMASHED: the function at ADDRESS overran a local array"
and its call chain, instead of returning to wherever the overrun pointed.
- The secret (`__stack_chk_guard`, one word, `-mstack-protector-guard=global`:
  the kernel has no thread-local storage of its own) is chosen in
  `entry.asm` before the first C function: the cycle counter, mixed with
  RDRAND where the CPU has it. On x86_64 its low byte is 0 (a string copy
  stops there) and it lives in read-only data, which `vmm_init` maps
  read-only.
- `__stack_chk_fail` is in `interrupts.c`, next to the crash report's
  stack scan (i386's is now a function, like x86_64's).
- The Makefile now rebuilds the whole kernel when its compiler flags change
  (`$(BUILD)/cflags.txt`); before, a new flag reached only the files edited
  since.
- Cost: none on system calls and interrupts, +2% on open and stat
  (docs/performance.md).
Checked: `make test` (229 self-tests pass; new: the guard is set, and on
x86_64 its low byte is 0 and its page read-only),
`make ARCH=x86_64 test-uefi-shell`, and once with a deliberate overrun
(removed again): the kernel stopped and named the function.

## 2026-10-14 — ASLR: every program somewhere else

Until now every program ran at the same addresses every time: the stack
right under 0xBFFFF000 (i386) / 0x7FFFFFFFE000, libraries from 0x40000000 /
0x7F0000000000, a PIE program at 0x00400000 / 0x555555554000. An attacker
who found a bug knew where to aim. Now each exec moves each part by a random
amount (`load_elf` in `kernel/usermode.c`, ranges in `include/user.h`):

| | i386 | x86_64 |
|---|---|---|
| stack (down) | 8 MiB, and within 8 KiB | 16 GiB, and within 8 KiB |
| mmap area: dynamic linker, libraries (down) | 256 MiB | 16 TiB |
| PIE program (up) | 256 MiB | 16 TiB |
| heap (up, after the program) | 32 MiB | 1 GiB |

Linux's ranges, except the mmap area: Linux's largest setting. The parts
can't meet (`user.h` says why). The mmap area and the stack ends are per
address space now (`mm->mmap_base`, `mm->stack_top`).
- `personality(ADDR_NO_RANDOMIZE)` turns it off for a program (gdb,
  `setarch -R`); a setuid or setgid program loses that flag (and the other
  `PER_CLEAR_ON_SETID` bits), as on Linux, so nobody can take the
  randomness away from a program with more rights. exec gets the persona
  in and out like the credentials, committed only when exec succeeds.
- i386's vsyscall page (the sysenter code) moved too: it is the page right
  above each program's stack, so it shares the stack's page table, and isn't
  at a fixed address any more. sysexit returns through `sysexit_eip`, set in
  `mm_switch` and by exec.
- Cost: +4% on exec, +1 cycle on a sysenter syscall (docs/performance.md).
Checked: systest runs a probe (itself, re-exec'd) three times and sees the
stack, heap and first mmap move each time, and stay put with
ADDR_NO_RANDOMIZE; `make test`, `make ARCH=x86_64 test-uefi-shell`, and the
live desktop with Chromium.

## 2026-10-14 — SIGTTOU: background jobs that change the terminal

A background job could change the terminal under the foreground program:
turn echo off, switch it to raw mode, or take the foreground away — and
with `stty tostop`, its output still appeared. Now (`drivers/tty.c`,
Linux's `tty_check_change` rules) a background job's tcsetattr (TCSETS*),
tcsetpgrp (TIOCSPGRP) or tcflush, and its writes when TOSTOP is set, send
SIGTTOU to its group: it stops, `jobs` says "Stopped (tty output)", and
after `fg` the call runs again. A job that ignores or blocks SIGTTOU goes
ahead — shells ignore it to take the terminal back when a job ends — and
an orphaned group gets EIO (TIOCSPGRP: ENOTTY), as on Linux.
- One check for both kinds of terminal (`job_control`, shared with the
  SIGTTIN read check): the console and a pty's slave side; the master is
  the terminal window itself and never stops.
- The write path costs one flag test while TOSTOP is off (the default).
Checked: systest (a session on a fresh pty, background jobs that call
tcsetattr, ignore SIGTTOU, write without and with TOSTOP), `make test`,
`make ARCH=x86_64 test-uefi-shell`, and bash on the live disk
(`stty tostop; echo hi &`, `stty -echo &`).

## 2026-10-14 — SO_RCVTIMEO and SO_SNDTIMEO

`setsockopt` took both and ignored them, so a program that asked for a
read "with a timeout" waited for ever when the other end went quiet. Now
they work on TCP, UDP and Unix sockets, as on Linux:
- receive and accept (SO_RCVTIMEO), send and Unix connect (SO_SNDTIMEO)
  give up with EAGAIN once the time is over, or return what they moved;
  a TCP connect returns EINPROGRESS (the handshake goes on; poll, then
  SO_ERROR); an accepted TCP socket gets the listener's values;
- a signal ends such a wait with EINTR even under SA_RESTART (signal(7):
  socket calls with a timeout are never restarted);
- both timeval layouts: two longs (options 20/21) and two 64-bit numbers
  (66/67), which 32-bit musl, with its 64-bit time_t, tries first;
  getsockopt gives the value back; microseconds out of range are EDOM.
- One wait helper for every socket kind (`sock_wait`, `socket.c`); TCP's
  `tcp_write` and `tcp_connect` take a timeout as `tcp_read` did. TCP's and
  UDP's 50 ms naps shorten to the deadline, so a 10 ms timeout ends on time.
Checked: systest (Unix, UDP and a TCP listener give up after 50 ms; the
64-bit layout; EDOM; EINTR under SA_RESTART), `make test`,
`make ARCH=x86_64 test-uefi-shell`.
Found on the way: the network fuzz harness (`make fuzz-build`, `make
attacks`) stopped building some commits ago (its stubs fell behind the
kernel); `tools/fuzz/packet.c`'s `alloc_skb` call is fixed, the missing
stubs are not yet.

## 2026-10-14 — /proc/net/wireless, and Settings' Wi-Fi pane

Wi-Fi works from the tray icon, but Settings still said "No Wi-Fi hardware
found": its pane looks for cards in `/proc/net/wireless`, as on Linux, and
MyOS had no such file. Now `fs/procfs.c` writes it in Linux's exact layout
(wext-proc.c's columns): every Wi-Fi interface, and while it is connected
the link quality (the AP's signal, -110..-40 dBm, as 0..70) and the signal
level, as cfg80211 fills it. The Wi-Fi core answers one question for it,
`wifi_link_info` (`net/wireless/core.c`), reading the connection under the
wiphy lock (freeing the AP's entry can sleep, so an unlocked read could see
it half gone). Settings' Wi-Fi pane now names the card, says whether it is
connected and how strong the signal is, and points at the panel's Wi-Fi icon
for choosing a network; its old "MyOS has no Wi-Fi drivers yet" text and the
switch that couldn't be turned on are gone. The Network pane's Wi-Fi row says
Connected / Not connected too.
Checked: the Wi-Fi self-test (simulated radio): connected at -61 dBm, then
not connected after the disconnect, eth0 not listed; `make test`; the live
disk (`cat /proc/net/wireless`: the two header lines; QEMU has no Wi-Fi card).

## 2026-10-14 — racetest: the two-waiters flake, and two stale mutations

racetest's "two waiters for one child" lost about once in four runs under
host load: the test let its sibling go (`sib_go = 1`) and then called
`waitpid`, and a preemption in between let the sibling's `WNOHANG` reap the
child first — all four wait checks failed with nothing wrong in the kernel.
Now the armed copy itself releases the sibling: the test syscall (423, test
builds only) takes a third address, a "go" flag the copy sets just before
it yields, so the sibling can only start once the main thread is inside
`waitpid`, at its status copy. Every call to 423 now passes all three
arguments: musl's `syscall()` passes six whatever the caller names, so the
readdir test had been handing the kernel garbage as its "done" flag; it now
waits for its sibling like the others.
`make verify-racetests` (each check against the bug it is for) had two
mutations that no longer applied, leaving their checks unverified: read/write
gained NULL-operation checks, and wait4's reap moved into `wait_child` with
job control. Both are rewritten for the code as it is (run_case takes several
old/new pairs now: putting the wait bug back means leaving the zombie in
`wait_child` and reaping it after the copy).
Checked: `make verify-racetests`: all eight bugs caught again (against the
wait bug, the sibling's two checks fail, as they should); `make test` three
times in a row with two other QEMUs running: racetest 18/18 each time.

## 2026-10-14 — The live disk's sshd and wpa_supplicant didn't start

Found while adding a service: five of the repository's 19 symlinks were
plain files in the working tree (dated 2026-10-11), each holding its link's
target as text — `live/overlay/etc/runit/runsvdir/default/sshd` and
`.../wpa_supplicant`, and the `supervise` links of sshd, httpd and
wpa_supplicant. This clone has `core.symlinks = false` in `.git/config`, so
git writes symlinks out that way and `git status` doesn't notice. runsvdir
only follows real links, so a live disk built from this tree started
neither sshd nor wpa_supplicant (the live boot's `ps` showed two `runsv`,
the console and the login screen): no SSH, and the Wi-Fi icon had no
supplicant to talk to. The five are real symlinks again (git sees no
change: the link targets are what it stores). Until `core.symlinks` is set
back to true, a checkout can flatten them again.

## 2026-10-14 — A system log on the live disk

Login, PAM, sshd and doas send their messages to /dev/log (syslog(3)), and
nothing was listening, so a failed login left no trace anywhere. The live
disk now runs socklog (Alpine's `socklog`, 31 KB; the live disk has no
BusyBox, so not its syslogd), the runit way, as Void Linux does:
`/etc/sv/socklog-unix` binds /dev/log as root and drops to `nobody`; its
`log` service, runit's own svlogd, writes `/var/log/socklog/current` with a
timestamp per line, starting a new file at 1 MB and keeping ten, so the log
never takes more than 10 MB. The directory is root's only (0700): login
failures are in it.
Checked on the 64-bit live disk: `/dev/log` is there; `logger` messages
arrive with their facility; the boot's own PAM messages are in the log
("session opened for user myos", "LOGIN ON console BY myos", and pam_unix
noting the empty password). sshd runs again (the symlink repair, above):
five runsv now, where there were two.
