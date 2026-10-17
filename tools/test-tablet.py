#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""test-tablet — an absolute pointer (QEMU's usb-tablet: what UTM hands the
Mac's pointer to, and what a USB touch screen looks like) reaches
/dev/input/event2.

  tools/test-tablet.py IMG OVMF_CODE OVMF_VARS WORK

IMG is the 64-bit shell image (make ARCH=x86_64 uefi). QEMU's control
socket (QMP) puts the tablet at exact places and presses its button; the
shell, over the serial console, records what event2 says. The machine has
no PS/2 controller and no USB mouse, so the tablet is the only pointer and
every event goes to it.
"""
import json, os, shutil, socket, sys, time

import qemu_serial as ti                    # QEMU with a serial console


def main():
    img, code, varsfd, work = sys.argv[1:5]
    os.makedirs(work, exist_ok=True)
    vars_copy = os.path.join(work, "tablet-vars.fd")
    shutil.copy(varsfd, vars_copy)
    qport = ti.free_port()
    m = ti.Machine(["qemu-system-x86_64", "-machine", "q35,i8042=off", "-m", "1G", "-cpu", "qemu64",
                    "-drive", "if=pflash,format=raw,readonly=on,file=" + code,
                    "-drive", "if=pflash,format=raw,file=" + vars_copy, "-net", "none",
                    "-drive", "if=none,format=raw,id=b,snapshot=on,file=" + img,
                    "-device", "ide-hd,drive=b,bus=ide.0,bootindex=0",
                    "-device", "nec-usb-xhci,id=usb-bus", "-device", "usb-tablet,bus=usb-bus.0",
                    "-qmp", "tcp:127.0.0.1:%d,server=on,wait=off" % qport],
                   os.path.join(work, "tablet-serial.log"))
    q = None

    def send(*events):
        q.sendall((json.dumps({"execute": "input-send-event",
                               "arguments": {"events": list(events)}}) + "\n").encode())
        time.sleep(0.5)

    try:
        m.expect(rb"Welcome to MyOS", 120, "the shell")
        m.expect(rb"# ", 30, "its prompt")
        q = socket.create_connection(("127.0.0.1", qport))
        q.sendall(b'{"execute": "qmp_capabilities"}\n')
        _, found = m.sh("dmesg | grep 'usb-hid'", 30, check=False)
        size = 24                           # struct input_event on x86_64
        m.send("dd if=/dev/input/event2 of=/tmp/t bs=%d count=7 2>/dev/null &\r" % size)
        time.sleep(1)
        send({"type": "abs", "data": {"axis": "x", "value": 16384}},
             {"type": "abs", "data": {"axis": "y", "value": 8192}})
        send({"type": "btn", "data": {"down": True, "button": "left"}})
        send({"type": "btn", "data": {"down": False, "button": "left"}})
        time.sleep(1)
        _, out = m.sh("od -An -v -tu2 -w%d /tmp/t" % size, 30, check=False)
    finally:
        m.quit()
        os.unlink(vars_copy)
    # Each line ends in type, code, value (low, high): EV_ABS 3, EV_KEY 1;
    # ABS_X 0, ABS_Y 1, BTN_LEFT 272.
    events = [" ".join(l.split()[-4:]) for l in out.splitlines() if len(l.split()) >= 4]
    checks = [("the tablet is found", "QEMU USB Tablet: a touch screen or tablet" in found),
              ("x = 16384 (the middle)", "3 0 16384 0" in events),
              ("y = 8192 (a quarter down)", "3 1 8192 0" in events),
              ("the button goes down", "1 272 1 0" in events),
              ("and up", "1 272 0 0" in events)]
    print(found.strip())
    print("\n".join(events))
    for what, good in checks:
        print("  %s %s" % ("ok  " if good else "FAIL", what))
    bad = [w for w, g in checks if not g]
    print("TABLET TEST %s" % ("FAILED: " + "; ".join(bad) if bad else "OK: absolute pointer on event2"))
    return 1 if bad else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ti.Failed as e:
        print("test-tablet: FAILED: %s" % e)
        sys.exit(1)
