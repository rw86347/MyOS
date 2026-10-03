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

## Threads / scheduler
- **No locks.** Preemption is on, so two threads can be inside the same code at once.
  `kmalloc`/`kfree` and the disk drivers aren't protected. (Console output is: each piece
  of text is written with interrupts off.) Syscalls run with interrupts off, so user programs
  can't collide; kernel threads could. Fix: `irq_save` sections (one CPU) or a mutex.
- **Round-robin mode still exists** for boot and the thread self-tests (before
  `sched_start()`). (`kernel/sched.c`)
- **Dead threads are reaped lazily**: their slot and stack are only freed when
  `thread_create` needs the slot. (`kernel/thread.c`)
- **Fixed tables: 64 threads, 64 processes, 32 fds and 32 memory areas per process.**
  (`THREAD_MAX`, `PROC_MAX`, `PROC_FILES`, `MM_AREAS`)
- **One thread per process**: `clone(CLONE_THREAD)` returns `-ENOSYS`, no `futex`.
  `vfork` is plain fork (copy-on-write makes it cheap), and `clone(CLONE_VM)` gets a copy.
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
  NCQ, no NVMe queue depth > 1, no MSI/MSI-X. Read-ahead is synchronous: the reader
  waits for the whole window (up to 128 KiB) instead of the next window being read
  while it copies. (`drivers/ahci.c`, `nvme.c`, `virtio_blk.c`)
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
  in QEMU, where waking a halted CPU is slow); on real hardware that's up to ~2 ms of CPU
  per seek. (`drivers/ata.c`, `POLL_FIRST`)
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

## Drivers
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
  - `parse_a` takes the first A record in a DNS answer without checking which name
    it belongs to (RFC 5452 response matching). The question section is checked, the
    answer records aren't. Fixing it naively breaks CNAMEs, because there the A
    record's owner is the chain target — see `docs/security.md`.
  - A forged ARP reply can still win the race on a request we did send (unsolicited
    ones are now ignored entirely). Not fixable on a shared segment.
  - `ip_send` checks the next hop, not the destination, so we will still *send* to a
    broadcast address even though we no longer receive one.
  - Nothing in `net/` has been fuzzed, and no test attacks the stack: the fixes are
    verified only against the happy path (`make test` ping/DNS/TCP).
  - TCP initial sequence number is `timer_ms()` + a constant: guessable, so a
    connection can be hijacked or reset blind. Linux: keyed hash (RFC 6528).
  - Ephemeral UDP ports are handed out in order from 49152, and the kernel's own
    DNS uses port 53000 and sequential IDs: open to DNS cache poisoning.
    Linux randomizes the port (RFC 6056).
  - IP ID is a global counter (leaks traffic rate). Linux: per-destination hash.
  - Any program can open `SOCK_RAW`. Linux requires CAP_NET_RAW. MyOS has no
    users or capabilities yet.
  - No Spectre v1 barriers on user-controlled indexes (syscall number, fd) and no
    retpolines; Pentium 4 and Pentium M are affected. Linux 6.1's cost for these
    is under 1% of the UDP path in instruction count (docs/performance.md); on a
    real P4 it is more and must be measured.
  - The parsers check lengths and checksums but have never been fuzzed.
- **TCP zero window stalls forever.** `output()` sends nothing when the peer advertises a
  window of 0, and because no segment goes out the retransmit timer is never armed, so
  the data sits in the send ring and nothing wakes it. `tcp_write` has already told the
  caller it was accepted. Needs a persist timer and zero-window probes.
- **TCP** (step 10e): client side only, no listen/accept. No congestion control
  (no slow start): at most 4 segments in flight, a fixed limit, not a response to
  loss. No out-of-order queue, SACK, window scaling (16 KiB window), timestamps,
  or RTT estimate (retransmit timer 250 ms doubling to 4 s, 8 tries). No Nagle
  (every write goes out at once, like TCP_NODELAY). TIME_WAIT is 2 s, not 60.
  At most 16 connections. Closing with unread data sends RST (as Linux does).
  Local ports are sequential (see network security above).
- **Disk DMA** (9d): bus-master IDE (PIIX/ICH legacy mode); AHCI, NVMe and virtio-blk
  are separate drivers (see Disk above). None tried on real hardware yet. We don't
  pick a transfer mode ourselves (SET FEATURES, PIIX timing registers): DMA runs at
  whatever mode the BIOS set up, checked by a test read at boot. Not yet tried on
  real hardware. LBA28 only (128 GiB).
