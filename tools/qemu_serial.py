#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""qemu_serial — a QEMU machine driven over its serial console, for tests.

The machine starts only once the test is connected to its serial port (no
output is lost); expect() waits for text, login() logs in on the console
(MyOS's console reads the serial port like the keyboard), and sh() runs a
command and returns its exit status and output. Used by tools/test-tablet.py
and the live disk's live/tools/test-install.py.
"""
import re, socket, subprocess, sys, time


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Machine:
    """One QEMU run, with its serial console on a socket."""

    def __init__(self, args, log_path):
        self.port = free_port()
        self.log = open(log_path, "wb")
        self.buf = b""                  # everything the serial port said
        self.pos = 0                    # what has been matched so far
        self.n = 0
        # wait=on: QEMU starts the machine only once we are connected, so no
        # output is lost.
        self.q = subprocess.Popen(args + [
            "-display", "none", "-no-reboot", "-monitor", "none",
            "-serial", "tcp:127.0.0.1:%d,server=on,wait=on" % self.port],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            try:
                self.s = socket.create_connection(("127.0.0.1", self.port))
                break
            except OSError:
                if self.q.poll() is not None:
                    sys.exit("test-install: QEMU did not start: %s" %
                             self.q.stderr.read().decode(errors="replace"))
                time.sleep(0.1)
        else:
            sys.exit("test-install: no serial port from QEMU")
        self.s.setblocking(False)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                d = self.s.recv(65536)
                if not d:
                    return False            # QEMU has gone
                self.buf += d
                self.log.write(d)
                self.log.flush()
            except BlockingIOError:
                time.sleep(0.02)
        return True

    def expect(self, pattern, timeout, what):
        """Wait until the serial output (after what was matched before)
        matches pattern; the match."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while True:
            m = rx.search(self.buf, self.pos)
            if m:
                self.pos = m.end()
                return m
            if time.time() > end or not self.pump(0.2):
                raise Failed("%s: not seen in %d s" % (what, timeout))

    def send(self, text):
        self.s.setblocking(True)
        self.s.sendall(text.encode())
        self.s.setblocking(False)

    def login(self, user, password):
        # A fresh prompt: Enter makes agetty print it again.
        self.pos = len(self.buf)
        self.send("\r")
        self.expect(rb"login: ?", 60, "the console's login prompt")
        self.send(user + "\r")
        if password is not None:
            self.expect(rb"[Pp]assword: ?", 30, "the password prompt")
            self.send(password + "\r")
        self.expect(rb"[$#] ", 60, "a shell prompt after logging in as " + user)
        self.sh("stty -echo; export PS1='$ ' LC_ALL=C")

    def sh(self, cmd, timeout=120, check=True):
        """Run cmd in the logged-in shell: (exit status, output). The
        markers are made by printf, so the echo of the typed line can't be
        mistaken for them."""
        self.n += 1
        n = self.n
        self.send("printf '\\n__%%s__\\n' B%d; { %s\n}; printf '__%%s=%%d__\\n' E%d $?\r"
                  % (n, cmd, n))
        b = self.expect(rb"__B%d__\r?\n" % n, 60, "the start of: " + cmd)
        e = self.expect(rb"__E%d=(\d+)__" % n, timeout, "the end of: " + cmd)
        out = self.buf[b.end():e.start()].decode(errors="replace")
        out = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", out).replace("\r", "")
        rc = int(e.group(1))
        if check and rc:
            raise Failed("`%s` exited %d:\n%s" % (cmd, rc, out[-2000:]))
        return rc, out

    def quit(self):
        self.q.kill()
        self.q.wait()
        self.log.close()


class Failed(Exception):
    pass
