#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""boottime.py — where a MyOS boot spends its time, seen from outside.

    python3 tools/boottime.py build/x86_64/myos-live-uefi.img
    python3 tools/boottime.py IMG --everyday      boot twice: time the second boot
    python3 tools/boottime.py IMG --no-login      stop at the login screen

Boots the image in QEMU the way UTM does (q35, AHCI, std VGA, e1000), stamps
every serial line with the seconds since QEMU started, and prints the
milestones: firmware -> our loader -> the kernel's steps -> runit -> the
login screen. Then it logs in on the screen (Return on the selected user,
the live disk's empty password) and times the desktop's programs appearing.

A fresh image runs its first-boot scripts; --everyday boots a copy once
first, so the timed boot is the one people see every day.

QEMU numbers compare versions of MyOS with each other; the real ones come
from the real machines (docs/performanceSuggestions.md). Write before/after numbers to
docs/performance.md.
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

SERIAL, MONITOR = 45474, 45475
OVMF_CODE = "/opt/homebrew/share/qemu/edk2-x86_64-code.fd"
OVMF_VARS = "/opt/homebrew/share/qemu/edk2-i386-vars.fd"
MILESTONES = (r"MyOS, written|Step \d+|initramfs:|mounted at /|dhcp:|usb:|storage:|runit: enter|"
              r"First boot|MyOS is up|login screen|starting the X server")
DESKTOP = ["icewm", "myos-menu", "myos-desktop", "myos-wifi", "myos-trash"]


class Machine:
    def __init__(self, img, scratch, keep=False, net=True):
        vars_fd = os.path.join(scratch, "vars.fd")
        shutil.copy(OVMF_VARS, vars_fd)
        self.t0 = time.time()
        self.q = subprocess.Popen(
            ["qemu-system-x86_64", "-machine", "q35,vmport=off,hpet=off", "-m", "4G",
             "-cpu", os.environ.get("BOOTTIME_CPU", "qemu64"),      # max: SMEP, SMAP
             "-vga", "none", "-device", "VGA",
             "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
             "-drive", f"if=pflash,format=raw,file={vars_fd}",
             "-drive", f"if=none,id=d0,format=raw,file={img}" + ("" if keep else ",snapshot=on"),
             "-device", "ide-hd,drive=d0,bus=ide.0,bootindex=0",
             *(["-nic", "user,model=e1000"] if net else ["-net", "none"]),
             "-no-reboot", "-display", "none",
             "-serial", f"tcp:127.0.0.1:{SERIAL},server=on,wait=on",
             "-monitor", f"tcp:127.0.0.1:{MONITOR},server=on,wait=off"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):                    # QEMU opens the port when it's ready
            time.sleep(0.1)
            try:
                self.s = socket.create_connection(("127.0.0.1", SERIAL))
                break
            except ConnectionRefusedError:
                if self.q.poll() is not None:
                    raise
        else:
            self.q.kill()
            raise ConnectionRefusedError("QEMU's serial port never opened")
        self.s.settimeout(0.05)
        self.buf, self.part, self.lines = b"", b"", []

    def now(self):
        return time.time() - self.t0

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            self.buf += d
            self.part += d
            while b"\n" in self.part:
                line, self.part = self.part.split(b"\n", 1)
                text = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", line).replace(b"\r", b"").decode("latin1").rstrip()
                if text:
                    self.lines.append((self.now(), text))

    def until(self, pattern, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.pump(0.1)
            if re.search(pattern, self.buf):
                return True
        return False

    def send(self, text):
        self.s.sendall(text.encode())

    def monitor(self, cmd):
        m = socket.create_connection(("127.0.0.1", MONITOR))
        m.settimeout(1)
        try:
            m.recv(4096)
            m.sendall(cmd.encode() + b"\n")
            time.sleep(0.3)
            m.recv(65536)
        except OSError:
            pass
        m.close()

    def shell(self):
        """Log in as myos on the serial console (the live disk's empty password)."""
        self.send("myos\n")
        self.until(rb"[Pp]assword: ", 15)
        self.send("\n")
        self.until(rb"\$ $", 30)

    def stop(self):
        self.q.kill()


def first_boot(img, scratch):
    """Boot a copy once, so its first-boot scripts have run; return the copy."""
    copy = os.path.join(scratch, "everyday.img")
    subprocess.run(["cp", "-c", img, copy], check=True)         # APFS clone: instant
    m = Machine(copy, scratch, keep=True, net=False)
    m.until(rb"login screen is up", 300)
    m.shell()
    m.send("sync; echo SYNCED\n")
    m.until(rb"SYNCED\r?\n", 60)
    time.sleep(5)                                               # let the block cache settle
    m.stop()
    return copy


def main(argv):
    if not argv or argv[0].startswith("-"):
        print(__doc__)
        return 2
    img, opts = argv[0], set(argv[1:])
    scratch = tempfile.mkdtemp(prefix="boottime-")
    try:
        if "--everyday" in opts:
            img = first_boot(img, scratch)
        m = Machine(img, scratch, keep="--everyday" in opts)
        m.until(rb"login screen is up", 300)
        t_login = m.now()
        seen = {}
        if "--no-login" not in opts:
            time.sleep(1)
            m.shell()
            m.monitor("sendkey ret")
            time.sleep(0.5)
            m.monitor("sendkey ret")
            t_enter = m.now()
            for _ in range(60):
                m.send('ps -eo comm= | tr "\\n" " "; echo; echo PSEND\n')
                m.until(rb"PSEND\s*\r?\n.*\$ $", 3)
                tail = m.buf[-4000:].decode("latin1")
                for w in DESKTOP:
                    if w not in seen and re.search(r"\b" + re.escape(w[:15]) + r"\b", tail):
                        seen[w] = m.now()
                if len(seen) == len(DESKTOP):
                    break
                time.sleep(0.5)
        m.stop()
        last = 0.0
        for t, text in m.lines:
            if re.search(MILESTONES, text):
                print(f"{t:7.2f}  (+{t - last:5.2f})  {text[:100]}")
                last = t
        print(f"{t_login:7.2f}            == the login screen is up")
        if "--no-login" not in opts:
            print(f"{t_enter:7.2f}            == Return pressed on the login screen")
            for w, t in sorted(seen.items(), key=lambda x: x[1]):
                print(f"{t:7.2f}            == {w} running, {t - t_enter:.1f} s after logging in")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
