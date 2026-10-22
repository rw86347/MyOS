# Where MyOS still differs from Linux

How this list is made, so it can be made again after every fix:

- **Files and tools:** `tools/linux-audit.sh`, run on MyOS (and on a Linux
  machine to compare). It checks what real programs were seen to open and
  what a Linux user types. First run, 2026-10-08, x86_64 live disk in QEMU:
  **24 present, 86 missing or failing.** After the fixes of 2026-10-09
  (docs/LOG.md): **85 present, 25 missing** — what is left is mostly
  section 2 below (laptop /sys, rtc, kmsg) and `who` (utmp).
- **System calls:** `tools/syscall-coverage.py` against Linux's own tables:
  x86_64 has 244 of Linux's 369 (2 accepted and ignored, 123 missing; it was
  222 / 145). The missing ones still worth having are in section 3.
- **What programs actually hit:** the kernel's `syscall N not implemented
  (program)` lines from the day's runs.

Found the hard way on 2026-10-08, and already fixed: no `/proc/version`
(LibreOffice's launcher stopped: "/proc not mounted"), no `/sys/block` (the
installer saw no disks), the direction flag not cleared on interrupt entry
(the kernel copied backwards over itself), and partitions numbered by count
instead of by their slot in the table (`sda2` could have meant Linux's `sda3`).

## 1. Breaks everyday programs now — done 2026-10-09

| What | Linux | MyOS | Who needs it |
|---|---|---|---|
| `/proc/<pid>/` (stat, status, statm, cmdline, comm, environ, maps, fd, exe, cwd, limits) and `/proc/self` as a link to it | one folder per process | none (only `self/fd`, `self/exe`) | `ps`, `top`, `pgrep`, `pkill`, `htop`, `killall`; Chromium's and LibreOffice's memory and process checks; GLib. **ps/top/pgrep fail outright today.** (ROADMAP 8j) |
| `/proc/mounts`, `/proc/self/mountinfo`, `/etc/mtab` → it | the mount table | none | `mount` (no list), `df`, `findmnt`, `lsblk`, GLib's mount monitor (file choosers, My Files' "Other places") |
| `/proc/uptime`, `/proc/loadavg`, `/proc/stat` | clock and CPU counters | none | `uptime` fails, `top`, `htop`, `w`, Chromium's task manager |
| `/dev/stdin`, `/dev/stdout`, `/dev/stderr`, `/dev/fd/` | links into `/proc/self/fd` | none | shell scripts, and bash's `<(...)` (it uses `/dev/fd/N`) |
| Disks: `/dev/sda…`, `/sys/dev/block`, `/sys/class/block`, re-reading a partition table (`BLKRRPART`) | block device files | `/sys/block` only (2026-10-08) | the installer (WORKLIST C2), `lsblk` (fails), `mkfs`, `sfdisk`, `fdisk` |
| `/proc/sys/kernel/{hostname,osrelease,ostype,pid_max}`, `random/{uuid,boot_id,entropy_avail}` | small text files | none | busybox, GLib, D-Bus, `uuidgen`, Chromium |
| `/proc/cmdline` | the boot command line | present but empty | scripts that look for boot options |

## 2. Laptop and desktop features

| What | Who needs it |
|---|---|
| `/sys/class/power_supply` (battery, AC) | a battery icon on the MSI — **done 2026-10-21** (with `/sys/firmware/acpi/platform_profile`; docs/BATTERY-PLAN.md) |
| `/sys/class/backlight` | screen brightness keys |
| `/sys/class/net/<if>/` (operstate, address, carrier) | network tools, Chromium's "are we online" |
| `/sys/class/input`, `/sys/class/sound`, `/sys/class/drm`, `/dev/dri/card0` | device listings; GPU drivers later |
| `/sys/devices/system/cpu/cpu0/…` (cpufreq) | CPU speed readers, `lscpu` |
| `/dev/rtc0`, `/dev/rtc` | `hwclock` (the "this PC also runs Windows" clock option) |
| `/proc/net/{dev,tcp,udp,unix}` | `ss`, `netstat`, `ifconfig`, traffic meters |
| `/dev/kmsg`, `/dev/log` | `dmesg -w`, programs that log to syslog |
| `/dev/full`, `/dev/hwrng`, `/dev/loop-control` | tests, `rngd`, loop mounts (disk images) |
| `who`, `w` | `/var/run/utmp` written at login |

## 3. System calls programs were seen to call (x86_64)

| Call (number) | Caller | Note |
|---|---|---|
| `fsopen` (430) | util-linux `mount` | it falls back to `mount(2)`; worth it with B27 |
| `close_range` (436) | `login` and others | easy: close a range of fds |
| `vhangup` (153) | `login` | can succeed and do nothing |
| `pidfd_open` (434) | GLib | GLib falls back; pidfds are cheap to add |
| `getcpu` (309) | Java, glibc programs | always CPU 0 |
| `sched_getparam` and the other `sched_*` (142–148) | Chromium's thread pool | priorities: plain answers |
| `mlock`, `munlock`, `mlockall` (149–152) | gnome-keyring | succeed (no swap, so memory never leaves RAM) |
| `landlock_*` (444–446), `seccomp` (317) | Chromium's sandbox | ENOSYS is the right answer for now; the sandbox is a later project |
| `ptrace` (101) | Chromium's crash reporter, debuggers | later (gdb would need it) |

Missing and likely to come up next: `waitid`, `msync`, `timerfd_*` (many
event loops), `signalfd`, `preadv`/`pwritev`, `sendmmsg`/`recvmmsg`,
`splice`/`tee`, `clone3` and `rseq` (glibc-built programs), `openat2`,
`epoll_pwait2`, `membarrier`. The rest of the 145 (modules, kexec, BPF,
io_uring, quotas, keyrings, mempolicy) have no user yet.

## Suggested order

1. `/proc/<pid>` and `/proc/self` — one piece of work that makes `ps`, `top`,
   `pgrep`, `pkill` work and removes the most "Error: mount /proc" failures.
2. `/proc/mounts` + `mountinfo`, `/proc/uptime`, `/proc/loadavg`, `/proc/stat`,
   `/dev/fd` and `/dev/std*`, `/proc/sys/kernel/*` — small, many users.
3. Block devices and `BLKRRPART` (WORKLIST C2) — the installer.
4. The cheap system calls: `close_range`, `pidfd_open`, `getcpu`, `sched_*`,
   `mlock*`, `vhangup`, `waitid`, `msync`, `timerfd`, `signalfd`.
5. Laptop `/sys`: battery, backlight, net, rtc.

Rerun `tools/linux-audit.sh` after each, and update the counts above.
