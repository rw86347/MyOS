# MyOS Roadmap

Target first: 32-bit x86 (i686) in QEMU. Real hardware and ARM come later.
Mark a step `[x]` only when its "Done when" check works.

## Phase 1 — Boot and basic kernel
- [x] **0. Toolchain** — cross-compiler (`i686-elf-gcc`), QEMU, GDB, Makefile.
  Done when: `make` produces a kernel binary.
- [x] **1. Boot & print** — our own BIOS MBR bootloader: load kernel from disk, enable A20, GDT,
  enter protected mode; kernel writes to VGA text buffer and serial port. (See `docs/DESIGN.md`.)
  Done when: QEMU shows **"Hello world from MyOS!"**.
- [x] **2. CPU tables & interrupts** — GDT, IDT, exception handlers, PIC remap, PIT timer, PS/2 keyboard.
  Done when: a fault prints a message instead of rebooting; typed keys echo to the screen.
- [x] **3. Memory** — BIOS E820 memory map (read by our MBR), physical page allocator, paging with
  the kernel in the higher half (0xC0000000, Linux's 3G/1G split), kernel heap (`kmalloc`/`kfree`).
  Done when: allocate/free tests pass and paging is on.
- [x] **4. Threads & scheduler** — kernel threads, context switch, then the **time-slot scheduler**:
  20 × 1 ms slots, slots 1 and 10 for the system, named tasks assigned to slots from a config file.
  Done when: two named threads print in their assigned slots (e.g. "brakes" in 4,9,14,19).

## Phase 2 — Running Linux programs
- [x] **5. User mode & syscalls** — ring 3, TSS, `int 0x80` handler using **Linux i386 syscall numbers**.
  Done when: a user-mode program calls `write()` and `exit()`.
- [x] **6. ELF loader** — load static ELF executables.
  Done when: an unmodified static musl "hello world" runs. (Alpine's static BusyBox does.)
- [x] **7. VFS + initramfs** — virtual file system, cpio initramfs loaded by our stage-2 loader.
  - [x] 7a: stage-2 loader (kernel at 1 MiB, initramfs after it, no size limit), 4 MiB pages.
  - [x] 7b: cpio unpacked into an inode tree (zero-copy), O(1)-per-component lookup, fds,
    `open`/`openat`/`read`/`close`/`statx`/`lseek`/`_llseek`; programs run from `/bin`.
  Done when: `open`/`read`/`close`/`stat` work on initramfs files.
- [x] **8. Processes & TTY** — `fork`, `execve`, `wait4`, `pipe`, `dup2`, signals, TTY line discipline.
  Done when: **a BusyBox shell runs** and `ls`, `cat`, `echo | wc` work. (Big milestone!)
  - [x] 8a/8b: processes, per-process address spaces, copy-on-write fork, demand paging,
    brk/mmap/munmap, exit/wait4, run queue, sleeping (wait queues).
  - [x] 8c: execve (envp, `#!` scripts). 8d: pipes, dup2/dup3, fcntl, getdents64,
    chdir/getcwd, symlinks, writev/readv, /dev/null, /dev/zero, /dev/console.
  - [x] 8e: terminal (canonical/raw, termios ioctls, Ctrl-C, VT100 on VGA), poll/select.
  - [x] 8f: signals (sigaction, masks, delivery frames, sigreturn, kill, groups, SIGCHLD,
    SIGPIPE, sigsuspend), process groups and sessions.
  - [x] 8g (kernel): TLS (set_thread_area), uname, ids, clocks (CMOS), nanosleep, limits,
    static-PIE loading, clone, *at() with dirfd, mprotect.
  - [x] 8h: BusyBox (Alpine's static i386 build, unmodified) in the initramfs; /init is a
    BusyBox shell script that starts a login shell. `make test-shell` types commands.
  - [x] 8i: writable files in RAM: create, write, append, truncate, mkdir, rm, rmdir, mv,
    ln -s, chmod, chown, touch, mknod, statfs; deleted-but-open files live on; `rm -r`
    while reading a directory. Done: `make test` (fstest) and `make test-shell`
    (`mkdir -p`, `>`, `>>`, `cp`, `mv`, `sed -i`, `rm -r`).
  - [ ] 8j: /proc (`ps`, `top`, `free`, `uptime`), `mount`/`umount`, /proc/mounts (`df`).
  - Later, as programs need them: job control stops (Ctrl-Z, `fg`, `bg`), named pipes,
    `alarm`/`setitimer`, `times`/`getrusage`, `reboot`, hard links.
- [ ] **9. Disks & file systems** — native drivers (see `docs/DESIGN.md`), old PCs first.
  Done when: files survive a reboot.
  - [x] Done early (polling, in-kernel): block device layer, ATA PIO read/write, MBR partitions,
    FAT32 read-only (long names, path lookup, file reads). Test disk: `make usb`; `make test`.
  - [x] 9a: IDE on interrupts (16 sectors per IRQ, polling fallback), disks found at every
    boot, a block cache (256 blocks, written back every ~1 s; `sync`/`fsync`).
  - [x] 9b: a data disk with ext2: read and write, mounted at /data at boot. Done: a file
    in /data survives a restart (QEMU and UTM); Linux's e2fsck finds the disk clean after
    every test run.
  - [x] 9c-1: the root file system on ext2: the kernel mounts the partition labelled
    myos-root as "/" (live disks without one run from the initramfs, as before), puts
    /dev on it, and mounts myos-data at /data. `make hd` / `run-hd` / `test-hd` (two
    boots; e2fsck on the root partition). Everything survives a restart.
  - [x] 9c-2: a page cache: programs and libraries run from disk without copying whole
    files. The entry page is faulted in at exec; the rest, and one page of
    read-ahead, on first touch. `make bench` (RAM root): /init + bash CPU work
    6144 µs → 1204 µs. The 18.7 ms disk-root figure is from before this and was
    not re-measured.
  - [ ] 9c-3: the live disk's installer (partition, mke2fs, copy, boot area), raw block
    devices (/dev/hda...), and the UTM VM on the installed layout.
  - [x] 9d (part): PCI scan + bus-master DMA (PIIX/ICH), reads and writes, PIO
    fallback. 64 KiB read: 1 250 instructions of CPU work (PIO: 41 788).
  - [x] 9d (rest): AHCI (SATA), NVMe and virtio-blk drivers, all scatter-gather DMA
    with interrupts; IRQ sharing. `read()` goes through the page cache (4-way,
    sized from RAM, read-ahead up to 128 KiB in one command); writes cluster into
    one command per run of blocks. `tools/diskbench.sh` (MyOS vs Linux 6.1 on all
    four types) and `tools/test-disks.sh` (self-tests on AHCI/NVMe/virtio).
  - [x] 9d-2: writes go through the page cache (dirty pages written back in runs of
    up to 128 KiB); the cache may use all free RAM and gives clean pages back when
    memory runs out. 64 KiB write ~130 000 → ~100 800 instructions on every type.
    Sizing math in `docs/performance.md` ("Cache sizing").
  - [ ] 9d-3: several commands in flight (NVMe queue depth, virtio chains, AHCI
    NCQ) and read-ahead that runs while the program copies; driver limits
    (several disks per driver, LBA48, AHCI BIOS hand-off).
  - [ ] 9e: FAT32 in the VFS: read, then write (USB sticks, swapping files with a Mac/PC).
    Read is in: the first FAT32 partition is mounted at /mnt (read-only). Write,
    which has to allocate clusters and update the FAT, is still open.
  - Later: ext4 read, exFAT, ext4 write (needs a journal). USB disks need a USB host
    controller first (UHCI/EHCI on old PCs, xHCI on the Pocket 4). NTFS is out of scope.

## Phase 3 — Networking & beyond
- [ ] **10. Network** — Linux's own NIC drivers through the Linux-API shim (decided
  2026-10-02, `docs/DESIGN.md`): first `e1000` (QEMU's card, no firmware), then `e100`,
  `8139too`, `r8169` for old PCs. TCP/IP (lwIP or our own) and the socket syscalls.
  Done when: `ping`, `wget` and BusyBox `udhcpc` work in QEMU.
  The stack above the driver follows Linux's shape (`sk_buff`, sockets, the same
  driver callbacks). Decided 2026-10-03, `docs/DESIGN.md`. Not lwIP.
  - [x] 10a: the driver boundary and a PCI scan. `sk_buff`, `net_device`
    (`ndo_start_xmit`), NAPI, loopback. QEMU's e1000 (8086:100e, IRQ 11) is
    found. `make test`: `net: loopback delivered`, `pci: e1000 8086:100e`.
    No Linux driver source yet (GPLv2; no MyOS licence file).
  - [x] 10b: our own e1000, ARP, and ICMP. QEMU user-net address 10.0.2.15/24.
    `make test` greps `net: ping 10.0.2.2 ok` (the gateway answered). No
    sockets. The shell's `ping` still cannot open a socket.
  - [x] 10c: UDP, DNS, and a TCP client. `dns_resolve` asks 10.0.2.3 for an
    A record. One TCP connection can handshake, send, and receive.
    `make test` greps `net: dns example.com is ` and
    `net: tcp example.com HTTP ok` (needs a path to the internet).
    No listen socket, no DHCP, no socket syscalls.
  - [x] 10d: UDP and raw ICMP sockets (`net/socket.c`: socket, bind, connect,
    sendto/recvfrom, sendmsg/recvmsg, the name calls, socketcall), `/etc/resolv.conf`,
    and `alarm`/`setitimer`. BusyBox `ping -c 2 10.0.2.2` works, and musl resolves
    names. `make test-shell` greps both.
  - [x] 10e: TCP sockets, client side (`net/tcp.c`: one connection per socket,
    16 KiB rings, up to 4 segments in flight, MSS option, backed-off retransmits)
    and a network thread for receive and TCP timers. BusyBox `wget` works;
    `make test-shell` greps `wget ok`.
  - [ ] 10f: DHCP (`udhcpc`): AF_PACKET, interface ioctls, a settable address.
- [ ] **11. Fuller Linux ABI** — `mmap`, `brk`, dynamic linking (`ld.so`), more syscalls as programs need them.
  Done when: ordinary dynamically linked GNU programs run.
  - [x] Pulled forward (2026-10-02): dynamic linking — the kernel loads a program's
    interpreter (PT_INTERP: musl's ld.so), file-backed mmap (demand-paged from the
    initramfs). Alpine's unmodified, dynamically linked bash runs as the login shell.
- [ ] **11b. Graphics** (after the network: X needs sockets too) — the boot loader picks a
  VESA graphics mode (UEFI GOP on the Pocket 4): a framebuffer, marked write-combining.
  A pixel console with big, scalable text; PS/2 mouse; `/dev/fb0`, `/dev/input/event*`,
  AF_UNIX sockets. Then Alpine's X server (fbdev), a small window manager, `xterm`.
  Done when: `xterm` windows can be moved with the mouse. No chip-specific video driver.
  - [x] PS/2 mouse driver (IRQ 12, IntelliMouse wheel); `make test` moves and clicks it
    through QEMU's monitor.
- [ ] **12. Real hardware** — boot from USB/CD on an old Pentium 4 / Pentium M machine.
- [ ] **12b. GPD Pocket 4** (`docs/hardwareRequirements.md`) — `arch/x86_64`, a UEFI loader,
  ACPI, PCIe with MSI/MSI-X; then NVMe (its boot SSD, native driver) and xHCI; then the
  Linux drivers through the shim (`r8169` for its Ethernet, `amdgpu`, ...).
- [ ] **13. WiFi** — pick ONE chip with open docs (e.g. Atheros AR5xxx / ath5k), port driver + firmware loading,
  802.11 + WPA2 (run `wpa_supplicant` in user space).
  Done when: connects to a WPA2 network.
- [ ] **14. ARM port** — one board (original Raspberry Pi), device tree, ARM boot path.

## Notes
- Drivers: most will move to user space, built against a Linux-kernel-API shim (see `docs/DESIGN.md`).
- WiFi alone can be more code than everything else combined — plan for it last.
- If speed matters more than learning: Linux kernel + BusyBox ("Linux From Scratch") reaches the same goals far faster.
