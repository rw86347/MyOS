#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# linux-audit.sh — run ON MyOS (or on Linux, to compare): which of the
# files and tools Linux programs rely on are there, and which are not.
#
#   sh linux-audit.sh            one line per item: ok / MISSING / FAILS
#
# The list is what real programs were seen to open (musl, GLib/GTK,
# Chromium, LibreOffice, Xorg, procps, util-linux, busybox, the installer)
# plus the usual places a Linux user looks. Run it on a Linux machine too:
# whatever is "ok" there and not here is a difference to close.
# docs/LINUX-COMPAT.md has what came out, and what each gap breaks.

ok=0; bad=0
have() {                # have PATH [what it is for]
    if [ -e "$1" ]; then ok=$((ok + 1)); else
        bad=$((bad + 1)); echo "MISSING  $1${2:+   ($2)}"; fi
}
reads() {               # reads PATH [what]: present and not empty
    if [ -r "$1" ] && [ -n "$(head -c 64 "$1" 2>/dev/null)" ]; then ok=$((ok + 1)); else
        bad=$((bad + 1)); echo "MISSING  $1${2:+   ($2)}"; fi
}
works() {               # works "description" command...: exits 0 with output
    d=$1; shift
    if out=$("$@" 2>&1) && [ -n "$out" ]; then ok=$((ok + 1)); else
        bad=$((bad + 1)); echo "FAILS    $d: $* -> $(printf '%s' "$out" | head -1 | cut -c1-70)"; fi
}

echo "== /proc"
for f in version cpuinfo meminfo stat uptime loadavg mounts filesystems partitions \
         diskstats swaps devices interrupts cmdline modules misc crypto; do reads /proc/$f; done
for f in self/status self/stat self/statm self/cmdline self/environ self/maps self/smaps \
         self/limits self/mounts self/mountinfo self/cgroup self/oom_score_adj self/comm \
         self/io self/auxv self/task; do have /proc/$f; done
have /proc/$$ "the per-process folders: ps, pgrep, pkill, top"
have /proc/$$/stat
have /proc/1/cmdline "init"
for f in sys/kernel/hostname sys/kernel/osrelease sys/kernel/ostype sys/kernel/pid_max \
         sys/kernel/random/uuid sys/kernel/random/boot_id sys/kernel/random/entropy_avail \
         sys/vm/overcommit_memory sys/fs/file-max sys/net/core/somaxconn; do reads /proc/$f; done
for f in net/dev net/route net/tcp net/udp net/unix net/if_inet6; do have /proc/$f; done

echo "== /sys"
for f in block class/block class/net class/input class/sound class/drm class/graphics \
         class/power_supply class/backlight class/rtc class/tty dev/block dev/char \
         devices/system/cpu/online devices/system/cpu/cpu0 firmware/efi \
         kernel/mm/transparent_hugepage fs/cgroup bus/pci/devices bus/usb/devices; do have /sys/$f; done

echo "== /dev"
for f in null zero full random urandom tty console ptmx pts shm fd stdin stdout stderr \
         kmsg log rtc rtc0 fb0 dri/card0 input/event0 snd/controlC0 loop-control hwrng; do have /dev/$f; done
for d in /sys/block/*; do [ -e "$d" ] && have "/dev/${d##*/}" "a disk's own file (partitioning, mkfs)"; done

echo "== tools"
works "list processes"      ps
works "find a process"      pgrep -l .
works "memory"              free
works "uptime"              uptime
works "disk space"          df -h /
works "mounts"              mount
works "disks"               lsblk
works "network addresses"   ip addr
works "sockets"             ss -a
works "a terminal's size"   stty size
works "who is logged in"    who
works "top, one screen"     top -b -n 1

echo "== $ok present, $bad missing or failing"
