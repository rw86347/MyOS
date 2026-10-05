# Known gaps

Shortcuts, limits and "later" notes in the current code, in one place. Remove an entry
when it's fixed. File references point at the comment that mentions the gap.

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
- **UEFI loader starts the existing 32-bit kernel** (`make uefi` / `make live-uefi`).
  The x86_64 kernel is a long-mode hello at physical 1 MiB (no paging.c,
  no portable `kernel/` yet). It paints GOP as a 64-bit pointer (MSI
  at `0x400000000` worked). `make ARCH=x86_64 live-uefi` builds the
  userland disk; the kernel does not load the initrd or exec
  myos-login. GOP above 4 GiB on i386 is mapped with PSE-36
  for the kernel console (32-bit virtual address, 36-bit physical in
  the PDE). User `mmap` of `/dev/fb0` cannot name that address (32-bit
  PTEs), so `simplefb` keeps an 8 MiB RAM shadow and, on the first mmap,
  blits it every 16 ms. Until then fbcon writes the real GOP. GPT is
  read only far enough to find the ESP and a Linux `myos-root` (16
  entries).
- **No USB mass storage.** A live UEFI stick's `myos-root` is not mounted
  on a machine whose only disk is the stick itself (xHCI only). The live
  UEFI ESP ships a login-only initramfs (base+init+desktop, not Firefox /
  gcc / office) so process 1 is runit → `myos-login` from RAM. The Pentium
  image still uses the small BusyBox initrd.

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
- **Wi-Fi is a pane with nothing behind it.** Settings shows Wi-Fi where the Mac does,
  off and greyed out: MyOS has no Wi-Fi driver. It reads `/proc/net/wireless`, which
  MyOS doesn't have, so it always says "No Wi-Fi hardware found".
- **Nothing reads the syslog socket.** `net/unix.c` has UNIX sockets now, but no
  syslog daemon runs, so login failures still go nowhere.
- **64 fds per process** (`PROC_FILES`). bash still moves its terminal to fd 255 for
  job control and gets `EBADF`, so the login shell has no Ctrl-Z / `fg`. `cloexec`
  is a 64-bit mask; going past 63 needs a bitmap. `select()` still only sees fds
  0–31 (Linux `fd_set` on i386 is 1024 bits; Firefox uses poll/epoll).
- **No permission checks.** Processes have real users now (uid, groups, setuid exec),
  but `open`, `unlink`, `kill`... don't check them: user `myos` can write `/etc/shadow`.
  The scheduler also ignores nice values (`setpriority` stores them).
- **/proc is `/proc/self/fd` and `/proc/self/exe`** (`fs/procfs.c`). No `/proc/<pid>`,
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
- **No device manager** (udev/mdev): the kernel makes the /dev nodes itself, and
  with no /sys udev would have nothing to read.

## CPU / interrupts
- **Uses the old 8259 PIC and PIT**; real machines from ~2005 have the APIC. Switch to the
  local APIC timer / IOAPIC later. (`arch/i386/pic.c`)
- **No double-fault task gate.** A kernel stack overflow becomes a triple fault (reboot)
  instead of a message. (`arch/i386/interrupts.c`)

## Memory
- **RAM above 768 MiB is ignored** ("highmem"). Fine for old PCs with ≤ 768 MiB.
  (`mm/pmm.c`, `arch/i386/include/memlayout.h`)
- **Kernel is one read/write/execute blob.** Code isn't write-protected and data isn't
  no-execute. The Makefile comment says to split permissions once paging is on — paging
  *is* on now, so this is due. (`Makefile`, `arch/i386/linker.ld`)
- **Frame allocator is a linear bitmap scan.** Slow with lots of RAM; fine for now. (`mm/pmm.c`)
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
  limit is SIGSEGV, where Linux gives SIGBUS.
- **`MAP_SHARED | MAP_ANONYMOUS` is still private** after `fork`. The `VM_SHARED` areas
  shared memory uses could carry it too, when a program needs it.

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
- `select()` reads and writes only the first 32 bits of each `fd_set`, so bits
  32-1023 of a caller's set are never cleared. Harmless while `FD_MAX` is 32
  and programs call `FD_ZERO`, but it is not what POSIX says.
- `ppoll`/`select` compute the timeout as a signed `tv_sec * 1000`, which
  overflows on a large value and is then treated as "no timeout", turning a
  short wait into an indefinite one. Interruptible, so it only affects the
  caller.
- **No job control stops**: SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU are ignored, so Ctrl-Z does
  nothing and `fg`/`bg` can't work. No SIGTTOU/SIGTTIN for background terminal access.
- **No syscall restart**: a signal makes a sleeping syscall return `-EINTR` even with
  `SA_RESTART`. No `sigaltstack`, no core dumps, `pselect`/`ppoll` ignore their mask.
- **One terminal**: the console (VGA + keyboard). Serial input isn't read; `/dev/ttyN` are
  all the console. VT100 subset only (no scroll regions, no line drawing).
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
- **Sockets: UDP and raw ICMP only** (step 10). `SOCK_STREAM` returns
  EPROTONOSUPPORT, so `wget` does not work yet; no AF_PACKET, so no `udhcpc`; no
  AF_INET6 or AF_UNIX. Each socket queues 8 datagrams and drops the rest.
  `setsockopt` accepts and ignores every option. Sending to 127.x fails
  (ENETUNREACH): there is no loopback route. The kernel can also do a DNS A
  lookup itself, and one TCP client connection. The address is
  the QEMU user-net default, not DHCP. TCP is one connection, client only,
  one segment in flight, no out-of-order queue, no listen. DNS is A records
  only. IP options and fragments are dropped. The receive queue holds 32
  packets; the neighbor table holds 8. `make test`'s DNS and HTTP lines need
  a path to example.com. QEMU ignores receives for one second after the
  receiver is turned on; the first transmit waits out the rest of that
  second, including on real hardware if a frame is sent that soon.
- **`uname` says "Linux"** (programs check it to know which syscalls exist); the release
  is "6.1.0-MyOS". `getrandom` is not cryptographic (no entropy source).
- **`mprotect(PROT_NONE)` doesn't hide pages**: present user pages stay readable on x86
  without PAE (`PROT_NONE` would need unmapping).
- **No address-space randomisation**: static PIEs always load at 0x00400000.
- **Every program runs as root** (uid 0).
- **File mappings are private copies** (made per page on first touch), so `MAP_SHARED`
  file mappings behave like `MAP_PRIVATE`: a program writing through one doesn't change
  the file. A program written at run time is copied into one buffer for the ELF loader
  (initramfs programs are read in place). Disk files are copied from the page cache.
- **Dynamic linking**: one interpreter level; the interpreter is put at the first free
  place in the mmap area (no randomisation).
- **Arguments + environment: 64 KiB, 1024 entries; paths 255 bytes** (Linux: 128 KiB+, 4096).
- **Syscalls run with interrupts off** (interrupt gate); long ones are capped (reads at
  256 KiB, console writes return short). Needs locks to change.
- **Execute permission isn't enforced** (no NX bit without PAE).
- **CPU exceptions become signals** (SIGSEGV/SIGFPE/SIGILL); a handler for them is called
  but the faulting instruction runs again when it returns (like Linux).

## Disk / file systems
- **One command at a time per disk** on every driver (IDE, AHCI, NVMe, virtio): no
  NCQ, no NVMe queue depth > 1, no MSI/MSI-X. Read-ahead no longer waits (the next
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
- **MBR partitions only**: no extended/logical partitions, no GPT. (`fs/partition.c`)
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
  Times are 32-bit seconds. Only on IDE disks (the USB-stick image has no /data).
- **No hard links**: a file and its name are one struct, so `link()` returns `-EPERM`
  (what Linux says on FAT). Hard links in the cpio archive become separate files.
- **No named pipes** (`mkfifo`: `mknod` of a FIFO returns `-EPERM`) and no block device
  nodes. (`kernel/sys_fs.c`)
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
- **The "USB stick" is an IDE disk in QEMU**: there's no USB stack.

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
- **`AF_PACKET` and the interface ioctls still do not exist**, so BusyBox
  `udhcpc` cannot run even though the kernel now does the same job. If a
  user-space client ever matters, that is what it needs — along with a way for
  user space to *set* the address, which also does not exist.
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
- **`sendfile64` (syscall 239) is not implemented.** BusyBox's `cp` and `cat`
  try it whenever the destination can take it, fall back to read/write
  correctly, and leave one `syscall 239 not implemented` line in every boot
  log. Harmless but it is a real syscall that real programs use, and doing it
  properly (straight out of the page cache) would make file copies faster
  rather than merely quieter.

## Drivers

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
- **All drivers are in the kernel**; the plan is to move most to user space. (`docs/DESIGN.md`)
- **Network security, not yet done** (needed before "bullet proof"; the threat model and
  the plan are in `docs/security.md`):
  - No CSPRNG (`getrandom` isn't cryptographic). Everything below needs one first.
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
  - `bind()` on a TCP or raw socket returns 0 and does nothing, so a program that
    asks for a source port doesn't get it. `setsockopt` accepts and ignores
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
    hosts* is gone, but Linux requires CAP_NET_RAW for the socket itself and
    MyOS cannot: every id is 0 and `setuid` is a stub, so the check would be
    granted to everything and would only look like security.
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
- **TCP** (step 10e): client side only, no listen/accept. No congestion control
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
- **`racetest`'s `wait` checks fail under host load.** They assert that an interleaving
  *happened*, so when the Mac is busy — another QEMU running in the next window — the
  sibling does not get scheduled in time and four checks fail. Nothing is wrong with the
  kernel when that happens, but it is indistinguishable from a real failure at a glance,
  which is the thing a test must not be.

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
- **Settings is mostly read-only.** Displays changes the scale and Mouse & Keyboard
  the pointer's size; Network, About,
  Storage, Date & Time and Users & Groups only show. The network page shows the
  address the system routes from (UDP connect + getsockname) and the DNS server:
  there are no interface ioctls (`SIOCGIF*`) to ask for more. The processor name
  comes from `cpuid` (no `/proc/cpuinfo`). Resolution can't be changed from it:
  X's fbdev driver can't switch modes while running.
- **Most of the mockup's programs do not exist**: no network tool or help, so
  there are no desktop icons and the Start menu (`live/myos-menu`) lists
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
  documents still use their type icon. No drag and drop, no cut/move (copy, then delete), no
  properties window, no "Empty Recycle Bin" (the bin is the freedesktop one,
  `~/.local/share/Trash`; open it with Ctrl+H, then `.local/share/Trash/files`).
  Without inotify it looks at the folder and its files every 2 s; in folders of
  more than 1000 files only the folder's own time is watched, so a file written
  over there needs F5. Disks finds other disks by `statfs` on the folders in `/`
  (no `/proc/mounts`).
- **Evince draws its own title bar** (GTK's header bar), not the Golden Hour one, and
  warns that it can't watch its settings file: there is no inotify (syscalls 291/332),
  so GLib's file monitors fail. Nothing visible breaks.
  Its entries come from `MENU_LEFT` / `MENU_RIGHT` in `live/theme/make-live-theme.py`;
  "All Programs" waits for something worth listing.
- **The Start menu is a fixed picture.** Entries are drawn at build time, so adding a
  program means rebuilding the theme; nothing reads `.desktop` files. The user's name
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
  wasn't in the first 256 bytes), and AbiWord also logs `syscall 328 not implemented`
  (eventfd2; GLib falls back to a pipe).
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
