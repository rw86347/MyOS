# Work list: Chromium first, then the gaps

Numbered so progress reads "N/M done". Part A is what stops Chromium
(x86_64) from starting, in the order it showed us; it grows as each fix
lets Chromium get further. Part B is `docs/gaps.md`'s "At a glance" list,
most-felt first: what breaks ordinary Linux programs, then security, then
networking depth, then hardware.

## A. Chromium runs (found by running it, 2026-10-07)

- [x] A1. Program loader maps on demand (a 220 MB binary: "Out of memory")
- [x] A2. `mmap` lengths over 4 GiB (PartitionAlloc's 32 GiB reservation),
      and the 64-bit `mmap` entry's own 4 GiB guard
- [x] A3. Serial console input (testing in QEMU; a login on ttyS0)
- [x] A4. `SOCK_SEQPACKET` Unix sockets (`socketpair: Not supported`)
- [x] A5. Passing open files (`SCM_RIGHTS`)
- [x] A6. `/proc/self/task/` with a link count of 2 + threads (zygote: FATAL)
- [x] A7. More threads and processes than 64 in the whole system
      (`pthread_create: Resource temporarily unavailable`)
- [x] A8. More than 64 open files per process; `select` past fd 31
- [x] A9. `/proc/self/fd` lists only open fds (crashpad closes the rest)
- [x] A10. `SO_PASSCRED` and `SCM_CREDENTIALS` (crashpad's handler)
- [x] A11. `sigaltstack`
- [x] A12. `/proc/cpuinfo`, `/proc/meminfo`, `/proc/sys/fs/inotify/*`
- [x] A13. `rt_sigqueueinfo` / `rt_tgsigqueueinfo`
- [x] A14. `int3` gives SIGTRAP, not a general-protection fault
- [ ] A15. xHCI: no false "interrupt does not arrive" under load
- [ ] A16. Chromium shows a window on the desktop (DISPLAY=:0)

## B. The gaps, most-felt first

Programs and memory
- [ ] B1. Use all RAM on x86_64 (2 GiB of the MSI's 16 GiB today)
- [ ] B2. Loopback: 127.0.0.1 for TCP and UDP
- [ ] B3. `setsockopt` that does something: ~~`SO_RCVTIMEO`/`SO_SNDTIMEO`~~
      (done 2026-10-14), `SO_KEEPALIVE`, `TCP_NODELAY`, `SO_REUSEADDR`, `SO_LINGER`
- [ ] B4. More than 16 TCP connections in the whole system
- [ ] B5. `PROT_NONE` enforced, and the no-execute bit on x86_64
- [ ] B6. `madvise(MADV_DONTNEED)` drops pages (they read back as zeros)
- [ ] B7. `SA_RESTART`: interrupted calls restart
- [ ] B8. Job control: SIGSTOP/SIGTSTP/SIGCONT, Ctrl-Z, `fg`/`bg`
- [ ] B9. Queued real-time signals; `si_pid`/`si_uid` for `kill`
- [ ] B10. `/proc/<pid>/` (stat, status, cmdline, maps): `ps`, `top`, `pidof`
- [ ] B11. `/proc/mounts`, `/proc/stat`, `/proc/uptime`, `/proc/loadavg`
- [ ] B12. Hard links (`link`, `linkat`)
- [ ] B13. `MAP_SHARED` file mappings write to the file; `msync`
- [ ] B14. `MAP_SHARED|MAP_ANONYMOUS` stays shared after `fork`
- [ ] B15. `preadv`/`pwritev` (and `2`)
- [ ] B16. `timerfd_create`/`settime`/`gettime`
- [ ] B17. `signalfd`/`signalfd4`
- [ ] B18. `waitid`
- [ ] B19. `close_range`
- [ ] B20. `sendmmsg`/`recvmmsg`
- [ ] B21. `openat2`, `execveat`
- [ ] B22. `clone3`
- [ ] B23. `sched_*`: policies, priorities, affinity
- [ ] B24. `mlock` family (accepted; there is no swap)
- [ ] B25. `splice`/`tee`/`vmsplice`, `sync_file_range`
- [ ] B26. POSIX message queues (`mq_*`)
- [ ] B27. The new mount API (`fsopen` & co.), unmounting
- [ ] B28. `settimeofday`; `vhangup` in the x86_64 table
- [ ] B29. `get_mempolicy`/`mbind`/`set_mempolicy` (one node)

Users and security
- [ ] B30. File permission checks in the VFS
- [ ] B31. Permission checks on `kill`, `mount`, `reboot`, the host name
- [ ] B32. Capabilities (real sets; `capset` drops) — iputils `ping`
- [ ] B33. CAP_NET_RAW for raw sockets
- [ ] B34. Empty passwords: the login screen refuses a typed one
- [ ] B35. Address-space randomisation

Networking depth (TCP/IP)
- [ ] B36. TCP congestion control (slow start, cwnd)
- [ ] B37. TCP out-of-order queue and SACK
- [ ] B38. TCP window scaling, timestamps, an RTT estimate
- [ ] B39. TCP: Nagle (and `TCP_NODELAY` to turn it off), TIME_WAIT 60 s
- [ ] B40. User space sets addresses and routes (`SIOCSIFADDR`, `SIOCADDRT`,
      netlink RTM_NEW*), and hears link events
- [ ] B41. DHCP renews its lease
- [ ] B42. IPv4 fragments reassembled
- [ ] B43. Multicast and broadcast received; IGMP
- [ ] B44. IPv6

Files and storage
- [ ] B45. ext4 read
- [ ] B46. FAT32 write
- [ ] B47. devpts (`/dev/pts` as a real file system)
- [ ] B48. Files over 4 GiB, paths over 255 bytes
- [ ] B49. `epoll` with a ready list and real `EPOLLET`

Hardware
- [x] B50. USB keyboards and mice (HID) (2026-10-17; limits: docs/gaps.md, USB)
- [ ] B51. USB hubs
- [ ] B52. The MSI's touchpad (I2C HID), two-finger scrolling (plan: docs/TOUCHPAD-PLAN.md)
- [ ] B53. ACPI: power off and reboot on real PCs
- [ ] B54. Local APIC timer and IOAPIC
- B55. Sound: now part D below (`docs/SOUND-PLAN.md`)
- [ ] B56. More than one CPU (SMP)

## C. The installer (`docs/INSTALLER-PLAN.md`)

What it needs beyond part B (FAT32 write B46, mounts B27, hard links B12 and
real passwords B34 are there already), then the installer itself.
- [ ] C1. The MSI's own SSD visible (Intel VMD/RST, or AHCI mode)
- [ ] C2. Block device nodes; re-reading a partition table (`BLKRRPART`, sizes)
- [ ] C3. UEFI runtime services: boot entries (`efivarfs`, `efibootmgr`)
- [ ] C4. Console key maps; the hardware clock in local time (an option)
- [ ] C5. `dosfstools`, `efibootmgr`, CJK fonts and pinyin on the live disk
      (CJK fonts are in: Noto Sans/Serif CJK, `[fonts]` in live/packages.txt;
      pinyin is in on the 64-bit disk: IBus + libpinyin, `[input]`, Ctrl+Space.
      The 32-bit lock still needs `fetch-live.py --add`, and the installer's
      Chinese checkbox doesn't install `[input]` yet)
- [ ] C6. `make test-install` in QEMU (helper and plan file exist)
- [x] C7. `myos-install --text`
- [x] C8. The GTK installer (the mockups' nine pages)

## D. Sound (`docs/SOUND-PLAN.md`)

- [ ] D1. S0: the MSI's sound facts (codec model, pins, DSP or not, digital mics)
- [x] D2. S1: the HD Audio controller: CORB/RIRB, MSI, codecs found
- [x] D3. S2: the sound core and the ALSA ABI (`/dev/snd`, PCM ioctls, mmap);
      `make test-sound` plays a tone and checks the recording (both CPUs)
- [ ] D4. S3: the generic codec driver (paths, mixer controls, jacks, mic) — written;
      paths and mixer work in QEMU, jacks and recording wait for the MSI
- [ ] D5. S4: speakers, headphones and headset mic on the MSI
- [ ] D6. S5: alsa-lib and alsa-utils; a sound server; the volume icon and keys
- [ ] D7. S6: HDMI audio (and SOF, only if the MSI has digital mics)
