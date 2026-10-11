# Known gaps

Shortcuts, limits and "later" notes in the current code, in one place. Remove an entry
when it's fixed. File references point at the comment that mentions the gap.

## ARM64 bring-up (QEMU virt, step 14)

- **ARM64 syscalls:** enough for BusyBox ash and Alpine bash (`clone`/`execve`/
  `wait4`/`pipe2`/`dup3`, open/read/write on the cpio, file-backed `mmap`,
  dynamic `ld.so`). Tiny fd table over the archive — not a VFS. No job
  control. Signals are stubs (`rt_sigaction` succeeds and does nothing).
- **ARM64 initramfs:** newc with `/init` (ash script), BusyBox applets,
  Alpine bash + musl + readline + ncurses. No writable files, no `/proc`.
- **ARM64 user image:** Alpine busybox.static is ET_EXEC at 0x400000 (MMIO if
  identity-mapped). Paging aliases that VA onto RAM at `ARM64_USER_IMAGE_PA`.

## At a glance: where MyOS is not (yet) Linux (checked against the code 2026-10-07)

The sections after this one have the detail. This list is what a Linux program is
most likely to trip over, each item checked in the source on the date above.

- **System calls** (`tools/syscall-coverage.py` counts them from Linux's own tables):
  on x86_64 Linux has 369; MyOS implements 218, accepts 3 and does nothing
  (`madvise`, `readahead`, `set_robust_list`), and 148 return ENOSYS. Missing ones
  real programs use: `sigaltstack`, `msync`, `mlock*`, `preadv`/`pwritev` (+`2`),
  `waitid`, `ptrace`, `rt_sigqueueinfo`, `timerfd_*`, `signalfd*`, `sendmmsg`/
  `recvmmsg`, `splice`/`tee`/`vmsplice`, `sync_file_range`, the `sched_*`
  priority/affinity setters, POSIX message queues (`mq_*`), `clone3`,
  `close_range`, `openat2`, `execveat`, `pidfd_*`, `rseq`, `io_uring`, AIO, the
  new mount API (`fsopen`, util-linux `mount` asks for it), `get_mempolicy`/
  `mbind`, `settimeofday`, `vhangup` (in the i386 table, not the x86_64 one).
  i386 lacks 200 of Linux's 442, many of them on purpose (legacy calls,
  `docs/DESIGN.md`).
- **Memory**: the x86_64 kernel uses only the first 2 GiB of RAM (the MSI has
  16 GiB); i386 uses 768 MiB. `PROT_NONE` isn't enforced (a `PROT_NONE` page is
  still filled in and readable on a touch, so guard pages don't fault) and there is
  no no-execute bit on either CPU. `madvise(MADV_DONTNEED)` doesn't drop pages, so
  they don't read back as zeros as on Linux. `MAP_SHARED` file mappings are private
  copies; `MAP_SHARED|MAP_ANONYMOUS` is private after `fork`. No address-space
  randomisation. At most 1024 areas per process.
- **Processes and signals**: no `sigaltstack`, no `SA_RESTART` (a signal makes a
  sleeping call return EINTR), no job-control stops (SIGSTOP/SIGTSTP do nothing:
  no Ctrl-Z), no ptrace or core dumps, signals are one bit each (not queued, no
  `si_pid` for `kill`). A program's `int3` arrives as SIGSEGV, not SIGTRAP: every
  x86_64 interrupt gate is kernel-only (`0x8E`, `arch/x86_64/interrupts.c`), so
  `int3` from user mode raises #GP. Chromium's and WritePerfect's deliberate
  crashes showed up that way. 64 threads, 64 processes and **64 open files per
  process** in the whole system (Chromium wants hundreds); `select` looks only at
  fds 0-31.
- **Users and security**: users and setuid exist, but **nothing checks
  permissions** (files, `kill`, `mount`, `reboot`, hostname): any user can write
  `/etc/shadow`. No capabilities: `capset` that drops any is refused, so iputils
  `ping` (setuid root, drops to CAP_NET_RAW) fails with "cap_set_proc: Operation
  not permitted" (`busybox ping` works). No seccomp, namespaces or landlock.
  Empty live passwords accept anything.
- **Files**: no hard links (`link` is EPERM), files at most 4 GiB, paths 255
  bytes, arguments + environment 64 KiB. ext2 only (ext4 refused, no journal),
  FAT32 read-only. Mounts: only tmpfs/proc/sysfs, never undone. `/proc` has only
  `self/exe`, `self/fd`, `cmdline` and `net`: no `/proc/<pid>`, `meminfo`,
  `cpuinfo`, `mounts` (so `ps`, `top`, `pidof`, `free -m` show nothing). `/sys`
  has only `class/graphics/fb0`.
- **TCP/IP** (`net/`; details under Networking and Drivers):
  - TCP has both ends (listen/accept since 2026-10-07, SYN cookies, at most 8
    waiting connections) but **at most 16 TCP connections in the whole system**
    (`TCP_MAX`), 16 KiB buffers each way and a 16 KiB window (no window scaling).
  - No congestion control (a fixed 4 segments in flight, no slow start), no
    out-of-order queue (dropped, the peer resends), no SACK, no timestamps, no RTT
    estimate (retransmit 250 ms doubling to 4 s, 8 tries), no Nagle, TIME_WAIT 2 s.
  - **No loopback**: anything to 127.0.0.1 fails with ENETUNREACH, TCP or UDP.
  - **`setsockopt` accepts every option and ignores it**: `SO_RCVTIMEO`/
    `SO_SNDTIMEO` (a read with a timeout waits for ever), `SO_KEEPALIVE`,
    `TCP_NODELAY`, `SO_REUSEADDR`, `SO_LINGER`... `getsockopt` knows four.
  - **No IPv6** at any layer (AF_INET6 is EAFNOSUPPORT, as musl expects).
  - IPv4: one device at a time, one default route, no fragments (dropped), no IP
    options, no multicast or broadcast received (DHCP excepted), 8 ARP entries.
  - DHCP is in the kernel and never renews; no DECLINE or ARP probe. The kernel's
    DNS asks for A records only.
  - User space can read the network but not configure it: the interface ioctls
    are the `SIOCGIF*` getters plus `SIOCSIFFLAGS`; netlink answers questions and
    sends no events; nothing sets an address or a route.
  - UDP queues 8 datagrams per socket; raw sockets are ICMP only (echo requests
    to send), open to every user (no CAP_NET_RAW check).
  - No `sendmmsg`/`recvmmsg`, no packet filter (netfilter), one rate-limit bucket.
- **Devices**: the serial port is written, never read (a login prompt on ttyS0
  can't be typed into). One CPU (no SMP), the 8259 PIC (no IOAPIC), no ACPI
  tables read (power off is real only in QEMU). USB: xHCI and mass storage only,
  no hubs, no USB keyboards/mice; no touchpad (I2C HID); no sound; graphics is the
  firmware framebuffer or QEMU's Bochs card, no DRM/KMS.
- **Programs**: Chromium (x86_64) starts as of 2026-10-07 (the program loader and
  `mmap` lengths were the first two blockers) and is not yet seen with a window.
  32-bit Firefox dies at GUI start; AbiWord aborts in musl's malloc.

## Boot
- **Kernel must end below 4 MiB** (incl. `.bss`): `entry.asm` maps only one 4 MiB page
  before `paging.c` maps the rest. The kernel is ~110 KiB, ~320 KiB with `.bss` (it ends
  at ~1.3 MiB physical), so there's room; the linker script now fails the build if not.
- **Needs a Pentium Pro or later.** `cpu_init()` checks and prints a message, but
  `entry.asm` already turns on 4 MiB pages (PSE) before that — a CPU without PSE would
  crash before the message. (`arch/i386/entry.asm`, `arch/i386/cpu.c`)
- **No check for BIOS disk extensions** (`int 0x13` AH=0x41) before using LBA reads.
  BIOSes from ~1998 on have them. (`mbr.asm`, `stage2.asm`)
- **Very old BIOSes copy only below 16 MiB** with `int 0x15` AH=0x87 (the 286-era version
  ignores base bits 24-31). Fine while kernel + initramfs fit below 16 MiB; a big
  initramfs (BusyBox + more) on such a machine would need "unreal mode" instead.
- **The boot disk has no file-system partition**: one partition entry (type 0x7F,
  `tools/mbr-partition.sh`) covers stage 2, the boot picture, the kernel and the
  initramfs; /data must be on another disk. Only ext2 disks labelled `myos-data` are
  mounted — any other (someone's Linux disk in an old PC) is left alone.
- **E820 map holds at most 32 entries.** (`layout.inc`, `E820_MAX`)
- **x86_64 live system** (`make ARCH=x86_64 live-uefi`): boots to the graphical
  login and desktop, from a USB stick too. First boot still logs `fsopen` (430,
  util-linux `mount`), `sigaltstack` (131, grep) and `get_mempolicy` (239,
  gdk-pixbuf-query-loaders) as not implemented; each program falls back.
- **UEFI / GOP (i386):** GOP above 4 GiB on i386 is mapped with PSE-36
  for the kernel console (32-bit virtual address, 36-bit physical in
  the PDE). User `mmap` of `/dev/fb0` cannot name that address (32-bit
  PTEs), so `simplefb` keeps an 8 MiB RAM shadow and, on the first mmap,
  blits it every 16 ms. Until then fbcon writes the real GOP. GPT is
  read only far enough to find the ESP and a Linux `myos-root` (16
  entries).
- **USB sticks need xHCI** (`docs/USB-PLAN.md`): a machine without it (the old
  PCs: UHCI/EHCI) can't mount the stick's `myos-root` and runs the ESP's
  login-only initramfs from RAM instead.

## Live disk userland (`live/`, 2026-10-03)
- **Boots to the graphical login** (`make live-hd`, `make utm-live`): `myos-login`
  starts X, checks the password with PAM and runs IceWM. agetty also runs on
  `/dev/console`: there are no `/dev/tty1`/`ttyS0` nodes yet. Alpine's apk was never
  run against the generated database (no Docker).
- **The live user is made at build time** (`tools/fetch-live.py`, `add_live_user`):
  shadow's `useradd` and `passwd` lock `/etc/passwd` with a hard link, and the VFS has
  none (each name is its inode). Until `link()` exists they fail. Settings' **Add User…**
  works around it: `/usr/libexec/myos-adduser` (setuid root, `live/myos-settings/
  myos-adduser.c`) checks an administrator's password with PAM (`/etc/pam.d/myos-admin`)
  and writes passwd, group and shadow itself (flock, write, fsync, rename). Changing a
  password still has no tool.
- **Empty passwords let any password in.** The live disk's `root` and `myos` have an
  empty password, and base-auth's `nullok` makes pam_unix accept *anything* typed for
  such an account, at the login screen and in `su` too. `myos-adduser` refuses a
  typed password for an account whose password is empty; the login screen doesn't
  yet. The installer must give both real passwords (or lock root's: `root:*:`).
- **Settings' Wi-Fi pane is still empty**, though Wi-Fi works (the MSI's AX201,
  from the tray icon): the pane reads `/proc/net/wireless`, which MyOS doesn't
  have, so it says "No Wi-Fi hardware found".
- **Nothing reads the syslog socket.** `net/unix.c` has UNIX sockets now, but no
  syslog daemon runs, so login failures still go nowhere.
- **64 fds per process** (`PROC_FILES`). bash still moves its terminal to fd 255 for
  job control and gets `EBADF`, so the login shell has no Ctrl-Z / `fg`. `cloexec`
  is a 64-bit mask; going past 63 needs a bitmap. `select()` still only sees fds
  0–31 (Linux `fd_set` on i386 is 1024 bits; Firefox uses poll/epoll).
- **No permission checks.** Processes have real users now (uid, groups, setuid exec),
  but `open`, `unlink`, `kill`... don't check them: user `myos` can write `/etc/shadow`.
  The scheduler also ignores nice values (`setpriority` stores them).
- **/proc is `/proc/self/fd`, `/proc/self/exe`, `/proc/cmdline` and a `net`
  entry** (`fs/procfs.c`). No `/proc/<pid>`,
  `meminfo`, `mounts`; `self` is a directory, not a link to the pid. Listing
  `/proc/self/fd` shows all 32 slots, open or not (opening or reading a closed one
  is `ENOENT`). A pipe or socket fd can't be reopened through it (`ENXIO`), so
  bash's `<(...)` can't use `/dev/fd`. A deleted file's link names just the file,
  not its old directory.
- **Named pipes: at most 64 open at once** (`FIFO_MAX`, `fs/pipe.c`), and a process
  may have at most 1024 memory areas (`MM_AREAS_MAX`, `mm/mm.c`; Linux allows 65530).
- **mount/umount2 are partial** (`kernel/sys_fs.c`). Only `tmpfs`, `proc` and `sysfs`
  can be mounted (`mode=` is the one option read; `size=` is ignored: it shares the RAM
  file system's pages). No devpts (`-ENODEV`); no block device files, so ext2/vfat
  give `-ENOTBLK`; no bind or move mounts. A mount can't be undone: `umount2` says
  `-EBUSY`, because the VFS doesn't count open files and cwds per mount. Remount
  accepts nosuid/nodev/noexec without enforcing them and refuses read-only. tmpfs and
  proc mounts don't show in `vfs_mount_count` (`df` misses them). Anyone may mount,
  reboot and set the host name: those calls don't check for root yet.
- **Power off is only real on QEMU** (`drivers/power.c`): it writes "soft off" to the
  PIIX4/ICH9 power-management port when the PCI subsystem vendor is QEMU's (0x1AF4).
  Real PCs need the sleep type from the ACPI tables (`_S5` in the DSDT); until we read
  them, power off halts with "safe to turn off". Restart uses the keyboard controller,
  then a triple fault. `reboot()` syncs first (Linux leaves that to the caller).
- **vhangup doesn't revoke open files** (`drivers/tty.c`): it flushes input and sends
  SIGHUP/SIGCONT to the foreground group, but a process that kept the terminal open
  can still read the next user's typing. Linux makes those file descriptors fail.
- **Alpine's package scripts assume BusyBox.** They call `adduser -S`, `addgroup`,
  `add-shell`... `/usr/sbin/{adduser,addgroup,deluser,delgroup,add-shell,remove-shell}`
  are our small front ends over shadow with BusyBox's syntax. A package whose script
  uses another BusyBox-only command would fail its install step (check
  `/var/log/first-boot.log`, or apk's output).
- **dash as /bin/sh runs scripts written for BusyBox ash.** `ca-certificates`'s
  trigger uses `&>` (an ash/bash-ism): under dash it backgrounds the command instead
  of silencing it. Harmless there; Debian fixes such scripts, Alpine doesn't need to.
- **No /bin/kill**: outside BusyBox, Alpine has none (procps-ng's is not built);
  bash and dash have it built in.
- **Coreutils is one program**: Alpine builds GNU coreutils as a single binary with
  `/bin/ls`, `/bin/cp`... as links to it (Debian has separate files). Same code.
  Overlaying one of those names must replace the symlink, never write through it
  (that replaces every applet). So ours are symlinks too: `/bin/rm` and
  `/usr/bin/rm` point at `/usr/bin/myos-rm` (and `tools/mklivehd.sh` unlinks first
  anyway). apk puts coreutils' `/bin/rm` back on an upgrade;
  `/etc/apk/commit_hooks.d/myos-rm.post-commit` re-points it after every apk run.
- **People's `rm` moves to `~/Trash`** (`live/myos-rm`): uid 1000-59999. Root and
  system accounts get GNU rm exactly. It is a seat belt, not a lock: a person can
  still delete their own files on purpose (`rm --reallyDelete`, `busybox rm`,
  `coreutils --coreutils-prog=rm`, or any program's `unlink()`). Files in `/tmp`,
  `/var/tmp`, `/dev/shm` and `/run` are deleted, not kept. Gaps: a file on another
  disk (a USB stick) is copied into `~/Trash` (the Mac and freedesktop.org keep a
  trash folder on each disk instead); `-i` on a folder asks once, not for each
  file in it; the Trash is ours (`~/Trash`, `._NAME.where`), not freedesktop.org's
  `~/.local/share/Trash`, so a GTK program's own "Move to Trash" lands somewhere
  the Recycle can doesn't look.
- **No device manager** (udev/mdev): the kernel makes the /dev nodes itself, and
  with no /sys udev would have nothing to read.

## CPU / interrupts

- MSI/MSI-X (W0a, 2026-10-06): 16 IRQs in all (16-31), handed out once and
  never given back; every message goes to the boot CPU. No IOAPIC: a device
  whose only interrupt is a line the 8259 can't see, and that has no MSI,
  gets none. The i386 kernel can't map registers above 4 GiB (no PAE), so
  under UEFI firmware that places 64-bit BARs high its NVMe stays off.

- The tick: the 8254 PIT, or the HPET in legacy replacement mode when the PIT
  is silent (gated on new Intel chipsets; found on the MSI 2026-10-06). The
  HPET is assumed at 0xFED00000 (Intel and AMD default) because the ACPI HPET
  table isn't read yet. With neither, the boot says "NO TICK" and sleeps never
  end. The planned tick is the local APIC timer in one-shot mode (CLAUDE.md),
  which also needs the APIC instead of the 8259 for device interrupts.
- **Uses the old 8259 PIC and PIT**; real machines from ~2005 have the APIC. Switch to the
  local APIC timer / IOAPIC later. (`arch/i386/pic.c`)
- **No double-fault task gate.** A kernel stack overflow becomes a triple fault (reboot)
  instead of a message. (`arch/i386/interrupts.c`)

## Memory
- **i386: RAM above 768 MiB is ignored** ("highmem"). Fine for old PCs with ≤ 768 MiB.
  (`mm/pmm.c`, `arch/i386/include/memlayout.h`)
- **Kernel is one read/write/execute blob.** Code isn't write-protected and data isn't
  no-execute. The Makefile comment says to split permissions once paging is on — paging
  *is* on now, so this is due. (`Makefile`, `arch/i386/linker.ld`)
- **Frame allocator is a linear bitmap scan.** Slow with lots of RAM; fine for now. (`mm/pmm.c`)
- **Out of memory, the program that faults dies**, whichever it is: there is no OOM
  killer choosing the biggest one (Linux's), and no swap. With the x86_64 kernel's 2 GiB
  (B1), Chromium starting while the first boot still runs can starve small programs.
  Probably what happened once on 2026-10-07: `update-mime-database` and `groupadd`
  died on a NULL read during first boot, and `chromium` was "not found". Not seen in
  nine boots since (USB and IDE, with and without serial typing).
- **The heap never returns pages to the PMM.** Size-class pages stay with their class, and
  the big-block range never shrinks. Freed memory is reused, but never goes back to the
  page pool. (`mm/kheap.c`)
- **Big blocks (> 1 KiB) are first-fit with a walk** over the big blocks. They're rare
  (thread stacks, buffers); small objects are O(1). (`mm/kheap.c`)
- **Nothing above 0xFF000000 is used yet** (planned for temporary mappings etc.).
  (`memlayout.h`)
- **System V shared memory is what MIT-SHM needs, no more** (`kernel/shm.c`, 2026-10-04):
  `shmget`, `shmat`, `shmdt`, and `shmctl`'s IPC_STAT, IPC_SET and IPC_RMID, through
  `ipc()` (117), which is all musl calls. Not there: the direct calls (395-398), IPC_INFO,
  SHM_INFO, SHM_STAT and `/proc/sysvipc/shm` (so `ipcs` lists nothing), SHM_LOCK (there's
  no swap, so nothing to lock), the pre-`IPC_64` struct, and semaphores and message
  queues (ENOSYS). At most 256 segments, and `shmget` with a key looks at all of them.
  Their pages count against the RAM-file limit (half of RAM). A first touch past that
  limit is SIGSEGV, where Linux gives SIGBUS. (Semaphores and message queues have
  their own files now: `kernel/sem.c`, `kernel/msg.c`.)
- **`MAP_SHARED | MAP_ANONYMOUS` is still private** after `fork`. The `VM_SHARED` areas
  shared memory uses could carry it too, when a program needs it.
- **The x86_64 kernel manages only the first 2 GiB of RAM** (`DIRECTMAP_LIMIT`,
  `arch/x86_64/include/memlayout.h`; the page allocator's frames are 32-bit
  numbers). The MSI has 16 GiB.
- **`madvise` does nothing** (`sys_nop`): `MADV_DONTNEED` keeps the pages, where Linux
  drops them and they read back as zeros. An allocator that relies on that sees old
  data. (`MADV_FREE` and the hints are fine as no-ops.)
- **`mprotect` walks every page of its range** to update the ones present: fine for
  ordinary ranges, slow for a huge reservation (Chromium's allocator reserves 32 GiB
  of address space; a whole-range `mprotect` would be 8 million steps).

## Threads / scheduler
- **No locks.** Preemption is on, so two threads can be inside the same code at once.
  `kmalloc`/`kfree` and the disk drivers aren't protected. (Console output is: each piece
  of text is written with interrupts off.) Syscalls run with interrupts off, so user programs
  can't collide; kernel threads could. Fix: `irq_save` sections (one CPU) or a mutex.
- **Round-robin mode still exists** for boot and the thread self-tests (before
  `sched_start()`). (`kernel/sched.c`)
- **Dead threads are reaped lazily**: their slot and stack are only freed when
  `thread_create` needs the slot. (`kernel/thread.c`)
- **Fixed tables: 64 threads, 64 processes, 64 fds per process.**
  (`THREAD_MAX`, `PROC_MAX`, `PROC_FILES`; memory areas grow, up to `MM_AREAS_MAX`)
- **Threads (2026-10-03): what's missing.** `vfork` is plain fork (copy-on-write
  makes it cheap), and `clone(CLONE_VM)` *without* `CLONE_THREAD` gets a copy.
  - **Shared futexes are keyed by address space**, not by physical page, so a
    futex in `MAP_SHARED` memory can't link two processes (Linux supports
    that). Fine for every thread of one process. (`kernel/futex.c`)
  - **No `FUTEX_WAIT_BITSET`, `FUTEX_WAKE_OP`, PI futexes or robust lists**
    (`set_robust_list` is accepted and ignored): musl and GLib don't use them;
    glibc programs would.
  - **Futex timeouts have 1 ms resolution** (the timer tick), rounded up.
  - **execve vs. exit_group race**: if one thread calls `exit_group` while
    another is in `execve` killing its siblings, the exec wins and the exit
    status is lost (Linux fails the exec instead).
  - **No `CLONE_SETTLS` beyond the 3 GDT TLS slots**, the same limit as
    `set_thread_area`.
- **No stack overflow detection.** A thread that uses more than 8 KiB silently overwrites
  the heap. Fix: guard pages (an unmapped page below each stack).
- **The slot table is compiled into the kernel.** Today `config/slots.conf` is turned into a
  C array at build time (`tools/slots2c.awk`), so changing it means rebuilding. Goal: edit
  `slots.conf` on the disk, reboot, and the kernel reads the file at boot and runs the new
  table — no rebuild. Needs a file system the kernel can read early (FAT32 already reads; or
  the initramfs, step 7) and a small parser in the kernel with the same checks as the awk
  script (slots 1/10 reserved, one owner per slot), falling back to the built-in table if
  the file is missing or wrong.
- **Slot tasks are created by the self-test**, not from the config: the kernel only knows
  names, and there are no programs to load yet. Later a name should map to a program
  (an ELF file) that the kernel starts at boot.
- **A slot owner can't run longer than 1 ms at a time, and overruns aren't reported.** It's
  simply preempted at the slot end. Real-time systems usually log or count overruns.
- **The first slot after `sched_start()` is shorter than 1 ms** (the frame starts mid-tick).

## Processes, signals, terminal (step 8)
- `select()` reads and writes only the first 32 bits of each `fd_set`
  (`fs/poll.c`, `do_select` caps nfds at 32). A process can have 64 fds now
  (`FD_MAX`), so a `select` on fd 32-63 never reports it ready. Not what POSIX
  says either: bits 32-1023 of the caller's set are never cleared.
- `ppoll`/`select` compute the timeout as a signed `tv_sec * 1000`, which
  overflows on a large value and is then treated as "no timeout", turning a
  short wait into an indefinite one. Interruptible, so it only affects the
  caller.
- **No job control stops**: SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU are ignored, so Ctrl-Z does
  nothing and `fg`/`bg` can't work. No SIGTTOU/SIGTTIN for background terminal access.
- **No syscall restart**: a signal makes a sleeping syscall return `-EINTR` even with
  `SA_RESTART`. No `sigaltstack`, no core dumps, `pselect`/`ppoll` ignore their mask.
- **One terminal**: the console (VGA + keyboard). Serial input isn't read (`drivers/
  serial.c` enables only the transmit interrupt), so the live disk's agetty on ttyS0
  prints a login prompt nobody can type into. `/dev/ttyN` are all the console.
  VT100 subset only (no scroll regions, no line drawing).
- **`poll`/`select`**: every waiter is woken on any terminal/pipe change (fine for a few
  waiters); `select` handles fds 0-31 only.
- **Time**: millisecond resolution; the CMOS clock is assumed to be UTC; no
  `settimeofday`. `alarm` and `setitimer` do ITIMER_REAL only, in whole ms; arming one
  walks the sorted list of armed processes (at most PROC_MAX), the tick checks only its head.
- **POSIX timers** (`timer_create` & co., `kernel/posix_timer.c`): 64 in the whole
  system, 16 per process (Linux: limited by RLIMIT_SIGPENDING); whole-ms resolution, so
  `timer_gettime` reports intervals rounded up to a ms. Clocks REALTIME, MONOTONIC and
  BOOTTIME only — no CPU-time clocks (EINVAL). Signals are still one bit per number, not
  a queue: a timer's expiries while its signal is pending become its overrun count (as on
  Linux), but a `kill()` of the same signal merges with the timer's and is reported as
  SI_TIMER — and `timer_delete` takes the merged bit back with it. Timers aimed at a
  thread that has ended go quiet. Signals report only `si_code` (and, for a timer, its
  id, overrun and value): no `si_pid`/`si_uid` for `kill`.
- **Sockets**: AF_INET (TCP both ends, UDP, raw ICMP), AF_UNIX (stream and
  datagram), AF_NETLINK (read-only), AF_PACKET (receive). No AF_INET6. A UDP
  socket queues 8 datagrams and drops the rest. `setsockopt` accepts and ignores
  every option (timeouts and keepalive included). Sending to 127.x fails
  (ENETUNREACH): there is no loopback. The kernel does DHCP and a DNS A lookup
  itself (A records only). IP options and fragments are dropped. The receive queue holds 32
  packets; the neighbor table holds 8. `make test`'s DNS and HTTP lines need
  a path to example.com. QEMU ignores receives for one second after the
  receiver is turned on; the first transmit waits out the rest of that
  second, including on real hardware if a frame is sent that soon.
- **`uname` says "Linux"** (programs check it to know which syscalls exist); the release
  is "6.1.0-MyOS". `getrandom` is ChaCha20 (`kernel/random.c`); how much entropy seeds
  it is an estimate (see below).
- **`mprotect(PROT_NONE)` doesn't hide pages**, on either CPU: present pages stay
  readable (`mm_mprotect` only changes the write bit), and a `PROT_NONE` area is
  still filled in on a read fault (`mm_fault` checks only writes). Guard pages and
  reservations therefore never fault.
- **No address-space randomisation**: static PIEs always load at 0x00400000.
- **Disk-file mappings are private copies** (made per page on first touch), not the
  page cache's pages. Since 2026-10-09, `MAP_SHARED` of a disk file opened for writing
  fails with ENODEV: as a copy it read stale pages after `write()`, and writes through
  it never reached the disk. SQLite's memory-mapped I/O falls back to `read()`; LMDB,
  which needs it, can't run. A read-only open still maps, as a snapshot. The real fix
  is ROADMAP step 11's single page cache behind both `read()` and `mmap()`. Programs are mapped from their file and paged in on touch, on both CPUs
  (x86_64's loader read the whole file into one buffer until 2026-10-07).
- **Dynamic linking**: one interpreter level; the interpreter is put at the first free
  place in the mmap area (no randomisation).
- **Arguments + environment: 64 KiB, 1024 entries; paths 255 bytes** (Linux: 128 KiB+, 4096).
- **Syscalls run with interrupts off** (interrupt gate); long ones are capped (reads at
  256 KiB, console writes return short). Needs locks to change.
- **Execute permission isn't enforced**: i386 has no NX bit without PAE, and the
  x86_64 kernel doesn't set NX either.
- **CPU exceptions become signals** (SIGSEGV/SIGFPE/SIGILL); a handler for them is called
  but the faulting instruction runs again when it returns (like Linux). On x86_64 a
  program's `int3` becomes SIGSEGV, not SIGTRAP: the IDT gates are all DPL 0, so `int3`
  from user mode is a general-protection fault. Chromium's `IMMEDIATE_CRASH()` and
  other deliberate traps are reported as "CPU exception 13".

## Disk / file systems
- **One command at a time per disk** on every driver (IDE, AHCI, NVMe, virtio, USB):
  no NCQ, no NVMe queue depth > 1 (NVMe and xHCI do use MSI-X). Read-ahead no longer waits (the next
  window is read while the program copies this one), but requests that don't wait
  (`blockdev_submit`) go through a worker thread per disk that runs them one by one
  through the driver — one thread switch per request, and no second command on the
  disk while one runs. Read-ahead stops at the first page whose blocks aren't next
  to each other on the disk (that window is read the waiting way), and at most 8
  read-ahead requests are on their way at once. (`kernel/blockdev.c`, `fs/pcache.c`)
- **AHCI**: no BIOS hand-off (BOHC; some boards need it), no hot plug, no port
  multipliers or ATAPI, LBA48 disks only, at most 4 disks, one controller.
- **NVMe**: namespace 1 only, 512-byte sectors only, registers below 4 GiB only, one
  controller. Takes 128 KiB at boot for bounce pages: block-cache writes are 1 KiB
  pieces in separate pages, which NVMe's PRP lists can't describe in one command.
- **virtio-blk**: the legacy (0.9.5) PCI interface only, one disk.
- **The page cache** (`fs/pcache.c`) holds reads and writes and may use all free RAM;
  the page allocator takes clean pages back when it runs out. Dirty pages (at most
  RAM/8, at most 32 MiB) are written about once a second, by `sync`/`fsync`, or when
  a writer goes over the limit — by the writer itself, so it waits. A write error is
  reported and the changes are dropped (no retry, no error for a later `fsync`).
  `fsync` writes the file's pages, then the whole block cache (metadata). A write that
  races a `truncate` of the same file may be lost. Dirty pages are never taken back
  under memory pressure without being written first, and nothing writes them early
  when free memory runs low (only the RAM/8 limit). Up to 1 GiB of cache (2^18 slots).
- **Only legacy IDE ports** (0x1F0/0x170, IRQ 14/15): a controller in "native PCI" mode
  (some SATA chipsets) isn't found. A channel whose IRQ never arrives is polled.
- **The block cache is an eighth of RAM (at most 2048 blocks)** and writes back about
  once a second: switching off loses up to ~1 s of changes (`sync` writes at once).
  Dirty neighbours go out as one command (up to 128 KiB); a failed write is reported
  and dropped (no retry). `fsync` writes the whole cache. (`fs/bcache.c`)
- **LBA28 only**: disks up to 128 GiB. LBA48 is a small addition. (`drivers/ata.c`)
- **Partitions**: the four MBR entries (no extended/logical), and GPT read far enough
  to find the ESP and Linux partitions (`fs/partition.c`).
- **FAT32 is read-only, and it is in the VFS.** The first FAT32 partition is mounted
  at /mnt (`fs/fatvfs.c`). Creating or writing a file returns EROFS. `statfs` reports
  no free clusters yet. Long names are ASCII only; other characters become `?`
  (`fs/fat32.c`).
- **Block devices live in a fixed-size array.** The comment says "no kmalloc yet" — we have
  kmalloc now. (`kernel/blockdev.c`)
- **Root on disk (9c)**: on an installed disk (`make hd`) the whole tree is on ext2. On
  a live disk only /data (if present) is; the rest is the initramfs in RAM, lost at
  power-off, at most half of RAM (`-ENOSPC`). After switching to a disk root the RAM tree
  (and the initramfs's 3 MiB) stays in memory unused; an installed disk still loads the
  full initramfs at boot. Files that mke2fs copied from the Mac belong to uid 501 (the
  Mac user), not root. No `mount`/`umount` calls; mounts can't be undone. Only the first
  `myos-root` counts. A program is mapped from its file and paged in on touch
  (the page cache, 9c-2); it is no longer copied whole at exec.
- **The disk driver spins up to 2048 status reads** before sleeping on the interrupt (fast
  in QEMU, where waking a halted CPU is slow) — but only when no other thread is ready
  to run; on real hardware that's up to ~2 ms of otherwise idle CPU per seek.
  (`drivers/ata.c`, `POLL_FIRST`)
- **ext2** (`fs/ext2.c`): the first ext2 file system found (a type-0x83 partition or a
  whole disk without a partition table) is mounted at /data; no `mount`/`umount` calls
  yet. No journal: switching off mid-change can leave the disk
  inconsistent (e2fsck repairs it); the superblock stays "not cleanly unmounted" since
  nothing unmounts. Refused: ext4 extents, 64-bit, flex_bg; mounted read-only: disks with
  checksums or other features we can't keep up to date. A directory we change loses its
  hash index (as with Linux's own ext2 driver). Hard links can't be made, and a file
  with two names (made by Linux) becomes two separate nodes here, whose sizes can drift
  apart until a reboot. Every directory looked at stays in memory (no eviction). File
  data lives in the page cache; mapping a miss costs one indirect-block lookup per 64
  blocks. One lock per file system. Block allocation scans the group's bitmap (bounded by its size).
  Times are 32-bit seconds.
- **No hard links**: a file and its name are one struct, so `link()` returns `-EPERM`
  (what Linux says on FAT). Hard links in the cpio archive become separate files.
- **No block device nodes** (`mknod` makes files, character devices and FIFOs only;
  `kernel/sys_fs.c`).
- **Permissions are stored, not checked**: everyone is root, so `chmod`/`chown` only change
  what `ls -l` shows. The one check: `execve` needs an x bit.
- **Directories: `lseek` only rewinds** (to 0); `seekdir` to a saved place isn't supported.
  Entries are listed newest first (`ls` sorts anyway).
- **`atime` isn't updated by reads** (like mounting with `noatime`).
- **Files are limited to 4 GiB** (32-bit sizes). A sparse file with a huge offset needs a
  big page table (4 bytes per 4 KiB of file size). (`fs/ramfs.c`)
- **One `read()`/`write()` copies at most 256 KiB** (so a syscall never runs long with
  interrupts off); programs and C libraries loop on short counts. (`fs/ramfs.c`, `RW_MAX`)
- **Paths are limited to 255 bytes** (Linux: 4096). (`include/vfs.h`, `PATH_MAX`)
- **The initramfs is built at compile time**: editing a file in it means `make` again
  (the kernel itself isn't rebuilt, only the archive and the disk image).
- **`make test`'s "USB stick" is still an IDE disk**; the real USB path is tested by
  `make ARCH=x86_64 test-usb` and `tools/test-disks.sh`.

## Concurrency (found 2026-10-03)

- **`copy_to_user`/`copy_from_user` can sleep, and most of the kernel was
  written as if they cannot.** A fault on a page of an `mmap`'d file goes
  through `mm_fault` into the page cache and waits for the disk, so another
  thread runs in the middle of the copy. "A syscall runs with interrupts off,
  so it is atomic" is the assumption behind a lot of this code (it is stated
  outright in `net/netdevice.c`) and it is only true between copies.

  Both halves of the answer are now in place, and **neither is sufficient on
  its own**:

  1. `fault_in_user()` (`arch/i386/include/uaccess.h`) touches a byte of every
     page of a user range before any shared state is read, so the copy that
     follows has nothing left to fault on. It is applied centrally in
     `rw()` — the one door `read()`, `write()`, `readv()` and `writev()` go
     through — and in `sys_getdents64` and the four socket send/receive
     syscalls. Cost: **+34 cycles on a 4 KiB read**, 1506 -> 1540
     (`docs/performance.md`).
  2. Each site that could become memory-unsafe is correct across a sleep
     anyway: the ring index is committed before the copy (`fs/pipe.c`,
     `net/tcp.c`, `drivers/tty.c`), the object is detached first
     (`net/socket.c`), the zombie is reaped first (`kernel/proc.c`), or a
     reference is held (`sys_getdents64`).

  **Why (1) alone is not enough:** `clone(CLONE_THREAD)` gives one address
  space several threads, so a sibling can `munmap` the range and map a file
  over it. It can only run while we are asleep, and we do not sleep after the
  pre-fault returns — but the pre-fault itself sleeps, so a thread woken while
  a later page is being probed can pull an earlier one out from under us.
  Pre-faulting removes the sleep that happens with **no attacker present**,
  which is the ordinary case and the one that made pipe and socket reads racy;
  it does not survive a deliberate race. `mm_fault` also holds the file across
  its own sleep and looks the area up again afterwards, so a sibling's
  `munmap` mid-fault cannot make it use a stale area or a freed inode.
- **The kernel has now been audited for this class once** (2026-10-03, by
  working from shared mutable state rather than from the 126 copy sites).
  Three more real bugs came out of it — `sys_getdents64`, `drivers/tty.c`,
  `kernel/proc.c`'s `wait4` — and are fixed; see `docs/security.md`. Checked
  and found safe: `fs/pcache.c`'s `copy_out` (it pins the page with `readers`
  across the copy, which is the right pattern), `fs/ramfs.c` (`file_truncate`
  keeps the page table and `page_at` re-reads it), signal delivery and
  `sigreturn` (thread-local state only). `f->pos` in the read and write paths
  of ramfs, ext2 and FAT can still be double-used by two threads sharing one
  `struct file`, which costs duplicate data but is not memory-unsafe.
- **The class is now tested.** `make test` can arm the nth copy by a thread to
  yield before copying (`include/uaccess_test.h`, syscall 423, `#ifdef
  SELFTEST` only), and `user/musl/racetest.c` uses it to race a sibling against
  a sleeping copy in `read`, `write`, `wait4` and `getdents64`.
  `make verify-racetests` reintroduces each of the six fixes one at a time and
  checks racetest notices. Running it found an eighth bug: the `getdents64` fix
  pinned the entry being copied but not `next`, which is fetched before the
  loop and goes stale during the copies that emit `.` and `..` — a name read
  out of a recycled inode and copied to user space. Both are pinned now.
- `drivers/tty.c` still has no attack-path test: reading `/dev/console` from a
  test would take the shell's input away. Its fix is the same shape as the
  pipe's, which is tested, but an analogy is not a test.

## Networking configuration (step 10f, 2026-10-03)

- Netlink (W0b, 2026-10-06) answers questions only: links, addresses and
  routes can't be changed through it yet (RTM_NEWLINK/NEWADDR/NEWROUTE are
  EOPNOTSUPP; SIOCSIFFLAGS can bring a link up or down). Multicast groups
  are remembered but no events are sent, so a program watching for a link
  to come up never hears it. No `ip route get`. Messages between programs
  (a destination pid other than 0) are refused. No /proc/net/dev, so
  `ifconfig` prints no counters.

- **No DHCP lease renewal.** `net/dhcp.c` parses the lease time and ignores
  it: the address is kept until reboot. On a network whose server hands it to
  someone else when the lease expires, two machines end up with one address.
  Renewal is not just a timer — `ipv4_addr` and friends are plain words read
  directly by every send, on the stated assumption that they are written once
  before any socket exists. Changing one under a live TCP connection would
  need the sockets told, so this is a design change rather than a loop.
- **No DHCP DECLINE, and no ARP probe before accepting an offer.** RFC 2131
  says to check the offered address is not already in use and decline it if it
  is. We take the server's word for it.
- **User space can't set an address or a route.** `AF_PACKET` (receive) and the
  `SIOCGIF*` ioctls exist, so `ifconfig` and `ip addr` can show the link, but
  `SIOCSIFADDR`, `SIOCADDRT` and netlink's RTM_NEW* don't: BusyBox `udhcpc` could
  get a lease and not apply it. The kernel's own DHCP client does the job.
- ~~ICMP through Apple's vmnet is unverified.~~ **Measured 2026-10-04 and it
  works**: a probe VM in `shared` mode took a lease from 192.168.64.1, and
  `ping google.com` returned 3 of 3 at 260–311 ms. QEMU's SLIRP still cannot do
  this (`ping 8.8.8.8` gets nothing; a packet capture shows MyOS emitting
  correct echo requests and SLIRP dropping them, because forwarding one would
  need a raw socket and therefore root), so ICMP to the internet can only be
  tested in UTM. See `docs/utm.md`.
- **`wget` failed to resolve a name once on vmnet. Not reproducible in 32
  attempts, so recorded rather than chased further.** In the first probe boot,
  `ping google.com` resolved and answered while `wget http://example.com/` in
  the same boot said `bad address 'example.com'`.
  Everything plausible was then tested and came back clean: the dual A+AAAA
  query musl's `getaddrinfo` sends (14 command-line attempts, both replies
  captured every time); the per-socket receive queue (8 datagrams deep, not
  1); the vmnet resolver itself, reached from command-line QEMU through
  SLIRP's NAT (4 of 4); the burst of pings that preceded it, replayed; and
  finally a second vmnet probe crossing program against hostname — ping, wget
  and nslookup, `example.com` and `google.com`, three rounds — which came back
  18 of 18 with every query answered in the packet capture.
  The cause was found afterwards, while chasing an unrelated DNS failure in
  `make test`: the Mac was on a tethered link where **one DNS lookup takes 2.1
  to 4.3 seconds** (the host's own `dig` reported 3550 ms, with 0% packet loss
  over 30 pings and no bad checksums in any capture). musl asks for A and AAAA
  together and has its own budget; on a link that slow it can run out. So
  nothing was lost and nothing was corrupted — the answer arrived later than
  anyone was willing to wait, which from inside the guest is indistinguishable
  from never arriving. That also explains why it would not reproduce: the
  latency varies, and most of the time it is fast enough. If it recurs, the method that works is a probe image
  that writes its results to its own disk and is read back with `debugfs` —
  there is no usable serial console under UTM. See `docs/utm.md`.
- **`sendfile` takes kernel memory only where a destination says so**
  (`file_ops.kbuf_ok`: RAM and ext2 files, pipes, terminals, ptys, /dev/null,
  TCP/UDP sockets). Anything else (a Unix socket, a FAT file) is -EINVAL, and
  the C library or program falls back to read + write. At most RW_MAX
  (256 KiB) per call, like read.
- **System calls answered as a Linux without the feature would:** swapon/swapoff
  (-ENOSYS: no swap, by design), unshare/setns with namespaces (-EINVAL), capset
  that would drop capabilities (-EPERM: MyOS has no capabilities to enforce, and
  saying yes would be a false security promise). adjtimex steps the clock for an
  offset instead of slewing it, and keeps the frequency without applying it.
  semctl/msgctl IPC_INFO and *_STAT (what `ipcs` lists with) are -EINVAL. Legacy
  fallbacks (stat64, fstat64, utimes, futimesat, 32-bit clock_settime) stay out
  (docs/DESIGN.md): musl uses them only after -ENOSYS from the modern calls.
- **inotify** has no IN_UNMOUNT (nothing unmounts), and its queue is 1024 events
  (then one IN_Q_OVERFLOW). An event goes to the watches on the file and on the
  one directory the VFS knows as its parent.

## Drivers

- Serial (COM1): a machine without a UART used to jam the console once the
  8 KiB output ring filled (nothing emptied it; found on the MSI 2026-10-06).
  Now serial_init checks for the UART and turns serial off without one. Still
  open: a UART that exists but whose IRQ 4 never arrives (not routed through
  the 8259 on some modern boards) would fill the ring the same way. The
  console makes a program wait for serial room on purpose, so test logs lose
  nothing; a missing interrupt needs a fallback (refill from the timer tick,
  or drop serial output when it stops draining).
- The e1000's transmit descriptors were only reclaimed inside `e1000_poll`,
  which runs from `net_rx_action`. Nothing on the transmit path called it and
  a completed transmit raises no interrupt (only `IMS_RXT0|IMS_LSC` are
  unmasked), so 15 sends with no inbound traffic filled the ring and every
  further `sendto` returned `EAGAIN` until something happened to receive. Found
  2026-10-03, fixed 2026-10-04: `tx_reap()` is called at the top of
  `e1000_xmit`, so the transmit path reclaims its own slots and does not depend
  on an RX interrupt. No transmit-done interrupt was added, to keep interrupt
  load off the slow target CPUs; the drained slots are not needed until the
  next transmit, which is when the reap runs. `kernel/net_test.c` sends past a
  full ring on a quiet link with interrupts off and checks none wedge.
- `net_rx_action` re-polls up to 8 times at a weight of 16 but only `drain()`s
  before and after the loop, into a 32-slot queue — so a receive flood
  allocates and copies up to 128 frames and throws ~96 away. Bounded, but it
  is four times the work and the loss happens after the copy instead of at the
  ring.
- `skb_reserve` has no bounds check at all, unlike `skb_put`/`skb_pull`/
  `skb_push`. Every caller today reserves a few bytes out of an allocation
  sized to include them, so it is safe; it is one changed size expression away
  from not being.
- `net/ipv4.c`'s ARP builder ignores `skb_put`'s NULL return and writes
  immediately. Safe only because the allocation is exactly the right size.
- `net/skbuff.c`'s double-free guard reads `skb->users` after `kfree(skb)`
  would have returned the block to the allocator, so on a reused block it
  reads garbage and proceeds. No path reaches it today — ownership is single
  and was traced end to end — but it should not be mistaken for protection.
  Nothing ever increments `users`, so it is a flag, not a refcount.
- `drain()` reads `rx_count` outside `irq_save`. Safe only because `netif_rx`
  is never actually called from an interrupt handler, which the `irq_save`
  inside it suggests was once the intent.

- IPv4 runs on one device at a time (W3b, 2026-10-06): the active one is the
  last link that became usable (Wi-Fi once connected and authorized), and
  each device keeps its own lease for when it is active again. Enough for a
  laptop with one network at a time; two networks at once (wired and Wi-Fi,
  routes by destination) need real per-device routing. Connections made on
  the old device stop working when IPv4 moves, as when a laptop changes
  networks. The kernel's own DHCP client renews nothing (an hour's lease
  from the simulated AP, forever).
- The Linux driver API (W2a, 2026-10-06), shortcuts a driver could notice:
  - `dmi_check_system` never matches: we don't read the SMBIOS tables, so a
    driver's per-laptop quirk tables are skipped.
  - `pci_disable_link_state` does nothing (ASPM stays as the firmware set
    it). iwlwifi asks for L0s/L1 off on some chips; if the AX201 is flaky
    on the MSI, this is a first suspect. `pci_reset_function` (FLR),
    `pci_stop_and_remove_bus_device` and `pci_rescan_bus` are declared but
    not written yet (a driver's last-resort recovery).
  - Crash dumps (`dev_coredump*`) are freed at once: nowhere to put them.
  - `net_ratelimit()` is always true: a flood of driver errors all print.
  - `kobject_uevent*` reach no one (no sysfs, no udev).
  - `dump_stack()` prints only the caller's address (no frame pointers).
  - `BUILD_BUG_ON` with a non-constant checks nothing (Linux's would catch
    some of those at build time through the optimizer).
- WPA2 (W3c, 2026-10-06):
  - **No software crypto.** Keys go to the driver (set_key); a driver that
    refuses a cipher can't join networks that need it. No TKIP (WPA1), no
    WEP, no management-frame protection (802.11w: the IGTK, BIP), no SAE
    (WPA3) yet — iwlwifi does all of these in hardware, so they come with
    W6/W7 if the home network wants them.
  - Received packet numbers aren't checked by us (replay protection): we
    trust the hardware (iwlwifi checks them, `RX_FLAG_PN_VALIDATED`).
  - The group key's rekeying (the AP's 2-way handshake) is wpa_supplicant's
    and should work, but the simulated AP never rekeys, so it's untested.
  - AF_PACKET (`net/packet.c`): no socket filters (SO_ATTACH_FILTER is
    accepted and ignored), no PACKET_MMAP rings, no promiscuous mode, no
    outgoing-frame taps (a packet socket sees received frames only).
  - No rtnetlink link events (RTM_NEWLINK/DELLINK): when a driver makes
    wlan0 again, wpa_supplicant doesn't hear it; its runit service watches
    wlan0's interface number and starts over instead.
  - RTM_SETLINK (link mode "dormant") is answered with an error that
    wpa_supplicant ignores; there is no `/dev/rfkill`.
- The tray (myos-menu, 2026-10-06) has one slot: the first program to dock
  gets it (the Wi-Fi icon); a second one isn't shown. The volume icon is
  still a picture.
- Unix datagram sockets (2026-10-06): at most 16 datagrams / 64 KiB wait
  for one socket and a datagram is at most 16 KiB (Linux allows ~200 KiB);
  no autobind (an unbound sender is nameless, so it can't be answered); no
  SO_PASSCRED / SCM_CREDENTIALS; shutdown() on a datagram socket says
  ENOTCONN.
- iwlwifi on our core (W6, 2026-10-06), what differs from mac80211:
  - A firmware restart replays the connection to the driver as mac80211
    does (interface, channel, link settings, the AP's peer, keys), but not
    a scan that was running, and the queue timing it replays is our default
    (we don't use the AP's WMM parameters yet).
  - No block-ack sessions: we never start one and ignore the AP's ADDBA,
    so no A-MPDU aggregation — Wi-Fi 6 works, but well below its speed.
  - Signal-quality (CQM) events aren't sent to wpa_supplicant (no roaming
    by signal); beacon loss waits for the driver's connection loss.
  - No channel-switch handling: an AP that changes channel is lost and
    found again.
  - No 802.11w (management frame protection): networks that require it
    (WPA3, some WPA2) can't be joined yet.
  - PCI: "remove" hides the device and "rescan" brings it back (no real
    re-enumeration); pci_find_ext_capability always says none (no ECAM yet),
    so iwlwifi skips what it would set there (LTR, L1 substates).
  - No wireless-regdb of our own: a card that doesn't know its country
    keeps the channels its driver registered (Intel's cards know).
  - The multicast filter is always the empty list (we join no groups: IPv4
    only, no IGMP). mDNS or IPv6 would need the real list.
- USB (`drivers/usb/`, 2026-10-07; `docs/USB-PLAN.md`): enough to run from
  a USB stick. Missing:
  - Hubs: a device behind a hub (or a laptop whose ports sit behind an
    internal one) is listed as "a hub: not supported yet".
  - Unplugging: the kernel says so but keeps the disk; its I/O then times
    out (10 s per command). Its memory is not given back.
  - Only xHCI. Old PCs (the Pentium 4 / M targets) have UHCI + EHCI, so
    their USB sticks don't work yet. Intel 7/8-series chipsets (2012-2014)
    start with their USB 3 ports routed to EHCI; Linux switches them over
    (`usb_enable_intel_xhci_ports`), we don't.
  - Bulk endpoints only: no keyboards or mice (interrupt endpoints), no
    webcams (isochronous). Mass storage only; other classes are listed.
  - Mass storage: bulk-only transport, LUN 0, 512-byte sectors; no UAS (a
    faster USB 3 protocol), no write-protect check, no TRIM.
  - One command at a time per controller's command ring, and a command that
    times out is not aborted (CRCR.CA): the ring may then be stuck.
  - "Wait for the stick we started from" needs the loader's BOOT_FROM_USB
    flag: the x86_64 loader sets it, the i386 one doesn't.
  - Not yet tested on real hardware (the MSI's Intel controller uses
    64-byte contexts and scratchpad pages; QEMU's uses neither).

### Display (step 11b, 2026-10-03)
- **Only QEMU/UTM's std VGA (Bochs, 1234:1111) gets graphics.** Real old PCs and every
  other card stay on the 80x25 text screen until the VESA boot framebuffer
  (`simpledrm` shape) is written. (`drivers/gpu/drm/tiny/bochs.c`)
- **No EDID**: the mode list is the fixed VESA table up to 2560x1600, whatever the
  window or monitor is. QEMU can offer an EDID blob; we don't read it.
  (`drm_add_modes_noedid`, `drivers/gpu/drm/drm_core.c`)
- **No `/dev/dri/card0`**: no KMS ioctls, no GEM buffer objects, no dumb buffers. Programs
  use `/dev/fb0`. One screen, one framebuffer at the start of video memory.
- **32 bits per pixel only** (XRGB8888); `FBIOPUT_VSCREENINFO` with another depth fails.
  No colour map (`FBIOGETCMAP`/`PUTCMAP`), no `read`/`write` on `/dev/fb0` (only `mmap`).
- **No horizontal panning**, and `yres_virtual` is "all of video memory" at boot.
- **Mode sets are standard modes only** (the exact size must be in the list).
- **The console's worst scroll redraws the whole screen** (about every 30th scroll at
  1920x1200: panning runs out of video memory). It runs inside `console_write` with
  interrupts off: 6.9 M instructions in QEMU, probably 5-10 ms of bus writes on a
  Pentium 4. Fine for a console, not for the 1 ms slot budget; move console drawing to
  a thread if it shows up in the jitter numbers. (`drivers/video/fbcon.c`)
- **A mode change from `/dev/fb0` redraws the console in the syscall** (interrupts off),
  same cost as above.
- **No hardware cursor**, no blinking; the cursor is an underline we draw.
- **Display ports are written without a lock.** The console and `FBIOPAN_DISPLAY` both
  write the Bochs registers; both run with interrupts off on one CPU, so they can't
  interleave today. SMP will need a lock.
- **A program that exits in `KD_GRAPHICS` leaves the console off** (as on Linux). There is
  no `VT_SETMODE`/`VT_PROCESS` switching yet.
- **A mapped `/dev/fb0` is always shared**: `MAP_PRIVATE` gets the device pages too, not a
  private copy. Pages past the device's end give SIGSEGV (Linux: SIGBUS).
- **The font comes from the BIOS** (`stage2.asm` copies it); a boot without our stage 2
  has no font and fbcon doesn't start. CP437 glyphs only (no Unicode).
- **`ioremap_wc` mappings are never undone**, and must be made before the first process
  starts (process page directories copy the kernel half once). (`arch/i386/pat.c`)

- **Keyboard ignores extended keys** (arrows, Home/End, right Ctrl/Alt…). Scancodes are
  parsed correctly, just not mapped. (`drivers/keyboard.c`)
- **Keyboard drops keys when its ring buffer is full.** (`drivers/keyboard.c`)
- **The serial output queue is 8 KiB.** Kernel messages that overflow it make `kprintf`
  wait for the port (rare); user `write()`s get a short count instead.
- **The cycle counter is calibrated during the first ~12 ms after boot**; anything asking
  for `cycles_per_us()` before then waits. It assumes the TSC ticks at a constant rate.
  On a Pentium M (and early Pentium 4s) it follows SpeedStep's clock changes, so for
  measurements there, turn SpeedStep off in the BIOS. Nothing uses it for timekeeping yet.
- **Keyboard: US layout, scancode set 1 only.**
- **PS/2 mouse** (`drivers/mouse.c`): found and set up at boot by polling ~10 command/ACK
  round trips (instant in QEMU; on a real mouse maybe 10-30 ms of boot time — make it
  interrupt-driven if a real machine shows it). Movements go into a 64-entry ring that
  nothing reads yet: `/dev/input` for X comes next (step 11b). No USB mouse.
- **Laptop touchpad is I2C HID, not the PS/2 mouse** (MSI Katana GF66 11UE,
  `docs/hardware-msi-katana-gf66.md`, 2026-10-06). The pad (Synaptics `06CB:7E7E`
  or ELAN `04F3:30AA` on `PNP0C50`) reports each finger. Two-finger scrolling
  like a Mac is possible on the hardware. It is not on the 8042 AUX port
  (`drivers/mouse.c` may see a ghost pointer). Split:
  - **Kernel (the blocker):** Intel LPSS I2C `8086:43e8` (`intel-lpss` +
    `i2c-designware`), then `i2c-hid` and `hid-multitouch`. Put contacts on a
    real `/dev/input/event*` as `ABS_MT_*`, or synthesize `REL_WHEEL` there.
    `drivers/evdev.c` only speaks PS/2 `REL_X`/`REL_Y`/`REL_WHEEL` on event1.
    No I2C, no HID. SMBus `8086:43a3` is a different adapter; the pad is not
    on it.
  - **Userspace (the Mac feel):** natural scroll (content follows the fingers),
    tap-to-click, pinch. That is libinput (or the old X Synaptics driver). The
    live disk has only `xf86-input-evdev` on `/dev/input/event1`
    (`live/overlay/etc/X11/xorg.conf.d/10-myos.conf`), which does not turn
    finger positions into a gesture. Until the kernel emits a wheel, or we add
    libinput, two fingers do nothing even if HID is up. Force Touch, inertia
    and Spaces swipes are not this pad.
- **All drivers are in the kernel**; the plan is to move most to user space. (`docs/DESIGN.md`)
- **Network security, not yet done** (needed before "bullet proof"; the threat model and
  the plan are in `docs/security.md`):
  - The CSPRNG (ChaCha20, `kernel/random.c`) is in; its seed's entropy is an
    estimate (below).
  - Nothing we send in response to a packet is rate limited (ICMP replies, ARP, the
    RST for an unknown connection, and the ACK `tcp_input` sends for duplicate data).
    When limits are added they must be per-peer and jittered, never one global
    counter — a global one is an oracle (SAD DNS, CVE-2016-5696).
  - The neighbour cache holds 8 entries for 60 s and only learns from a reply to a
    request of ours, which is tight but means a host that moves is unreachable until
    its entry expires. No ARP probe of an entry in use (Linux re-probes), and no
    gratuitous ARP on our own address at boot.
  - No IP broadcast or multicast is received at all (unicast to our address only).
    DHCP will need an exception for its own port.
  - `bind()` on a raw socket returns 0 and does nothing (TCP `bind` works since
    listen/accept: a port below 1024 needs root). `setsockopt` accepts and ignores
    everything.
  - No IPv6, at any layer: no Ethertype 0x86DD, no extension headers, no ICMPv6, no
    neighbour discovery, no `AF_INET6` (`socket()` says EAFNOSUPPORT). Not a gap we
    are in a hurry to close — see `docs/security.md`.
  - A forged ARP reply can still win the race on a request we did send (unsolicited
    ones are now ignored entirely). Not fixable on a shared segment.
  - `ip_send` checks the next hop, not the destination, so we will still *send* to a
    broadcast address even though we no longer receive one.
  - The Ethernet padding fix is untested: `drivers/e1000.c` needs PCI and MMIO so it
    is not in the `make attacks` build, and nothing checks that a frame under 60
    bytes is padded with zeros rather than the previous packet's bytes. Needs a wire
    capture (`qemu -object filter-dump`).
  - `make fuzz` covers `on_ip`, `on_arp`, `udp_input`, `parse_a` and `skip_name` well
    (94–100% of lines) and `tcp_input` at 86%. It sends one packet per run except on
    the `tcp` target, so bugs needing a particular sequence of packets are mostly
    out of reach. Host `irq_save` is a no-op, so interrupt races are invisible to it.
  - Any program can open `SOCK_RAW` and read every ICMP packet, including other
    programs'. Sending is restricted to echo requests, so the attack *on other
    hosts* is gone, but Linux requires CAP_NET_RAW for the socket itself. MyOS
    has users now but no capabilities; a check would have to be "root only",
    which would break `busybox ping` for ordinary users.
  - No Spectre v1 barriers on user-controlled indexes (syscall number, fd) and no
    retpolines; Pentium 4 and Pentium M are affected. Linux 6.1's cost for these
    is under 1% of the UDP path in instruction count (docs/performance.md); on a
    real P4 it is more and must be measured.
- **The CSPRNG's entropy is measured, not given, and the estimate is a guess.** Neither
  target CPU has RDRAND or RDSEED, so the boot key comes from timing 512 ISA port cycles
  against the cycle counter and keeping the jitter. Each sample is worth perhaps half a bit
  to two bits, which is an estimate and not a measurement: the honest claim is "probably
  enough for a 128-bit key on real hardware". **Under QEMU with `-icount` the cycle counter
  is deterministic, so the emulator produces the same key every boot** — do not treat a QEMU
  boot as having any secrets. Real entropy accounting would need a health test on the samples
  (NIST SP 800-90B's repetition-count and adaptive-proportion tests) and a refusal to proceed
  when it fails, the way Linux blocks `getrandom` until its pool is credited.
- **One rate-limit budget covers every kind of reply.** An ICMP flood from a peer therefore
  suppresses the resets we would send that same peer. That is acceptable — both are replies
  to that peer's own packets — but it is one bucket doing two jobs. Also: 16 slots, so two
  peers can share one, and an attacker who can spoof source addresses can evict a victim's
  slot. Linux has a route cache to hang this on; we do not.
- **Zero-window probing gives up, where RFC 1122 says not to.** §4.2.2.17 says a connection
  must stay open for as long as the peer keeps answering probes, even if its window never
  reopens. MyOS probes a shut window with one byte, backing off to 60 s, and then fails the
  connection with `ETIMEDOUT` after `PROBES` tries — about four minutes. Deliberate: code
  that never gives up is a hang, and the worst case has to be bounded. Linux does the same
  thing for the same reason (`tcp_retries2`), just with a longer limit. The probing itself
  is not a shortcut — it is implemented and tested, see `docs/security.md`.
- **A shut window still has no separate timer or counter.** The probe byte counts as sent, so
  the ordinary retransmit timer repeats it and `retries` counts both probes and retransmits;
  the two are told apart by whether `snd_wnd` is zero. Smaller and one timer fewer, but it
  means a connection that alternates between a shut window and real loss shares one counter
  between the two.
- **TCP** (step 10e): listen/accept are in (`net/tcp.c`). A listener answers a
  SYN with a SYN cookie and keeps nothing until the peer's ACK; at most 8
  finished connections wait for `accept`. The live disk's `sshd` (port 22)
  and `darkhttpd` (off until `doas myos-httpd enable`) can take a connection.
  Log in with this Mac's key: `tools/myos-ssh.sh setup` then `tools/myos-ssh.sh`.
  No congestion control
  (no slow start): at most 4 segments in flight, a fixed limit, not a response to
  loss. No out-of-order queue, SACK, window scaling (16 KiB window), timestamps,
  or RTT estimate (retransmit timer 250 ms doubling to 4 s, 8 tries; a shut window
  is probed on the same timer, doubling to 60 s, 10 tries). No Nagle
  (every write goes out at once, like TCP_NODELAY). TIME_WAIT is 2 s, not 60.
  At most 16 connections. Closing with unread data sends RST (as Linux does).
  Local ports are random in 49152..65535 and the initial sequence number is a
  keyed hash of the four-tuple (RFC 6056, RFC 6528).
- **Disk DMA** (9d): bus-master IDE (PIIX/ICH legacy mode); AHCI, NVMe and virtio-blk
  are separate drivers (see Disk above). None tried on real hardware yet. We don't
  pick a transfer mode ourselves (SET FEATURES, PIIX timing registers): DMA runs at
  whatever mode the BIOS set up, checked by a test read at boot. Not yet tried on
  real hardware. LBA28 only (128 GiB).

## Found running Chromium (2026-10-08)

- **MAP_SHARED of a disk file is private** (ext2): writes never reach the file
  or other processes. RAM files (memfd, /dev/shm, tmpfs) are really shared.
- **A RAM file once mapped shared keeps its pages when shrunk** (zeroed), until
  the file itself goes: a mapping may still point at them, and there is no
  reverse map to unmap it. Linux sends SIGBUS past the new end instead.
- **`/proc/self/fd` lists fds 0-63 only** (a node each, made at boot); fds up to
  1023 work, they just aren't listed. No `/proc/<pid>/task` for other processes,
  no `/proc/uptime`, no `/proc/<pid>` directories (`ps` complains).
- **No `ptrace`**: Chromium's crash reporter (crashpad) can't take a dump.
- **Memory areas are a sorted array**: lookup is a binary search, but adding or
  splitting one moves the rest — with Chromium's thousands of areas that is a
  memmove of up to a few hundred KiB per mprotect. A tree later, measured first.
- **A GPU helper of Chromium dies of a page fault** (software GL); Chromium
  carries on. Not investigated yet.
- **Chromium runs without its sandbox** (`--no-sandbox`, `/etc/chromium/myos.conf`):
  it needs seccomp-bpf and user namespaces, which we don't have. A page that
  exploits a bug in Chromium's renderer gets the user's rights, not a jail's.
  And `--disable-gpu`: no GL/Vulkan window surfaces, so it draws in software.
- **Firefox does not start** (the Start menu used to run it; now Chromium).
- **IceWM sometimes crashes at login, over and over** (a NULL read in a library,
  the same address each restart), and once myos-desktop did: 4 boots in 11 on
  2026-10-08, all while a second QEMU and builds loaded the Mac; none in 8 quiet
  boots. Probably a race in the kernel, as racetest's was. The kernel's message
  now names the library: catch it there next time.
- **`madvise`**: DONTNEED and FREE drop pages; DONTFORK, WIPEONFORK and the rest
  are accepted and ignored.
- **Loopback is UDP only.** TCP to 127.0.0.1 (and ping) still says ENETUNREACH:
  a local TCP connection needs both ends of the state machine in one kernel. A
  socket bound to 127.0.0.1 is found by its port alone, so it also hears its
  port on the outside address.
- **No IPv6** (socket(AF_INET6) is EAFNOSUPPORT); and still no DHCP renewal.

## Sound (2026-10-07, docs/SOUND-PLAN.md)

- **One program at a time.** No `dmix` yet: alsa-lib's software mixer needs ALSA's
  timer device (`/dev/snd/timer`) and user-added controls (`ELEM_ADD`, for
  `softvol`), which we don't have, so `/etc/asound.conf` points `default` straight
  at `plug:hw:0`. A second program gets EBUSY until a sound server (S5) mixes.
- **No status/control page to map.** alsa-lib falls back to `SYNC_PTR` (one ioctl
  where Linux reads memory). Measure before adding it: on i386, musl's 64-bit
  time makes the mapped layout differ from what Linux's kernel offers anyway.
- **Timestamps are monotonic** whatever `tstamp_type` asks for; no audio
  timestamps (`audio_tstamp` = system time).
- **Only interleaved access**, S16_LE and S32_LE (24 bits in 32 where the codec
  has no 32), two channels, the codec's rates from 32 to 192 kHz; `plug`
  converts the rest. No `WRITEN`/`READN`, `LINK`, suspend/resume.
- **Generic codec only.** Speaker, headphone and line-out paths, Master and Capture,
  Mic Boost, headphone and mic jacks — from the spec; no per-codec fixes (Linux
  has none for the MSI either). A combo jack's headset mic needs Realtek's
  undocumented COEF settings: not there. No HDMI audio, no S/PDIF, no beep.
- **One playback and one capture stream** per card, 256 KiB each, allocated at
  boot and never freed (so a mapped buffer can't vanish under a program).
- **Jack switching and recording** run only on real hardware: QEMU's `wav` backend
  can't record, and its codec has no jack sense. Checked on the MSI: not yet.

## Open files and the objects behind them (found 2026-10-04)

All of these are fixed; what is left is the testing gap and the capability one.
`docs/security.md` has the class, `docs/performance.md` the cost.

- **The socket pinning is not covered by a test.** Thirteen socket calls now hold their fd
  (`SOCK_SYSCALL` in `net/socket.c`), but a test for it was written, found to pass against
  the unfixed kernel by `verify-racetests.sh`, and deleted rather than kept. The fix keeps
  the semantics — a call whose fd is taken away still completes against the socket it
  started with, as Linux does — so there is no return value that differs; only the
  corruption of whatever took the freed block, and one armed yield is not enough ordering
  control to make the sibling free *and* reallocate before this thread resumes. The
  mechanism is the one the `read`/`write` case does prove.
- **The terminal ioctls check `euid == 0`, not a capability.** Linux wants
  `CAP_SYS_TTY_CONFIG` for `KDSKBMODE`, `KDSETMODE`, `VT_ACTIVATE` and `VT_SETMODE`. There
  are no capabilities here yet, which is tracked in `docs/security.md`'s plan; root is the
  stand-in. An X server is started by root, so nothing real is blocked by it today.
- **`struct file`'s serial number is 32 bits**, so it wraps after 2^32 opens. A collision
  then needs the new file to land on the same heap address *and* at the same fd number, and
  its consequence is `epoll` polling the wrong file rather than touching freed memory. The
  64-bit version cost twice the memory per open file (size classes — `docs/performance.md`).
- ~~`racetest` checks failing under host load~~ (fixed 2026-10-07): the armed copy
  yielded once, and the scheduler could pick another thread, so the interleaving
  sometimes never happened. It now yields until the sibling's "done" flag is set (at
  most 200 ms; `kernel/uaccess_test.c`). `make verify-racetests` passes again: three of
  its mutations had stopped applying after the `pread`/`pwrite` and `file_copy_in`
  changes of 2026-10-05, so those fixes went unverified until today.

## The graphical desktop (step 11b, 2026-10-04)

- **MIT-SHM is still switched off** in `live/overlay/etc/X11/xorg.conf.d/10-myos.conf`,
  so pictures travel down the socket and every local X client pays for that in copies.
  The kernel has System V shared memory now (`kernel/shm.c`, `make test`'s `shmtest`),
  and the X server's permission check gets the client's uid from `SO_PEERCRED`, which
  `net/unix.c` has. Turning the extension back on needs a check on the live disk first:
  without shared memory, Imlib2 drew nothing at all (IceWM's theme and the login screen
  came up black), so that's where to look.
- **`link()` is still missing**, so the X server cannot make its lock file and is
  started with `-nolock` (`live/myos-login/myos-login.c`). Two X servers on one
  display would not be noticed.
- **The X server lets in any local program** (found 2026-10-09). myos-login starts it
  with no `-auth` file, so X has no access control: any process on the machine, of any
  user, can connect to `:0`, read the keyboard (passwords typed into a window
  included), take screenshots and send input. A second account, a daemon, or someone
  logged in over ssh gets the desktop of whoever sits at it. `tools/qemu-desktop.py`'s
  `xdo` relies on this today, as the same user. The fix is Linux's: myos-login writes a
  random cookie to a file only root reads, starts X with `-auth` on it, and gives the
  session user `~/.Xauthority` (xauth); the test tool then sets `XAUTHORITY`.
- **16 pseudo-terminals** (`PTY_MAX`, `drivers/pty.c`), each with a one-page ring in
  each direction. `/dev/pts` is an ordinary RAM directory, not devpts: the nodes are
  made and removed by hand, and a `mount -t devpts` still fails.
- **`epoll` is an array, scanned** (`EPOLL_MAX` 64 per epoll fd, `fs/epoll.c`): every
  wait walks all of them and calls each file's `poll`. That is the cost X pays per
  event; Linux keeps a ready list. `EPOLLET` is accepted and treated as level
  triggered, and `epoll_pwait`'s signal mask is ignored.
- **`/sys` holds one thing**: `class/graphics/fb0`, because the X server's fbdev
  driver refuses a screen without it (`fs/sysfs.c`). Anything else that reads /sys —
  udev, libinput, power management — finds nothing.
- **Two-finger scroll needs kernel HID first**, then a userspace gesture
  reader. X only binds `xf86-input-evdev` to event0/event1. The laptop pad
  and the split (kernel vs libinput) are under Drivers.
- **Programs that look at other processes find nothing.** With no `/proc/<pid>`,
  `pidof`, `pgrep`, `ps` and `top` come back empty, so service scripts that wait for
  a program to appear spin for ever.
- **`FBIOPUTCMAP` fails** (`-EINVAL`), and the fbdev driver calls it even on a
  TrueColor screen: ten "(EE) FBIOPUTCMAP: Invalid argument" lines in `Xorg.0.log`
  that mean nothing. Gamma is not adjustable.
- **The desktop scale comes in five steps.** Settings >
  Displays picks one scale for the session (100-200%, automatic: 150% at
  1920x1200). `myos-xsettings` gives it to GTK (XSETTINGS, live) and Xft programs
  (`Xft.dpi`), and points IceWM at the theme drawn for it (`~/.icewm/theme`):
  `make-live-theme.py` draws IceWM's pictures and the Start menu at 100, 125, 150,
  175 and 200% (`GoldenHour`, `GoldenHour-125`...; `/usr/share/myos/menu/<scale>/`,
  2.3 MiB in all), and a scale between those is rounded to the nearest. IceWM's
  own theme menu lists all five. The login screen scales by itself (it measures
  the screen).
- **The taskbar tray is drawn over IceWM's clock.** The mockup's tray is the
  last 150 px of the bar: its own top-to-bottom color (`#2d7c8f` to `#16546a`),
  a 1.5 px `#0b2346` line on its left, the network and volume icons, and the
  time. IceWM's clock is one flat color, only as wide as the text, and it
  stretches one taskbar picture across the whole bar, so that band can't be a
  fixed 150 px. `myos-menu` paints the tray in a window on the taskbar. The
  bar's own name isn't visible to `XFetchName` (IceWM sets it with
  `XSetWMProperties`), so the window is found by climbing from the Start
  button, or the clock, to the first wide strip. IceWM's clock stays, hidden
  under the tray, so that width stays clear of task buttons; the icons sit
  just to its left, and a full bar can slide a task button under them.
- **Window corners are round; the soft shadow beside the title bar is not.**
  The mockup's windows have 8 px corners and a shadow (blur 5 px, 35% black,
  mostly on the left and right of the green bar). `make-live-theme.py` clips
  IceWM's frame to that curve and draws the 1.5 px dark edge. The mask is one
  bit, so it can leave a hole where the wallpaper shows through, and it cannot
  darken what is behind the window. The blur waits for the compositor
  (`docs/DESIGN.md`).
- **The pointer's size reaches open programs only when they reopen.** Settings >
  Mouse & Keyboard sets it (Normal to Largest, times the desktop scale; Adwaita's
  pointers, 24-96 px, X picks the nearest). GTK programs and IceWM (restarted)
  follow at once; xterm and other plain X programs bake their pointer in when they
  open. The login screen always has the default size.
- **WritePerfect's Linux window has no Touch Bar and no in-window shell.**
  The GTK front end (`linux/gui_gtk.c` in the WritePerfect tree) now has the Mac's
  menus, shortcuts (Command is Ctrl), the cream F-key strip, and HTML reveal
  plugins (ZHwriter, Thesaurus, Writer, Reveal Codes) in WebKitGTK. The Mac's
  Touch Bar F-key row and the shell pane are still Mac-only. Plugin AI calls
  that need a remote HTTP key stay Mac-only; local stroke data (`hanzi:`) works.
- **Win+I doesn't open Settings**: myos-menu opens the Start menu as soon as the
  Windows key goes down and takes the keyboard, so no Windows-key combination
  reaches IceWM. Settings is in the Start menu and on Ctrl+Alt+S
  (`/etc/icewm/keys`); Win+I needs the menu to open on key *release*.
- **Settings is mostly read-only.** Displays changes the scale, Wallpaper the
  desktop picture (the theme landscape, plus the twenty scenes in
  `/usr/share/myos/wallpapers`), and Mouse & Keyboard the pointer's size; Network, About,
  Storage, Date & Time and Users & Groups only show. The network page shows the
  address the system routes from (UDP connect + getsockname) and the DNS server:
  there are no interface ioctls (`SIOCGIF*`) to ask for more. The processor name
  comes from `cpuid` (no `/proc/cpuinfo`). Resolution can't be changed from it:
  X's fbdev driver can't switch modes while running.
- **The desktop lists `~/Desktop`** (`myos-desktop`; the folder is created if it
  is missing), in the same left-hand
  column as the Recycle can (`myos-trash`, which keeps the fifth place).
  Double-click opens a file the way My Files does. Right-click on the
  wallpaper offers New Folder and Change Desktop Background…, which opens
  Settings > Wallpaper. The Start menu programs are not planted as icons;
  they stay on the Start menu (`live/myos-menu`):
  Terminal, My Files, Web Browser (Firefox), WritePerfect, AbiWord, Preview,
  Disks (My Files' disk view), Settings, Run..., Log off and Turn off.
- **Firefox is on the disk but needs 1 GiB of RAM.** Alpine 3.20's 32-bit package
  (plus codecs, 255 MiB). `make utm-live` and `make run-live` give the machine
  1024 MiB; 256 MiB (the old default) is enough for IceWM and AbiWord and not
  for Firefox. The session turns the content-process sandbox off
  (`MOZ_DISABLE_CONTENT_SANDBOX`, `MOZ_DISABLE_GMP_SANDBOX`: no seccomp, no
  user namespaces) and turns WebRender's GL path off (`MOZ_WEBRENDER=0`: no
  render node). `/dev/shm` is a tmpfs for `shm_open`. `/proc/self/exe`,
  `fallocate` and `memfd_create` are there (Firefox uses them to find its
  files and size shared memory). The live VM CPU is `pentium3,+sse2`: Alpine
  Firefox is built with SSE2, which a Pentium 4 / Pentium M has. Missing
  `/proc` files (`cpuinfo`, `meminfo`, `/proc/<pid>/…` beyond `self/fd` and
  `self/exe`) or other syscalls may still stop it. A GUI start dies in
  `libxul.so` with a write to address 0 (no message, just "Segmentation
  fault"). `firefox --version` works. Look at `~/.xsession-errors` and the
  serial line `firefox: killed by CPU exception`.
  Chromium is not
  available for i386. There is no desktop icon: it is the Start menu's
  "Web Browser".
- **My Files (`live/myos-files`) is a first version.** It looks like the mockup but
  the text is DejaVu Sans, not the mockup's Verdana/Trebuchet (Microsoft's licence
  doesn't let us ship them).   The View button (right of Up) has the Mac's four
  layouts: Icons, List, Columns, Gallery. Pictures show a thumbnail of the file
  (cached as a hidden `._name` sidecar, the Mac's name on a disk that isn't HFS).
  A PDF's sidecar is its first page, used as the icon and as Columns' far-right
  preview. List does not unfold a folder in place (open it, or use Columns). Other
  documents still use their type icon. Drag a file onto the Recycle can (or
  Delete) to put it in `~/Trash`; `rm` does the same, and remembers path,
  owner and mode in `._name.where` so Put Back can restore them. Empty Trash
  (right-click the can, or `rm --emptyTrash`) is the real delete; so is
  Delete Permanently (`rm --reallyDelete`, which runs GNU rm). No cut/move (copy, then delete), no properties window.
  Without inotify it looks at the folder and its files every 2 s; in folders of
  more than 1000 files only the folder's own time is watched, so a file written
  over there needs F5. Disks finds other disks by `statfs` on the folders in `/`
  (no `/proc/mounts`).
- **Evince draws its own title bar** (GTK's header bar), not the Golden Hour one, and
  (it used to warn that it couldn't watch its settings file; inotify is in now).
- **All Programs** is the row above Log off. It opens the Programs folders the way
  Windows XP did: `/usr/share/myos/programs`, then `~/.local/share/myos/programs` on top
  of it, then any `.desktop` file in `/usr/share/applications` that is not already listed.
  The pinned rows above it come from `MENU_LEFT` / `MENU_RIGHT` in
  `live/theme/make-live-theme.py`.
- **The Start menu's pinned rows are a fixed picture.** Those entries are drawn at build
  time, so adding one there means rebuilding the theme. All Programs is the part that
  reads the folders. The user's name
  is drawn in DejaVu Sans Bold, the rest of the menu in the theme's Trebuchet/Verdana
  (the build machine's fonts), so the header text is a slightly different face.
- **Log off and Turn off don't ask "are you sure?"** IceWM's own confirmation box isn't
  drawn in the theme (`ConfirmLogout=0`); a themed one belongs in `myos-menu`.
- **AbiWord aborts at start** ("Aborted" in a terminal, nothing in
  `~/.xsession-errors`), though it is in the Start menu. Mostly, not always (1 start
  in ~10 got a window). The serial log says more: `abiword: killed by CPU exception
  14 ... at EIP 400248b2, read of 80000008`. That is musl's malloc (mallocng's
  `get_meta`, libc offset 0x248b2) checking a block it was asked to free: the
  block's group header says its owner is at 0x80000000, which is garbage. Once (of
  ~12) the X server died the same way, same address. Same in every desktop scale and
  with myos-xsettings stopped, so not the scale. 0x80000000 is what a float-to-int
  conversion of NaN gives, and the top of user space. Not found yet (2026-10-04).
  Two more clues: the crashing code is reached from libc offset 0x24d60 (found by
  scanning the user stack for return addresses on the fault; the caller outside musl
  wasn't in the first 256 bytes). (It also logged `syscall 328 not implemented`,
  eventfd2, which is in now: `fs/eventfd.c`.)
- **WritePerfect is built from outside this tree** (`WP_DIR`, default `../wp51/wp51c`),
  so it is on the live disk only when that folder is on the build machine; MyOS's
  own source on the disk can't rebuild it. The Start menu opens its terminal editor
  in an xterm (`writeperfect`). `wp --gui` is its Linux ZHwriter screen, for Chinese,
  not the full Mac window, and the pinyin there needs `node`, which isn't on the
  disk. Its `agent.c` leaves out `<sys/select.h>`, which musl needs; the Makefile
  adds it with `-include` rather than editing WP's tree.
- **The hot corner only curls.** `myos-menu` draws the curl (24 px, 40 px under the
  mouse) in a desktop-type window below all windows, but the window overview it should
  open isn't written (ROADMAP 11b), and it grows at once instead of over 120 ms.
- **Run... can't tell you a program doesn't exist**: it hands the line to `sh -c` and
  the error goes to `~/.xsession-errors`.
- **myos-menu leans on two IceWM details**: the Start button's window is named
  "TaskBarMenu", and a taskbar button is its picture plus 5 px each way (how the
  151x35 placeholder makes a 156x40 button). A different IceWM may need these
  checked (`src/wmtaskbar.cc`, `YButton::updateSize`).

## Kernel memory safety (found 2026-10-08)

Guard pages under every thread's kernel stack, a double-fault stack of its
own, `cld` on every entry and page-table checks are in (docs/LOG.md,
2026-10-08). Still open:

- **"main" (the boot thread) has no guard page.** Its stack is in the
  kernel image, which is mapped with big pages (2 MiB / 4 MiB), so no
  single page under it can be left out. It only idles after boot.
- **The kernel image is writable and executable throughout.** Text is not
  read-only and data is not no-execute (x86_64 has NX; non-PAE i386 has
  none). A stray kernel write can change kernel code. Fix: map .text
  read-only and the rest NX, which costs TLB entries unless the sections
  are aligned to the big pages.
- **The stack pool keeps its pages.** A window's pages stay with it after
  its thread dies, for the next thread: the pages held are the most
  threads ever alive at once (16 KiB each on x86_64), never given back.
- **MEMDEBUG has no red zones.** It catches a write to freed memory (pages
  and kmalloc objects), not an overrun into a neighbour that is in use.
- **Not yet: stack canaries** (-fstack-protector-strong) against a buffer
  overrun inside a frame overwriting a return address; SMEP/SMAP (the
  kernel must not run, or quietly read, user pages).

## Disk files and /proc per process (2026-10-09)

- **Reading two partition tables at once isn't safe.** `partition_scan`
  uses one static sector buffer. That only matters if BLKRRPART runs while
  a USB disk is being plugged in.
- **Only the first 16 sd disks get correct numbers.** sdq and later need
  Linux's majors 65–71; ours put minor > 255 into an 8-bit rdev.
  BLOCKDEV_MAX is 16, so this can't happen yet.
- **No BLKDISCARD (TRIM).** mke2fs carries on without it.
- **`umount` still always answers EBUSY.** The VFS doesn't count open
  files per mount, so the installer can't unmount the new system to check
  it.
- **Some /proc/<pid> numbers are zeros.** RSS, page faults, context
  switches, I/O bytes, and `environ` (the kernel doesn't keep a copy of a
  program's environment). `ps` and `top` show 0 memory for each process.
- **`/proc/<pid>/fd`, `exe` and `cwd` exist only under `/proc/self`.**
  Another process's open files can't be listed yet (`lsof`, `ls -l
  /proc/PID/fd`).
- **EPOLLET is real only for change-count files** (the mount tables).
  Everywhere else it is level-triggered, which costs extra wake-ups and
  is right only for readers that read until EAGAIN.
- **Chromium crashes in the browser process soon after loading a page**
  (a use-after-free-looking read). Not yet found; see docs/LOG.md
  2026-10-09. No `ptrace` (crashpad can't take its dump), and no
  `/proc/<pid>/task` for other processes.
- **A deleted file that is still open reports `st_nlink` 1** (Linux: 0). SQLite only
  logs a warning about it.

## The installer (2026-10-10)

- **UEFI only, and only onto a disk without an EFI partition** (or by
  erasing). Next to Windows it would have to add EFI/MYOS to Windows' FAT
  partition, and MyOS's FAT driver only reads. Installing on a BIOS PC
  isn't written either (the helper refuses both, before writing).
- **No boot entry is written** (no efibootmgr / SetVariable). The firmware
  finds EFI/BOOT/BOOTX64.EFI by itself on most PCs. On others, pick the
  disk in the boot menu (F11 on the MSI).
- **The installed EFI partition carries the live login initrd** (240 MB),
  which an installed system doesn't need, and the firmware loads it at
  every start. A small one, or none, would start faster.
- **A failed try leaves the disk mounted** (umount is always EBUSY): the
  window asks for a restart before the next try.
- **ssh login is off on an installed system.** Turn it on with
  `doas ln -s /etc/sv/sshd /var/service/` (a new host key is made then).
- **The window's install flow was not clicked through in QEMU**, only
  the helper with a written plan, then a boot of the result.

## Kernel config (2026-10-10)

- **The kernel doesn't read the config yet.** Settings › Kernel saves it
  to `myos-config`, but the scheduler still runs `config/slots.conf`'s
  table (docs/KERNEL-CONFIG.md §10, steps 2–7). The page says so.
- **No `myos-taskd`, no binding at `execve`, no OS class yet.** Slots
  still go to a thread with the right name (docs/TIME-SLOTS.md).
- **Behind a hybrid MBR (no 0xEE entry) the kernel never reads the GPT**,
  so GPT names are invisible there. The live/UTM images list
  `myos-config` in the MBR too; the kernel's own reader will need the GPT.
- **The installer doesn't make `myos-config` yet**, so an installed
  system has nowhere to save; Settings says "no myos-config partition".
- **Only the x86_64 image has it.** The i386 live disk (MBR, BIOS) needs
  the config in its 0x7F boot area (docs/KERNEL-CONFIG.md §8).
- **Some savings only the kernel can confirm.** Deep idle (ACPI _CST), C1E,
  PCIe ASPM, USB autosuspend, SATA link power and disk spin-down are
  switchable in Settings and marked "checked when MyOS starts": a program
  can't see them, and the kernel will skip what isn't there.
- **MyOS's /proc/cpuinfo leaves out flags** such as `est`, `acpi` and `tm`,
  so Settings reads CPUID itself.
