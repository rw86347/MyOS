#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# stick-logs.sh — copy a MyOS live stick's logs onto the Mac, read-only.
#
#   sudo tools/stick-logs.sh            (macOS asks for the password: the
#                                        stick is read as a raw disk)
#
# macOS can't mount ext2, but e2fsprogs' debugfs reads it straight from
# the partition without mounting or writing it. Copies the stick's
# /var/log (the kernel log of each boot in boot/N.txt, written by the
# bootlog service; wpa_supplicant.log; the system log in socklog/) to
# build/stick-logs/<date-time>/, owned by the user who ran sudo.
set -e
DEBUGFS=${DEBUGFS:-/opt/homebrew/opt/e2fsprogs/sbin/debugfs}
[ -x "$DEBUGFS" ] || { echo "debugfs not found: brew install e2fsprogs" >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run it with sudo: sudo $0" >&2; exit 1; }
cd "$(dirname "$0")/.."

# The stick: an external, physical disk whose second partition is Linux
# (the live disk's myos-root). Exactly one, or we stop.
part=
for d in $(diskutil list physical external | sed -n 's|^/dev/\(disk[0-9]*\) .*|\1|p'); do
    if diskutil list "$d" | grep -q "Linux.* ${d}s2\$"; then
        [ -z "$part" ] || { echo "more than one MyOS stick is plugged in" >&2; exit 1; }
        part=${d}s2
    fi
done
[ -n "$part" ] || { echo "no MyOS stick found (diskutil list external)" >&2; exit 1; }

out=build/stick-logs/$(date +%Y%m%d-%H%M%S)
mkdir -p "$out"
echo "Reading /var/log from /dev/$part"
"$DEBUGFS" -R "rdump /var/log $out" "/dev/r$part" 2>/dev/null
# Each desktop user's ~/.xsession-errors: what the desktop's programs (the
# tray icons, X clients) printed. Saved as xsession-errors.USER.
for u in $("$DEBUGFS" -R "ls /home" "/dev/r$part" 2>/dev/null | tr -s ' ' '\n' \
           | grep -v -e '^$' -e '^[0-9(]' -e '^\.\.*$' -e '^lost+found$'); do
    "$DEBUGFS" -R "dump /home/$u/.xsession-errors $out/xsession-errors.$u" \
        "/dev/r$part" 2>/dev/null || true
    [ -s "$out/xsession-errors.$u" ] || rm -f "$out/xsession-errors.$u"
done
[ -n "$SUDO_USER" ] && chown -R "$SUDO_USER" build/stick-logs
echo "Copied to $out:"
ls -l "$out" "$out/log" "$out/log/boot" 2>/dev/null
