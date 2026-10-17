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

## At a glance: where MyOS is not (yet) Linux (checked against the code 2026-10-13)

The sections after this one have the detail. This list is what a Linux program is
most likely to trip over, each item checked in the source on the date above.

- **System calls** (`tools/syscall-coverage.py` counts them from Linux's own tables):
  on x86_64 Linux has 369; MyOS implements 244, accepts 2 and does nothing
  (`readahead`, `set_robust_list`), and 123 return ENOSYS. Missing ones real
  programs use: `ptrace`, `timerfd_*`, `signalfd*`, `sendmmsg`/`recvmmsg`,
  `splice`/`tee`/`vmsplice`, `sync_file_range`, POSIX message queues (`mq_*`),
  `clone3`, `openat2`, `execveat`, `pidfd_*`, `rseq`, `io_uring`, AIO, the new
  mount API (`fsopen`, util-linux `mount` asks for it), `get_mempolicy`/`mbind`,
  `settimeofday`, `seccomp`, landlock. i386 implements 264 of Linux's 442; most
  of the 176 it lacks are legacy calls left out on purpose (`docs/DESIGN.md`).
- **Memory**: the x86_64 kernel uses all RAM, but only programs' private pages
  live above 2 GiB: the kernel, its caches and RAM files fit in the first 2 GiB;
  i386 uses 768 MiB. On x86_64 programs' data, stacks and heaps can't run as code
  (NX); i386 has no NX bit without PAE. A disk file can't be mapped `MAP_SHARED` for
  writing (ENODEV; read-only it is a snapshot); RAM files and
  `MAP_SHARED|MAP_ANONYMOUS` are really shared. Programs' addresses are randomised
  (ASLR, since 2026-10-14); the kernel's are not.
- **Processes and signals**: job control works (Ctrl-Z, `fg`, `bg`, SIGTTIN, SIGTTOU), no
  ptrace or core dumps, signals are one bit each (not queued, no `si_pid` for
  `kill`). 512 threads and 256 processes in the whole system on x86_64 (256 and
  128 on i386); 1024 open files per process.
- **Users and security**: users, groups and setuid exist, and file permissions
  are checked as Linux does (`fs/perm.c`, 2026-10-13); `kill` reaches only your
  own processes; `reboot`, `sethostname`, `mount` and making device nodes are
  root only. No ACLs, and the kernel's own files have fixed modes. No capabilities: `capset` that drops any is refused, so iputils
  `ping` (setuid root, drops to CAP_NET_RAW) fails with "cap_set_proc: Operation
  not permitted" (`busybox ping` works). No seccomp, namespaces or landlock.
  Empty live passwords accept anything.
- **Files**: no hard links (`link` is EPERM), files at most 4 GiB, paths 255
  bytes, arguments + environment 64 KiB. ext2 only (ext4 refused, no journal),
  FAT32 read-only. Mounts: tmpfs, proc, sysfs, and ext2/vfat from a block device
  (root only); disk mounts can be undone (`umount`), RAM mounts can't. `/proc` has
  `/proc/<pid>`, `cpuinfo`, `meminfo`, `mounts`, `uptime` and more, but some
  per-process numbers are zeros (memory, faults) and `fd`/`exe`/`cwd` exist only
  under `/proc/self`. `/sys` has `fb0`, the network, the disks and the CPU, no
  input devices.
- **TCP/IP** (`net/`; details under Networking and Drivers):
  - TCP has both ends (listen/accept since 2026-10-07, SYN cookies, at most 8
    waiting connections) but **at most 16 TCP connections in the whole system**
    (`TCP_MAX`), 16 KiB buffers each way and a 16 KiB window (no window scaling).
  - No congestion control (a fixed 4 segments in flight, no slow start), no
    out-of-order queue (dropped, the peer resends), no SACK, no timestamps, no RTT
    estimate (retransmit 250 ms doubling to 4 s, 8 tries), no Nagle, TIME_WAIT 2 s.
  - **Loopback is UDP only**: TCP and ping to 127.0.0.1 fail with ENETUNREACH.
  - **`setsockopt` ignores most options**: `SO_KEEPALIVE`, `TCP_NODELAY`,
    `SO_REUSEADDR`, `SO_LINGER`... are accepted and change nothing. `SO_RCVTIMEO` /
    `SO_SNDTIMEO` work (since 2026-10-14) on TCP, UDP and Unix sockets — not yet on
    netlink and packet sockets, where they are accepted and ignored. A negative
    timeout means "don't wait" on Linux; here it waits 1 ms.
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
- **Devices**: serial input reaches the console, but there is no `/dev/ttyS0`.
  One CPU (no SMP), the 8259 PIC plus the local APIC for MSI only (no IOAPIC), no
  ACPI tables read but the power-off numbers (drivers/acpi.c). USB: xHCI, mass storage
  and HID keyboards/mice only, no hubs; no I2C touchpad (I2C HID,
  `docs/TOUCHPAD-PLAN.md`); sound is HD Audio, one program at a time; graphics is
  the firmware framebuffer or QEMU's Bochs card, no DRM/KMS.
- **Programs**: Chromium (x86_64) is the Start menu's browser; it runs without
  its sandbox and crashes in the browser process soon after loading a page
  (2026-10-09 below). 32-bit Firefox dies at GUI start; AbiWord aborts in musl's
  malloc.

## Boot
- **Kernel must end below 4 MiB** (incl. `.bss`): `entry.asm` maps only one 4 MiB page
  before `paging.c` maps the rest. The kernel is ~490 KiB, ~1.3 MiB with `.bss` (it ends
  at ~2.3 MiB physical, 2026-10-13: up from 1.3 MiB), so there's room, but less each
  month; the linker script fails the build if not.
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
- **SeaBIOS (QEMU's BIOS) hangs after stage 2 sets the picture's mode** when
  System Management Mode is on (QEMU's default). SeaBIOS runs some of its
  drivers (AHCI, USB) in 32-bit mode and gets there through SMM; once the
  video BIOS has switched to mode 13h (`int 0x10`, AX=0x13: that call alone
  does it, found by taking stage 2 apart), the next trip through SMM never
  comes back. So a BIOS boot from a SATA disk hangs at stage 2's first read,
  every time, and a machine with a USB keyboard hangs now and then (SeaBIOS
  polls it). Found 2026-10-17 by `make test-install` and `make test-usb-hid`,
  which start those machines with `smm=off`. IDE disks without USB devices
  (UTM's old-PC VMs) never go that way. Real old PCs have other BIOSes, whose
  SATA ROMs are 16-bit code; mode 13h is the oldest graphics mode there is.
  Not reported to SeaBIOS/QEMU yet.
- **x86_64 live system** (`make ARCH=x86_64 live-uefi`): boots to the graphical
  login and desktop, from a USB stick too. First boot still logs `fsopen` (430,
  util-linux `mount`) and `get_mempolicy` (239, gdk-pixbuf-query-loaders) as not
  implemented; each program falls back.
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
  `/dev/console`: `/dev/tty1`-`tty7` are all that one console, and there is no
  `/dev/ttyS0` node. Alpine's apk was never run against the generated database
  (no Docker).
- **The live user is made at build time** (`live/tools/fetch-live.py`, `add_live_user`):
  shadow's `useradd` and `passwd` lock `/etc/passwd` with a hard link, and the VFS has
  none (each name is its inode). Until `link()` exists they fail. Settings' **Add User…**
  works around it: `/usr/libexec/myos-adduser` (setuid root, `live/myos-settings/
  myos-adduser.c`) checks an administrator's password with PAM (`/etc/pam.d/myos-admin`)
  and writes passwd, group and shadow itself (flock, write, fsync, rename). Changing a
  password still has no tool.
- **Empty passwords let any password in, in `su`.** The live disk's `root` and `myos`
  have an empty password, and base-auth's `nullok` makes pam_unix accept *anything*
  typed for such an account without asking. `myos-adduser` and the login screen
  (since 2026-10-13) refuse a typed password for an account whose password is empty;
  `su` doesn't. The installer gives both real passwords (and locks root's).
- **Settings' Wi-Fi pane only shows the state** (since 2026-10-14: `/proc/net/wireless`):
  the card, connected or not, and the signal. Choosing a network is still the tray
  icon's job (myos-wifi); the pane has no network list of its own. The kernel's
  `/proc/net/wireless` has no noise figure and no discard counters (zeros, as Linux
  shows them for cfg80211 cards). The connected line has been seen on the simulated
  radio (`make test`), not yet on the MSI's AX201.
- **The system log is plain files, root only** (since 2026-10-14: socklog, the
  runit way): `/var/log/socklog/current`, 10 MB at most, one log for everything (no
  separate auth log); read it with `doas cat`. Nothing sends it elsewhere, and the
  kernel's own messages (`dmesg`) don't go into it (no klogd: socklog's `klog`
  service needs `/proc/kmsg`, which MyOS doesn't have). Messages sent before it
  starts, early in boot, are lost (as on Linux without a kernel log buffer for them).
- **Permissions are checked, the plain Unix way only** (`fs/perm.c`, since
  2026-10-13): owner/group/other bits, the sticky bit, setgid directories, and
  Linux's rules for open, create, delete, rename, chmod, chown, utimensat,
  truncate, chdir, execve, access, inotify and Unix sockets. Not there: ACLs
  (no extended attributes are kept at all), `noexec`/`nosuid` mounts
  (accepted, not enforced), writing to a
  setuid file doesn't clear its setuid bit (Linux does), and the kernel can
  still read a present `PROT_NONE` page (no SMAP). Device nodes have Alpine's
  modes and group numbers built in (`node_mode`, `fs/dev.c`): no udev to set
  them. The scheduler also ignores nice values (`setpriority` stores them).
- **/proc** (`fs/procfs.c`) has `/proc/<pid>`, `cpuinfo`, `meminfo`, `mounts`,
  `stat`, `uptime` and the `net` files; what's missing per process is in the
  2026-10-09 section. `self` is a directory, not a link to the pid. Listing
  `/proc/self/fd` shows the 64 built-in slots, open or not (opening or reading a
  closed one is `ENOENT`); fds 64-1023 work but aren't listed. A pipe or socket fd
  can't be reopened through it (`ENXIO`), so bash's `<(...)` can't use `/dev/fd`.
  A deleted file's link names just the file, not its old directory.
- **Named pipes: at most 64 open at once** (`FIFO_MAX`, `fs/pipe.c`).
- **mount/umount2 are partial** (`kernel/sys_fs.c`). Root may mount `tmpfs`, `proc`,
  `sysfs`, and `ext2`/`vfat` from a block device (`mode=` is the one tmpfs option
  read; `size=` is ignored: it shares the RAM file system's pages). No devpts
  (`-ENODEV`); no bind or move mounts. `umount2` (root only) takes an ext2 or vfat
  mount off when nothing uses it (`vfs_umount`), and EBUSY otherwise; tmpfs, proc
  and sysfs mounts stay (EBUSY), and MNT_DETACH (lazy) isn't done. An unmounted
  file system's in-memory inodes are kept, unreachable, not freed: a stick
  mounted and unmounted again and again costs a little memory each time. Remount
  accepts nosuid/nodev/noexec without enforcing them and refuses read-only. tmpfs and
  proc mounts don't show in `vfs_mount_count` (`df` misses them).
- **Power off reads only what it needs from ACPI** (`drivers/acpi.c`, since
  2026-10-13): the PM1a/PM1b ports from the FADT and `_S5`'s two sleep types, found
  as bytes in the DSDT (no AML interpreter). A board whose `_S5` is computed, not a
  constant package, a "hardware-reduced" one (no PM1 ports), or one that needs
  `_PTS` run first still just halts with "safe to turn off". QEMU's own switch stays
  as the fallback. Not yet tried on the MSI. No other sleep state (suspend). Restart
  uses the keyboard controller, then a triple fault; ACPI's reset register isn't
  read. `reboot()` syncs first (Linux leaves that to the caller).
- **The i386 BIOS disk doesn't start on QEMU's q35 machine** (no serial output at
  all, any RAM size; found 2026-10-13). The `pc` machine and UEFI are fine.
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
  `/usr/bin/rm` point at `/usr/bin/myos-rm` (and `live/tools/mklivehd.sh` unlinks first
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
  under UEFI firmware that places 64-bit BARs high its NVMe and xHCI (USB)
  stay off (QEMU's OVMF does: `make test-usb-hid` boots i386 by BIOS).

- The tick: the 8254 PIT, or the HPET in legacy replacement mode when the PIT
  is silent (gated on new Intel chipsets; found on the MSI 2026-10-06). The
  HPET is assumed at 0xFED00000 (Intel and AMD default) because the ACPI HPET
  table isn't read yet. With neither, the boot says "NO TICK" and sleeps never
  end. The planned tick is the local APIC timer in one-shot mode (CLAUDE.md),
  which also needs the APIC instead of the 8259 for device interrupts.
- **Uses the old 8259 PIC and PIT**; real machines from ~2005 have the APIC. The local
  APIC is switched on only for MSI (`arch/x86_64/apic.c`). Switch to the local APIC
  timer / IOAPIC later. (`arch/i386/pic.c`)

## Memory
- **i386: RAM above 768 MiB is ignored** ("highmem"). Fine for old PCs with ≤ 768 MiB.
  (`mm/pmm.c`, `arch/i386/include/memlayout.h`)
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
- **System V IPC is what programs have asked for, no more** (`kernel/shm.c`, `sem.c`,
  `msg.c`): shared memory (MIT-SHM), semaphores and message queues, through `ipc()` (117)
  and the direct calls. Not there: IPC_INFO, SHM_INFO, SHM_STAT and `/proc/sysvipc`
  (so `ipcs` lists nothing), SHM_LOCK (there's no swap, so nothing to lock), the
  pre-`IPC_64` struct. At most 256 segments, and `shmget` with a key looks at all of
  them. Their pages count against the RAM-file limit (half of RAM). A first touch past
  that limit is SIGSEGV, where Linux gives SIGBUS.
- **RAM above 2 GiB holds only programs' private pages** (x86_64, since
  2026-10-13: `pmm_alloc_user`, `include/pmm.h`). The kernel's own memory, the page
  cache, tmpfs and memfd files, shared memory and ELF segments stay in the first
  2 GiB (the low zone, which every device can DMA into); on a 16 GiB machine
  those can run out while 14 GiB are free. Next steps: page-cache and RAM-file
  pages in the high zone for drivers that do 64-bit DMA. The high zone's share
  counts take 2 bytes per high page, in low RAM (7 MiB for 14 GiB).
  Not yet tried on the MSI. The UEFI loader keeps at most 96 memory-map entries
  after merging; RAM described past that would be ignored.
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
- **Fixed tables: 512 threads and 256 processes on x86_64, 256 and 128 on i386**
  (`THREAD_MAX`, `PROC_MAX`). A process's fd table starts at 64 and doubles up to 1024
  (`PROC_FILES`); memory areas grow up to `MM_AREAS_MAX` (65536, Linux's limit).
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
- **A slot goes to whichever thread has its owner's name** (`thread_find` in
  `kernel/sched.c`); the owners come from the kernel config (`kernel/kconfig.c`).
  Binding by program at `execve` is designed in `docs/TIME-SLOTS.md`; what is
  missing is under Kernel config below.
- **A slot owner can't run longer than 1 ms at a time, and overruns aren't reported.** It's
  simply preempted at the slot end. Real-time systems usually log or count overruns.
- **The first slot after `sched_start()` is shorter than 1 ms** (the frame starts mid-tick).

## Processes, signals, terminal (step 8)
- **`ppoll`/`pselect6` cut a wait longer than ~24.8 days to that** (the wait counts
  in 32-bit ms, `ts_ms` in `fs/poll.c`); it then ends early, as a timeout.
- **Job control** (since 2026-10-13: `kernel/signal.c`; SIGTTOU since 2026-10-14):
  stop signals stop the process, SIGCONT continues it, `wait4`/`waitid` report both
  (WUNTRACED, WCONTINUED), `ps` shows T, a terminal stop doesn't reach an orphaned
  group; a background job reading its terminal gets SIGTTIN, and one changing it
  (tcsetattr, tcsetpgrp, tcflush) or writing to it with TOSTOP set gets SIGTTOU (or
  EIO; Linux's `tty_check_change` rules, `drivers/tty.c`). Not yet: SIGTTOU for the
  rarer terminal calls Linux also checks (TCXONC, TIOCSETD, TCSBRK, ...: not
  implemented here); threads asleep in a syscall stop only when it returns; ptrace
  stops. The console
  is a controlling terminal like a pty now (TIOCSCTTY, TIOCNOTTY, TIOCGPGRP only for
  its own session).
- **Syscall restart follows Linux's lists, mostly** (since 2026-10-13: `-ERESTARTSYS`,
  `signal_check`): reads and writes on pipes, terminals, sockets, eventfd, inotify
  and input devices, `wait4`, `accept`, `recv`/`send`, `flock`/`F_SETLKW` and FIFO
  opens run again after a handler with `SA_RESTART` (or when no handler runs);
  `poll`, `select`, `epoll_wait`, `nanosleep`, System V IPC and `connect` say EINTR.
  `futex` waits still say EINTR (Linux restarts them; musl loops anyway). No
  `ERESTART_RESTARTBLOCK` (a restarted sleep with the time left). No core dumps,
  `pselect`/`ppoll` ignore their mask.
- **One terminal**: the console (VGA or framebuffer, keyboard, and the serial port's
  input, which `drivers/serial.c` hands to the console like the keyboard's). There is
  no separate `/dev/ttyS0`; `/dev/ttyN` are all the console. VT100 subset only (no
  scroll regions, no line drawing).
- **`poll`/`select`**: every waiter is woken on any terminal/pipe change (fine for a few
  waiters).
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
  every option (timeouts and keepalive included). TCP and ping to 127.x fail
  (ENETUNREACH): loopback is UDP only. The kernel does DHCP and a DNS A lookup
  itself (A records only). IP options and fragments are dropped. The receive queue holds 32
  packets; the neighbor table holds 8. `make test`'s DNS and HTTP lines need
  a path to example.com. QEMU ignores receives for one second after the
  receiver is turned on; the first transmit waits out the rest of that
  second, including on real hardware if a frame is sent that soon.
- **`uname` says "Linux"** (programs check it to know which syscalls exist); the release
  is "6.1.0-MyOS". `getrandom` is ChaCha20 (`kernel/random.c`); how much entropy seeds
  it is an estimate (see below).
- **The kernel can still read a `PROT_NONE` page that is present** (one that was
  readable before an `mprotect`): such a page only loses its user bit, and with no
  SMAP the kernel's own accesses pass, so `write(fd, prot_none_page, n)` sends its
  data where Linux says EFAULT. A page never touched faults for the kernel too
  (EFAULT), and every program access faults (`mm_fault`, 2026-10-13).
- **ASLR covers programs, not the kernel** (since 2026-10-14). Every exec moves the
  stack, the mmap area (dynamic linker, libraries), a PIE program, the heap and
  i386's vsyscall page by Linux's amounts (`include/user.h`); a program built
  without PIE (ET_EXEC) stays where it was linked, as on Linux. The kernel is
  always at the same address (no KASLR), so a kernel bug's exploit knows where
  kernel code is. 32-bit address space limits i386 to 65,536 places for the
  mmap area and 2,048 (×512 within 8 KiB) for the stack. Turned off only per
  program, with `personality(ADDR_NO_RANDOMIZE)` (lost on a setuid exec);
  there is no `/proc/sys/kernel/randomize_va_space`.
- **Only private, read-only use of a disk file's pages is shared with the page cache**
  (since 2026-10-15: a read fault maps the cache's frame, copy-on-write; docs/performance.md).
  `MAP_SHARED` of a disk file is still a private copy made per page on first touch, and
  since 2026-10-09 a disk file opened for writing can't be mapped shared (ENODEV): writes
  through it never reached the disk. SQLite's memory-mapped I/O falls back to `read()`;
  LMDB, which needs it, can't run. A read-only open still maps, as a snapshot. The rest
  of ROADMAP step 11 (a shared, writable map that the cache writes back) is still to do.
  A private map whose file is truncated keeps the old pages (Linux: SIGBUS past the end).
  Files in the initramfs and in RAM file systems are still copied page by page. Programs are mapped from their file and paged in on touch, on both CPUs
  (x86_64's loader read the whole file into one buffer until 2026-10-07).
- **`myos-mkesp` can't make a small EFI partition** (found 2026-10-17): its FAT32
  geometry comes out too small for the FAT on a 64 MiB partition ("negative count"
  in `finish`). The installer's 512 MiB partitions are fine.
- **Dynamic linking**: one interpreter level; the interpreter is put at the first free
  place in the mmap area (which starts at a random place: ASLR).
- **Arguments + environment: 64 KiB, 1024 entries; paths 255 bytes** (Linux: 128 KiB+, 4096).
- **Syscalls run with interrupts off** (interrupt gate); long ones are capped (reads at
  256 KiB, console writes return short). Needs locks to change.
- **No-execute only on x86_64** (since 2026-10-13): a program's pages run only where
  it asked (PROT_EXEC, an ELF segment marked X); stacks, heaps and data don't. i386
  without PAE has no NX bit, so there every readable page can run. An executable
  stack (PT_GNU_STACK with X) isn't honoured: stacks never run.
- **CPU exceptions become signals** (SIGSEGV/SIGFPE/SIGILL); a handler for them is called
  but the faulting instruction runs again when it returns (like Linux).

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
- **Root on disk (9c)**: on an installed disk (`make hd`) the whole tree is on ext2. On
  a live disk only /data (if present) is; the rest is the initramfs in RAM, lost at
  power-off, at most half of RAM (`-ENOSPC`). After switching to a disk root the RAM tree
  (and the initramfs's 3 MiB) stays in memory unused; an installed disk still loads the
  full initramfs at boot. Files that mke2fs copied from the Mac belong to uid 501 (the
  Mac user), not root. Only the first
  `myos-root` counts. A program is mapped from its file and paged in on touch
  (the page cache, 9c-2); it is no longer copied whole at exec.
- **The disk driver spins up to 2048 status reads** before sleeping on the interrupt (fast
  in QEMU, where waking a halted CPU is slow) — but only when no other thread is ready
  to run; on real hardware that's up to ~2 ms of otherwise idle CPU per seek.
  (`drivers/ata.c`, `POLL_FIRST`)
- **ext2** (`fs/ext2.c`): the first ext2 file system found (a type-0x83 partition or a
  whole disk without a partition table) is mounted at /data; root can mount others
  (`mount -t ext2`) and unmount them. No journal: switching off
  mid-change can leave the disk
  inconsistent (e2fsck repairs it); a file system never unmounted (the root, /data
  at power off: runit's stage 3 doesn't unmount yet) stays "not cleanly
  unmounted". Refused: ext4 extents, 64-bit, flex_bg; mounted read-only: disks with
  checksums or other features we can't keep up to date. A directory we change loses its
  hash index (as with Linux's own ext2 driver). Hard links can't be made, and a file
  with two names (made by Linux) becomes two separate nodes here, whose sizes can drift
  apart until a reboot. Every directory looked at stays in memory (no eviction). File
  data lives in the page cache; mapping a miss costs one indirect-block lookup per 64
  blocks. One lock per file system. Block allocation scans the group's bitmap (bounded by its size).
  Times are 32-bit seconds.
- **No hard links**: a file and its name are one struct, so `link()` returns `-EPERM`
  (what Linux says on FAT). Hard links in the cpio archive become separate files.
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
  (a destination pid other than 0) are refused.

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
- **inotify**: an unmount ends the watches on it (IN_UNMOUNT, IN_IGNORED); its queue is 1024 events
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
  no autobind (an unbound sender is nameless, so it can't be answered);
  shutdown() on a datagram socket says ENOTCONN.
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
    internal one) is listed as "a hub: not supported yet". QEMU adds a hub
    by itself when a controller is down to its last free port: UTM's USB 3
    controller has 4 ports and UTM puts 4 devices on it, so
    `tools/mkutm.sh --usb-stick` gives it 8 (`-global nec-usb-xhci.p2=8`).
  - Unplugging: the kernel says so but keeps the disk; its I/O then times
    out (10 s per command). Its memory is not given back.
  - Only xHCI. Old PCs (the Pentium 4 / M targets) have UHCI + EHCI, so
    their USB sticks don't work yet. Nor do UTM's own USB keyboard and
    mouse: UTM puts them on an EHCI (docs/utm.md; `mkutm.sh --usb-stick`
    adds its own on the xHCI). Intel 7/8-series chipsets (2012-2014)
    start with their USB 3 ports routed to EHCI; Linux switches them over
    (`usb_enable_intel_xhci_ports`), we don't.
  - Bulk and interrupt endpoints: no webcams (isochronous). Mass storage
    and HID only; other classes are listed.
  - HID (`drivers/usb/usb_hid.c`, `drivers/hid/hid.c`, 2026-10-17): keys,
    buttons, relative X/Y and the wheel, into the PS/2 paths (event0/event1);
    absolute pointers (USB touch screens, pen tablets, QEMU's `usb-tablet`)
    into event2, scaled to 0..32767, a touch as the left button. Missing:
    - Multi-touch: a touch screen's first finger points and taps; the others
      are ignored (no pinch or two-finger scroll), and there is no
      `ABS_MT_*` (docs/TOUCHPAD-PLAN.md).
    - Touch screens not on USB: most laptops' are I2C (the GPD P2 Max's is
      a Goodix part, probably on I2C), which needs the I2C and ACPI work of
      docs/TOUCHPAD-PLAN.md and the chip's own protocol. They would feed
      the same event2 (`evdev_touch`).
    - One event2 for every absolute pointer, with no rotation or
      calibration: a panel mounted sideways needs its axes swapped.
    - The Consumer page: volume, brightness and media keys.
    - The keyboard's lights (Caps Lock): no SET_REPORT.
    - Key repeat on the text console: a USB keyboard doesn't repeat by
      itself (PS/2 ones do), and nothing repeats for it. X repeats keys
      itself, so the desktop is fine; Linux does it in its input core.
    - Touchpads' finger reports (precision-touchpad mode,
      `docs/TOUCHPAD-PLAN.md`): a USB touchpad works as a mouse.
    - The parser keeps 24 fields and 8 report IDs per interface and reads
      descriptors up to 1024 bytes (a cut one loses its last fields); 4 HID
      interfaces per device. No boot-protocol fallback for a descriptor it
      can't read.
    - A keyboard unplugged with a key held leaves that key down.
  - Mass storage: bulk-only transport, LUN 0, 512-byte sectors; no UAS (a
    faster USB 3 protocol), no write-protect check, no TRIM.
  - One command at a time per controller's command ring, and a command that
    times out is not aborted (CRCR.CA): the ring may then be stuck.
  - "Wait for the stick we started from" needs the loader's BOOT_FROM_USB
    flag: the x86_64 loader sets it, the i386 one doesn't.

### Display (step 11b, 2026-10-03)
- **A BIOS boot has graphics only on QEMU/UTM's std VGA (Bochs, 1234:1111).** Real old
  PCs and every other card stay on the 80x25 text screen until a VESA boot framebuffer
  is written. (`drivers/gpu/drm/tiny/bochs.c`) A UEFI boot uses the firmware's
  framebuffer (GOP, `drivers/video/simplefb.c`) at the mode the firmware chose.
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
- **The font comes from the BIOS** (`stage2.asm` copies it) or, under UEFI, a 4 KiB font
  file the loader reads; without either, fbcon doesn't start. CP437 glyphs only (no
  Unicode).
- **`ioremap_wc` mappings are never undone**, and must be made before the first process
  starts (process page directories copy the kernel half once). (`arch/i386/pat.c`)

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
  interrupt-driven if a real machine shows it). Movements go to X through
  `/dev/input/event1` (`drivers/evdev.c`), as USB mice's do (`drivers/usb/usb_hid.c`).
- **A PC without a PS/2 controller hung at boot** (found 2026-10-17 with QEMU's
  `i8042=off`, while adding USB keyboards for the GPD P2 Max): such a PC reads 0xFF
  from port 0x64, "a byte waiting", forever, and `keyboard_init` and `mouse_init`
  emptied the controller until it was empty. Fixed: after 16 bytes there is no
  controller (Linux's `i8042_flush`), and the PS/2 keyboard and mouse are skipped.
  Not done: ACPI's "no 8042" flag (FADT `IAPC_BOOT_ARCH`), which Linux also reads.
- **Laptop touchpad is I2C HID, not the PS/2 mouse** (MSI Katana GF66 11UE,
  `docs/hardware-msi-katana-gf66.md`, 2026-10-06; the plan: `docs/TOUCHPAD-PLAN.md`). The pad (Synaptics `06CB:7E7E`
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
  are separate drivers (see Disk above). NVMe runs on the MSI (its Kingston SSD); the
  others haven't been tried on real hardware yet. We don't
  pick a transfer mode ourselves (SET FEATURES, PIIX timing registers): DMA runs at
  whatever mode the BIOS set up, checked by a test read at boot. LBA28 only (128 GiB).

## Found running Chromium (2026-10-08)

- **MAP_SHARED of a disk file** (ext2) is refused for writing and a snapshot read-only
  (Processes above). RAM files (memfd, /dev/shm, tmpfs) are really shared.
- **A RAM file once mapped shared keeps its pages when shrunk** (zeroed), until
  the file itself goes: a mapping may still point at them, and there is no
  reverse map to unmap it. Linux sends SIGBUS past the new end instead.
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
- ~~racetest's two-waiters case could lose under host load~~ (fixed 2026-10-14): the
  sibling was let go just before `waitpid`, so a preemption in that gap let it reap
  the child first. The armed copy now releases the sibling itself (a third argument
  to the test syscall, `kernel/uaccess_test.c`), from inside `waitpid`.

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
- **X's cookie is all-or-nothing** (since 2026-10-13: before, X let in every
  local program of every user). myos-login makes a random cookie per server
  (`/run/myos-login/X0.auth`, root's) and writes it to the user's
  `~/.Xauthority`; without it X answers "Authorization required". Every
  program of the session user has it, and can read keys and take
  screenshots of the whole desktop, as on Linux's X11; separating programs
  from each other needs Wayland (docs/performanceSuggestions.md, Part 2).
- **16 pseudo-terminals** (`PTY_MAX`, `drivers/pty.c`), each with a one-page ring in
  each direction. `/dev/pts` is an ordinary RAM directory, not devpts: the nodes are
  made and removed by hand, and a `mount -t devpts` still fails.
- **`epoll` is an array, scanned** (`EPOLL_MAX` 64 per epoll fd, `fs/epoll.c`): every
  wait walks all of them and calls each file's `poll`. That is the cost X pays per
  event; Linux keeps a ready list. `EPOLLET` is accepted and treated as level
  triggered, and `epoll_pwait`'s signal mask is ignored.
- **`/sys` holds little** (`fs/sysfs.c`): `fb0` (the X server's fbdev driver refuses a
  screen without it), the network devices, the disks, the CPU and the EFI firmware
  flag. No input devices and no uevents, so libinput and udev find nothing to work with.
- **Two-finger scroll needs kernel HID first**, then a userspace gesture
  reader. X only binds `xf86-input-evdev` to event0/event1. The laptop pad
  and the split (kernel vs libinput) are under Drivers; the plan is
  `docs/TOUCHPAD-PLAN.md`.
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
  `/usr/share/myos/wallpapers`), Mouse & Keyboard the pointer's size, Users & Groups
  can add a user, and Kernel saves the kernel config (which the kernel doesn't read
  yet); Network, About, Storage and Date & Time only show. The network page shows
  only the address the system routes from (UDP connect + getsockname) and the DNS
  server; the `SIOCGIF*` ioctls exist now and could tell it more. The processor name
  comes from `cpuid`: the code predates `/proc/cpuinfo`. Resolution can't be changed
  from it: X's fbdev driver can't switch modes while running.
- **The desktop lists `~/Desktop`** (`myos-desktop`; the folder is created if it
  is missing), in the same left-hand
  column as the Recycle can (`myos-trash`, which keeps the fifth place).
  Double-click opens a file the way My Files does. Right-click on the
  wallpaper offers New Folder and Change Desktop Background…, which opens
  Settings > Wallpaper. The Start menu programs are not planted as icons;
  they stay on the Start menu (`live/myos-menu`):
  Terminal, My Files, Web Browser (`myos-browser`: Chromium, or Firefox where there
  is no Chromium), WritePerfect, AbiWord, Preview,
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
  Firefox is built with SSE2, which a Pentium 4 / Pentium M has. `/proc` has
  `cpuinfo`, `meminfo` and `/proc/<pid>` now; what still stops it isn't known
  (not retried since they came). A GUI start dies in
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
  It watches the folder with inotify (since 2026-10-13: a change made elsewhere
  shows within ~0.1 s, a file written over in place too); without inotify it
  falls back to looking every 2 s. Disks lists the ext2/ext4/FAT mounts in
  `/proc/mounts`; it doesn't notice a disk mounted while it's open (F5).
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
- **The kernel's write-xor-execute is x86_64 only** (since 2026-10-13: its code
  read-only and the only kernel memory that runs; read-only data read-only; the rest
  — data, heap, stacks, device registers, the direct map — not runnable). The i386
  kernel is still one writable, runnable image: no NX without PAE, and 4 MiB pages.
  The direct map's view of x86_64's code is read-only too.
- **The stack pool keeps its pages.** A window's pages stay with it after
  its thread dies, for the next thread: the pages held are the most
  threads ever alive at once (16 KiB each on x86_64), never given back.
- **MEMDEBUG has no red zones.** It catches a write to freed memory (pages
  and kmalloc objects), not an overrun into a neighbour that is in use.
- **The stack canary is one value for the whole kernel**, chosen once at
  boot (since 2026-10-13: `-fstack-protector-strong`, entry.asm). Linux
  gives each thread its own; a leaked canary here works on every stack.
  On the old PCs (no RDRAND) it comes from the cycle counter alone: how
  long the BIOS and the boot loader took, which varies by millions of
  cycles, but isn't a real random source. On i386 it isn't read-only either
  (no read-only kernel pages there yet). Two hot entry points go without
  one on purpose: `sysenter_dispatch` (its only buffer is a 4-byte word
  `get_user32` fills) and `interrupt_dispatch` (a crashing program's
  report, which has a name buffer, is kept out of line so it doesn't add
  one there).
- SMEP and SMAP are on (x86_64, CPUs that have them, since 2026-10-13): the
  kernel can't run a user page, nor touch one outside the user-copy
  routines and exec's stack and ELF setup. The old PCs (and i386) have
  neither.

## Disk files and /proc per process (2026-10-09)

- **Reading two partition tables at once isn't safe.** `partition_scan`
  uses one static sector buffer. That only matters if BLKRRPART runs while
  a USB disk is being plugged in.
- **Only the first 16 sd disks get correct numbers.** sdq and later need
  Linux's majors 65–71; ours put minor > 255 into an 8-bit rdev.
  BLOCKDEV_MAX is 16, so this can't happen yet.
- **No BLKDISCARD (TRIM).** mke2fs carries on without it.
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
  2026-10-09. Seen again 2026-10-13 in about 1 run in 3, 25-50 s after
  starting it on the live disk: a thread pool thread reads a pointer with
  bit 44 set (0x65555a7d0000, which is Chromium's 0x55555a7d0000 + 2^44) at
  chromium+649c2f7, then the crash handler faults or hits its int3. The
  same with and without NX and the kernel's W^X (an A/B on the committed
  version), so not caused by them. No `ptrace` (crashpad can't take its dump), and no
  `/proc/<pid>/task` for other processes.
- **A deleted file that is still open reports `st_nlink` 1** (Linux: 0). SQLite only
  logs a warning about it.

## The installer (2026-10-10)

- **UEFI: only onto a disk without an EFI partition** (or by erasing).
  Next to Windows it would have to add EFI/MYOS to Windows' FAT partition,
  and MyOS's FAT driver only reads (the helper refuses, before writing).
- **BIOS (2026-10-17): a whole disk only.** MyOS has no boot menu to start
  another system, so it never shares a BIOS disk (the window and the helper
  say so). An MBR: disks up to 2 TB, four primary partitions (boot area,
  myos-root, myos-config, myos-data). The boot area is 64 MiB, of which
  today's stage 2, kernel and initramfs use 3.6 MB.
- **No boot entry is written** (no efibootmgr / SetVariable). The firmware
  finds EFI/BOOT/BOOTX64.EFI by itself on most PCs. On others, pick the
  disk in the boot menu (F11 on the MSI).
- **ssh login is off on an installed system.** Turn it on with
  `doas ln -s /etc/sv/sshd /var/service/` (a new host key is made then).
- **The window's install flow is not clicked through in QEMU**:
  `make test-install` (2026-10-17) runs the window's helper with a written
  plan, then starts the result and logs in (UEFI: NVMe, SATA, USB and the
  P2 Max's shape; BIOS: IDE and SATA). The window's pages themselves are
  only tried by hand.

## Kernel config (2026-10-10)

- **The kernel reads the config, but uses only its slot owners** (since
  2026-10-13: docs/KERNEL-CONFIG.md §10 steps 1–3). The mode, the latency
  budget and the power savings are read, checked and logged, and do nothing
  yet (steps 6 and 9); a new config takes effect at the next start. With no
  good copy the kernel runs its built-in one (`config/slots.conf`, realtime).
- **No `myos-taskd`, no binding at `execve`, no OS class yet.** Slots
  still go to a thread with the right name (docs/TIME-SLOTS.md).
- **Behind a hybrid MBR (no 0xEE entry) the kernel never reads the GPT**,
  so GPT names are invisible there. The live/UTM images list `myos-config`
  in the MBR too, as type 0xDA, and the kernel takes an unnamed 0xDA
  partition as a candidate (its contents must pass every check). The
  installer makes `myos-config` (GPT, 1 MiB after myos-root, cleared).
- **Only GPT and hybrid-MBR disks have it.** The i386 live disk (MBR,
  BIOS) needs the config in its 0x7F boot area (docs/KERNEL-CONFIG.md §8),
  which `tools/mbr-partition.sh` sizes with no room to spare: i386 always
  runs its built-in config.
- **Some savings only the kernel can confirm.** Deep idle (ACPI _CST), C1E,
  PCIe ASPM, USB autosuspend, SATA link power and disk spin-down are
  switchable in Settings and marked "checked when MyOS starts": a program
  can't see them, and the kernel will skip what isn't there.
- **MyOS's /proc/cpuinfo leaves out flags** such as `est`, `acpi` and `tm`,
  so Settings reads CPUID itself.
