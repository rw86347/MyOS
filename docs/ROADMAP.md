# MyOS Roadmap

Target first: 32-bit x86 (i686) in QEMU, then real x86 PCs and x86_64 (MSI, GPD Pocket 4).
**Apple Silicon Macs (bare metal, no macOS)** are a planned third CPU family after the
ARM64 port has a QEMU `virt` bring-up; see step **15**. Running MyOS on an M1 Mac in
**QEMU/UTM** (emulated x86) is not a port — it is the normal dev workflow today.
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
    Started (2026-10-03): `mount -t proc` gives `/proc/self/fd/N` (what `ttyname`
    and `login` need); `getcwd` and those links share `vfs_path`. 2026-10-04:
    `/proc/self/exe` (what Firefox reads to find its files).
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
    - [x] The installer installs, for UEFI (2026-10-10) and BIOS (2026-10-17), and
      `make test-install` checks it in QEMU end to end: install from the stick, start
      the new disk alone, log in (docs/INSTALLER-PLAN.md).
    - [x] The live disk's software: `make live-root` gathers `live/packages.txt`
      (IceWM desktop, logins, AbiWord, gcc/make/python3/adb, apk) as pinned Alpine
      packages plus the MyOS source into `build/live/rootfs.tar`.
    - [x] A normal userland instead of BusyBox (2026-10-03): GNU coreutils,
      findutils, grep, sed, gawk, util-linux, procps; dash as /bin/sh, bash for
      people; runit as process 1 (Void Linux layout, `/etc/runit/1-3`, `/etc/sv`),
      agetty + util-linux login; man pages. `!busybox` in `packages.txt` makes the
      build fail if anything pulls BusyBox in. BusyBox stays in the test initramfs.
      The kernel now looks for `/sbin/init` after `/init`, like Linux.
    - [x] Kernel calls for it (2026-10-03): `sethostname`, `vhangup`, `reboot`
      (restart through the keyboard controller, power off on QEMU/UTM, halt
      elsewhere; Ctrl-Alt-Del to init), `mount -t tmpfs`, `umount2` (errors only).
      Done: `make test` (boottest), `poweroff -f` / `reboot -f` stop QEMU.
    - [x] What runit, runsv and login needed next (2026-10-03): `flock`, named
      pipes (FIFOs), `getpriority`/`setpriority`, memory areas without a fixed
      limit (32 → up to 1024), `/proc/self/fd`, and the start-up strings in Linux's
      order. `make live-hd` builds `build/myos-live.img`; `make utm-live` the UTM VM.
      Done: it boots to "myos login:", user `myos` logs in, `doas id` is root,
      `doas poweroff` and Ctrl-Alt-Del shut down cleanly.
    - [ ] Still missing for it: the rest of `/proc`, `/sys` (8j), device nodes
      `/dev/tty1`… and `/dev/ttyS0`, block device files (so `mount /dev/sda1`
      works), undoing a mount, `utmp` writes, UNIX sockets (syslog), hard links
      (`useradd`, `passwd`), fds above 31 (bash's job control).
    - [x] Users (part, 2026-10-03): real/effective/saved/fs uid and gid per process,
      supplementary groups, the `set*id` family, setuid/setgid on exec with
      `AT_SECURE`, files owned by their creator, setgid directories.
    - [ ] Users: permission checks in the VFS and on `kill`. Until then any user
      can read and write every file, so the logins are not yet a security boundary.
    - [ ] The installer picks the compositor level (`none`, `xrender`, `gl`) from a
      measured repaint time, RAM and the GPU, writes `/etc/myos/desktop.conf`, and
      leaves `picom` out where it doesn't fit. Requirements: `docs/DESIGN.md`.
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
  - [ ] 9d-3: several commands in flight and driver limits.
    - [x] requests that don't wait (`blockdev_submit`, a worker per disk) and
      read-ahead that runs while the program copies (68 of 71 pages in the test).
    - [ ] drivers that take several commands at once (NVMe queue depth, virtio
      chains, AHCI NCQ) instead of the worker thread.
    - [ ] driver limits: several disks per driver, LBA48, AHCI BIOS hand-off.
  - [ ] 9e: FAT32 in the VFS: read, then write (USB sticks, swapping files with a Mac/PC).
    Read is in: the first FAT32 partition is mounted at /mnt (read-only). Write,
    which has to allocate clusters and update the FAT, is still open.
  - [x] USB sticks and disks on xHCI (2026-10-07): our own xHCI and mass-storage
    (bulk-only, SCSI) drivers; the live system runs from a USB stick in QEMU
    (`make test-usb`). `docs/USB-PLAN.md`. Old PCs still need UHCI/EHCI.
  - [x] USB keyboards and mice (HID, 2026-10-17): interrupt endpoints and a
    report-descriptor parser; they reach the console and X as PS/2 ones do.
    A PC with no PS/2 controller boots now (it used to hang).
  - Later: ext4 read, exFAT, ext4 write (needs a journal). NTFS is out of scope.

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
  - [x] 10f: DHCP, in the kernel (`net/dhcp.c`), not `udhcpc`. The address, netmask,
    gateway and nameserver are runtime values learned at boot: DISCOVER, OFFER,
    REQUEST, ACK, with a CSPRNG transaction id, exponential backoff, and a fall back
    to the old static 10.0.2.15 if nobody answers. `make test` greps
    `dhcp: lease from 10.0.2.2` *and* the `ipv4: eth0 is ...` line — the address
    alone proves nothing, because under QEMU the fallback hands out exactly the same
    four numbers, so only the "lease from" line distinguishes a working client from
    one that gave up. The reply parser is a fuzz target (`make fuzz TARGET=dhcp`).
    This is what UTM's `Shared` (Apple vmnet) mode needed: it hands out addresses by
    DHCP and has no fixed ones to hard-code.
    Still missing: `AF_PACKET` and interface ioctls, so `udhcpc` itself cannot run,
    and no lease renewal (see `docs/gaps.md`).
- [ ] **11. Fuller Linux ABI** — `mmap`, `brk`, dynamic linking (`ld.so`), more syscalls as programs need them.
  Done when: ordinary dynamically linked GNU programs run.
  - [x] Pulled forward (2026-10-02): dynamic linking — the kernel loads a program's
    interpreter (PT_INTERP: musl's ld.so), file-backed mmap (demand-paged from the
    initramfs). Alpine's unmodified, dynamically linked bash runs as the login shell.
  - [x] Pulled forward (2026-10-03): threads, for GLib (GTK, AbiWord) and the X
    server — `clone(CLONE_THREAD)` thread groups, `futex` (wait/wake/requeue, both
    timeout forms), `gettid`, `set_tid_address` + clear-on-exit, `exit` vs
    `exit_group`, per-thread signal masks, `tgkill`, execve/fork from a threaded
    process. `user/musl/pthreadtest` (linked against Alpine's real musl) passes.
- [ ] **11b. Graphics** (after the network: X needs sockets too) — the boot loader picks a
  VESA graphics mode (UEFI GOP on the Pocket 4): a framebuffer, marked write-combining.
  A pixel console with big, scalable text; PS/2 mouse; `/dev/fb0`, `/dev/input/event*`,
  AF_UNIX sockets. Then Alpine's X server (fbdev), a small window manager, `xterm`.
  Done when: `xterm` windows can be moved with the mouse.
  Display drivers use Linux's DRM/KMS shape (decided 2026-10-03, `docs/DESIGN.md`):
  `include/drm/` keeps Linux's header paths and names, and programs get `/dev/fb0`
  through fbdev emulation, as on a modern Linux.
  - [x] PS/2 mouse driver (IRQ 12, IntelliMouse wheel); `make test` moves and clicks it
    through QEMU's monitor.
  - [x] Console split: `drivers/vt.c` (VT100 parser + cell grid) and backends:
    80x25 text (`vga.c`) or pixels (`drivers/video/fbcon.c`). `TIOCGWINSZ` follows the
    screen; `SIGWINCH` on a change.
  - [x] DRM core subset (`drivers/gpu/drm/drm_core.c`): modes with VESA timings,
    connectors, the simple display pipe. Our own driver for the Bochs/QEMU std VGA
    (`drivers/gpu/drm/tiny/bochs.c`, PCI 1234:1111, UTM's "VGA" display): any
    standard mode up to 2560x1600, 1920x1200 at boot.
  - [x] Framebuffer write-combining (PAT, `arch/i386/pat.c`) with 4 MiB pages.
  - [x] Pixel console: the VGA BIOS's 8x16 font (copied by `stage2.asm`), 2x on big
    screens (120x37 at 1920x1200), scrolling by panning.
  - [x] `/dev/fb0` (29:0): Linux's fbdev ioctls and structs, `mmap` (device memory,
    shared across `fork`), mode changes at run time, `KDSETMODE`. `make test` runs
    `user/fbtest.c` and checks a QEMU screendump (`tools/check-screen.py`).
  - [ ] VESA boot framebuffer for real hardware (Linux's `simpledrm` shape): `stage2.asm`
    picks the mode, the same DRM core drives it.
  - [x] What the X server needs underneath it: `/dev/input/event0`/`event1`
    (`drivers/evdev.c`), AF_UNIX sockets (`net/unix.c`), `epoll` (`fs/epoll.c`),
    pseudo-terminals for `xterm` (`drivers/pty.c`, `/dev/ptmx` + `/dev/pts/N`, with
    the line discipline moved out of the console into `struct tty`), and the one
    thing the fbdev driver looks for in `/sys` (`fs/sysfs.c`). Alpine's X server
    runs unmodified on the framebuffer; `user/musl/boottest.c` covers the ptys.
  - [x] System V shared memory for MIT-SHM (`kernel/shm.c`): `shmget`, `shmat`,
    `shmdt`, `shmctl` through `ipc()`. `make test` runs `user/musl/shmtest.c`.
  - [ ] MIT-SHM switched back on in the live disk's X config, checked with Imlib2
    (IceWM's theme, the login screen).
  - [x] A graphical login (`live/myos-login/myos-login.c`): starts the X server,
    draws the Golden Hour login screen, checks the password with PAM and runs the
    session; runit keeps it alive. The desktop wears the same theme
    (`live/theme/make-live-theme.py` writes IceWM's theme and the login screen's
    pictures from the palette in `assets/themes/golden-hour`).
  - [x] The mockup's Start menu and Start button (`live/myos-menu/myos-menu.c`),
    on Ctrl+Esc, the Windows key or a click; lists only programs that exist.
  - [x] Firefox on the live disk (`[browser]` in `live/packages.txt`): Alpine's
    32-bit build, Start menu "Web Browser", HTML from My Files. Sandbox off
    (`MOZ_DISABLE_CONTENT_SANDBOX`) and `/dev/shm` mounted. `firefox --version`
    prints 132.0.2 on MyOS; a GUI start still dies in `libxul.so` (write to
    NULL). See `docs/gaps.md`.
  - [ ] The window overview (hot corner, F3) at every compositor level, and `picom`
    started by the session where `/etc/myos/desktop.conf` allows it (`docs/DESIGN.md`).
    `picom` 10.2 is in the live disk's `[compositor]` group.
- [ ] **12. Real hardware** — boot from USB/CD on an old Pentium 4 / Pentium M machine.
- [ ] **12b. GPD Pocket 4** (`docs/hardwareRequirements.md`) — `arch/x86_64`, a UEFI loader,
  ACPI, PCIe with MSI/MSI-X; then NVMe (its boot SSD, native driver) and xHCI; then the
  Linux drivers through the shim (`r8169` for its Ethernet, `amdgpu`, ...).
  Branch `dualChip` started the tree shape: `ARCH=`, `phys_addr_t`,
  `live/packages.x86_64.txt`, `arch/x86_64/PORT`.
  First visible check (done): hello on QEMU + MSI (GOP at `0x400000000`).
  x86_64 live userland built (`make ARCH=x86_64 live-uefi`).
  - [x] The portable kernel runs as x86_64 on QEMU + OVMF (2026-10-05): higher half,
    4-level paging with copy-on-write fork, `syscall`/`sysret`, ELF64, signals, a
    BusyBox shell (`make ARCH=x86_64 test-uefi-shell`). `docs/x86_64-PORT-PLAN.md`.
  - [x] The system calls that were missing, on both CPUs (2026-10-05): sendfile,
    times/getrusage, pause, clock setting + adjtimex, dmesg (syslog), chroot,
    pivot_root, inotify, fcntl record locks, System V semaphores and message
    queues, and x86_64's own layouts (statfs, select, shm*, ...). `user/sys/systest.c`.
  - [ ] Next: the live desktop (`myos-login`, X) on the x86_64 kernel, then the
    GPD Pocket 4 itself (ACPI, MSI, NVMe, xHCI).
- [ ] **13. WiFi** — the MSI Katana's Intel AX201: our own 802.11 stack with Linux's
  interfaces (nl80211 up, mac80211's driver API down), Intel's `iwlwifi` through the
  shim, `wpa_supplicant` in user space, a Windows-style tray icon. Steps W0-W8 in
  `docs/WIFI-PLAN.md` (decided 2026-10-06; replaces the ath5k idea).
  Done when: the MSI connects to a WPA2 network from the tray icon.
  Progress (2026-10-06): W0-W3, W5 and W6 done — in QEMU, Alpine's
  wpa_supplicant joins a simulated WPA2 network through our stack, and
  iwlwifi links into the x86_64 kernel; the tray icon is written (its
  screenshot test is left); next is W7, the real card on the MSI.
- [ ] **14. ARM64 port** — CPU-specific code only in `arch/` (same rule as i386 and x86_64).
  Linux **aarch64** syscall ABI and ELF64 user programs. Boot with a **device tree** in
  QEMU's `virt` machine first; no Apple hardware in this step.
  Done when: `make ARCH=arm64 run` (or equivalent) prints hello on the serial console and a
  static musl aarch64 binary runs one `write` syscall.
  - [ ] 14a: exceptions, EL1, GIC, timers, MMU (4 KiB pages or 4-level as needed), context
    switch, `kmalloc`, and a minimal UART driver for early print.
    Started (2026-10-11): QEMU virt hello, FDT/PMM, 4K L0→L2 MMU, GIC/timer, kheap, EL0
    data, musl static `write` (`make ARCH=arm64 test`).
  - [ ] 14b: Linux syscall table for aarch64, `fork`/`execve`, paging and signals on ARM64.
    Started (2026-10-11): newc walker + embedded cpio; `fork`/`execve`/`wait`/
    pipes; Alpine aarch64 bash (dynamic musl) as the login shell.
    `make ARCH=arm64 test` greps `bash from MyOS` and `bash-5.2`.
    `make ARCH=arm64 utm-arm64` writes a native UTM virt VM.
  - [ ] 14c: **Raspberry Pi (original board)** as the first **bare-metal ARM** target — simpler
    firmware and published docs than Apple. Device tree for that board, SD boot, USB keyboard
    if needed. Done when: the Pi boots MyOS from an SD card to a shell on serial or HDMI.
- [ ] **15. Apple Silicon bare metal** — MyOS as the **only** OS on the machine (MacBook Air
  M1 and later Apple laptops/desktops with Apple Silicon). This is **not** a macOS app or a
  VM: the Mac must boot MyOS from its own partition or external disk without running Darwin.
  Depends on **14** (shared `arch/aarch64/` kernel) and most of the x86_64 bring-up (UEFI
  habits, live userland on `aarch64` Alpine, Linux drivers through the shim where we use them).
  Apple blocks arbitrary kernels the way a PC BIOS does not — plan on a **small boot chain**
  (study [Asahi Linux](https://asahilinux.org/) / m1n1: hand-off into our kernel with a DTB
  and reserved memory map). CPU-specific Apple blocks stay in `arch/aarch64/` and
  `arch/aarch64/apple/` (or similar), not in portable code.
  Done when: a **MacBook Air (M1)** boots MyOS from disk to the same kind of login session
  the x86_64 live stick reaches today (framebuffer or serial, keyboard, root disk, power off).
  - [ ] 15a: document the target machines and boot path (`docs/hardware-apple-silicon.md`:
    models, secure boot / recovery constraints, partition layout next to macOS if dual-boot
    is ever wanted — **single-boot MyOS-only is the first goal**).
    Started (2026-10-11): hardware doc + DTB platform probe (`make ARCH=arm64 test-arm64-apple`).
  - [ ] 15b: boot — load the kernel + DTB on at least one M1-class machine; serial or
    framebuffer text; memory map and CPU features parsed from the DTB.
    Started (2026-10-11): DTB `apple,uart` harness + `apple,s5l-uart` (M1 Air fixture);
    `test-arm64-uart-early`, m1n1 payload/x0 sim, `docs/m1n1-chainload.md` (hardware boot open).
  - [ ] 15c: storage — Apple NVMe (or the path Asahi uses) so `myos-root` on the internal
    SSD is visible; USB boot from a stick remains the installer path until internal disk works.
  - [ ] 15d: input and display — built-in keyboard/trackpad and the laptop panel (framebuffer
    or a simple KMS path); enough for `myos-login` or agetty.
  - [ ] 15e: **`live/packages.aarch64.txt`** (or merge into arch-specific world files): Alpine
    aarch64 userland, same desktop story as x86_64 where packages exist; rebuild
    `make ARCH=arm64 live-uefi` (or an Apple-specific boot image) on the Mac host.
  - [ ] 15f: networking and Wi‑Fi on Apple hardware (last within this step — same priority as
    step 13 on x86: hard, mostly Linux driver + firmware through the shim).
  Expect this step to take **much longer** than a PC or Pi port; treat each sub-bullet as its
  own visible milestone in `docs/LOG.md`.

## Notes
- Drivers: most will move to user space, built against a Linux-kernel-API shim (see `docs/DESIGN.md`).
- WiFi alone can be more code than everything else combined — plan for it last.
- **Apple Silicon bare metal** reuses the ARM64 kernel but adds Apple-only boot and hardware;
  it is scheduled **after** step 14's QEMU and Pi checks so `arch/` stays one tree per ISA.
- If speed matters more than learning: Linux kernel + BusyBox ("Linux From Scratch") reaches the same goals far faster.
