#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""test-sleep.py — make test-sleep: put MyOS to sleep in QEMU and wake it

    test-sleep.py ARCH LOG qemu-system-... ARGS...

Starts QEMU with its monitor on our pipe and its serial port in LOG, waits
for the shell, and plays the power daemon (myos-powerd) from the host:

  1. a busy loop in the background; QEMU's CPU time over 4 s, awake
  2. the lid shuts (a CPU added: kernel/fakelid_test.c) ->
     echo mem > /sys/power/state; QEMU's CPU time over 4 s, asleep
  3. a key with the lid shut: it stays asleep
  4. the lid opens: awake. The wall clock jumped, the uptime did not; the
     kernel's time asleep is the one this Mac saw (within 1.5 s)
  5. asleep again, a key wakes it; again, the power button does
  6. the lid's and the power button's events, as /dev/input gave them

Exit status 0 if every check passed.
"""
import os
import re
import subprocess
import sys
import time

ARCH, LOG = sys.argv[1], sys.argv[2]
QEMU = sys.argv[3:]
EV = 24 if ARCH == 'x86_64' else 16             # sizeof(struct input_event)
fails = []
cpus = 0


def log():
    try:
        with open(LOG, 'rb') as f:
            text = f.read().decode('latin-1')
    except FileNotFoundError:
        return ''
    return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]|\r', '', text)


def wait_for(pattern, timeout, start=0):
    """The first match in the log after offset start, waiting up to timeout s."""
    end = time.time() + timeout
    while True:
        m = re.search(pattern, log()[start:], re.M)
        if m or time.time() > end:
            return m
        time.sleep(0.2)


def check(ok, what):
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)
    if not ok:
        fails.append(what)


def monitor(cmd):
    qemu.stdin.write((cmd + '\n').encode())
    qemu.stdin.flush()


def type_line(text):
    keys = subprocess.run(['tools/qemu-type.sh', text], capture_output=True, text=True).stdout
    for k in keys.split('\n'):
        if k:
            monitor(k)
            time.sleep(0.05)


def cpu_seconds():
    """QEMU's CPU time so far ([[dd-]hh:]mm:ss.ss from ps)."""
    out = subprocess.run(['ps', '-o', 'time=', '-p', str(qemu.pid)],
                         capture_output=True, text=True).stdout.strip()
    s = 0.0
    for part in out.replace('-', ':').split(':'):
        s = s * 60 + float(part)
    return s


def cpu_share(seconds):
    c0, t0 = cpu_seconds(), time.time()
    time.sleep(seconds)
    return (cpu_seconds() - c0) / (time.time() - t0)


def move_lid():
    """A new CPU (QEMU runs with -smp 1,maxcpus=N): its notify moves the lid."""
    global cpus
    cpus += 1
    monitor('device_add %s-%s-cpu,id=lid%d,socket-id=0,core-id=%d,thread-id=0'
            % (QEMU[QEMU.index('-cpu') + 1], ARCH, cpus, cpus))


def sleep_round(mark, what):
    m = wait_for(r'sleep: asleep \((\d+) ms to stop\)', 20, mark)
    check(m, 'asleep (%s), in %s ms' % (what, m[1] if m else '?'))
    return m


if os.path.exists(LOG):
    os.remove(LOG)
qemu = subprocess.Popen(QEMU + ['-serial', 'file:' + LOG, '-monitor', 'stdio'],
                        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
try:
    if not wait_for(r'Welcome to MyOS', 120):
        check(False, 'the shell came up')
        raise SystemExit(1)
    time.sleep(1)
    type_line('mkdir -p /sys; mount -t sysfs none /sys; cat /sys/power/state /sys/power/mem_sleep;'
              ' grep -h NAME /sys/devices/virtual/input/input[34]/uevent')
    type_line('(dd if=/dev/input/event3 bs=%d count=4 2>/dev/null | od -v -An -tx1 > /tmp/lid) &' % EV)
    type_line('(dd if=/dev/input/event4 bs=%d count=8 2>/dev/null | od -v -An -tx1 > /tmp/pwr) &' % EV)
    type_line('(while :; do :; done) & B=$!')
    time.sleep(1)
    awake = cpu_share(4)

    # The lid shuts: sleep. A key: still asleep. The lid opens: awake.
    mark = len(log())
    type_line('echo T0 $(date +%s) $(cut -d" " -f1 /proc/uptime)')
    move_lid()
    check(wait_for(r'lid: shut \(notify 0x80\)', 10, mark), 'the lid shuts (Notify 0x80)')
    type_line('echo mem > /sys/power/state; echo T1 $(date +%s) $(cut -d" " -f1 /proc/uptime)')
    host_asleep = None
    if sleep_round(mark, 'the lid shut, a busy loop running'):
        host_asleep = time.time()
        asleep = cpu_share(4)
        check(asleep < max(0.25, awake / 3),
              'QEMU rests: %.0f%% of a CPU asleep, %.0f%% awake' % (asleep * 100, awake * 100))
        key_mark = len(log())
        monitor('sendkey a')
        time.sleep(2)
        check(not re.search('sleep: woken', log()[key_mark:]), 'a key with the lid shut: still asleep')
    move_lid()
    woke = wait_for(r'sleep: woken by the lid after ([\d.]+) s asleep', 10, mark)
    check(woke, 'the lid opens: awake')
    if woke and host_asleep:
        host = time.time() - host_asleep
        check(abs(float(woke[1]) - host) <= 1.5,
              'asleep %s s by the kernel, %.1f s by this Mac' % (woke[1], host))
    t0 = wait_for(r'^T0 (\d+) ([\d.]+)', 1, mark)
    t1 = wait_for(r'^T1 (\d+) ([\d.]+)', 10, mark)
    if t0 and t1:
        wall, up = int(t1[1]) - int(t0[1]), float(t1[2]) - float(t0[2])
        check(wall >= 6 and wall - up >= 4,
              'the wall clock moved %d s, the uptime %.1f s (it leaves sleep out)' % (wall, up))
    else:
        check(False, 'the clocks read before and after')

    # A key wakes it (the lid is open), and the power button does.
    mark = len(log())
    type_line('echo mem > /sys/power/state; echo T2')
    if sleep_round(mark, 'from the shell'):
        time.sleep(1)
        monitor('sendkey a')
        check(wait_for(r'sleep: woken by a key', 10, mark), 'a key wakes it')
    check(wait_for(r'^T2', 10, mark), 'the write returns after waking')
    mark = len(log())
    type_line('echo mem > /sys/power/state; echo T3')
    if sleep_round(mark, 'again'):
        time.sleep(1)
        monitor('system_powerdown')
        check(wait_for(r'sleep: woken by the power button', 10, mark), 'the power button wakes it')
    wait_for(r'^T3', 10, mark)
    time.sleep(1)
    monitor('system_powerdown')                 # awake now: KEY_POWER, for the daemon
    time.sleep(1)

    mark = len(log())
    type_line('kill $B; cat /tmp/lid /tmp/pwr; echo DONE')
    wait_for(r'^DONE', 15, mark)
    hexes = ' '.join(re.findall(r'\b[0-9a-f]{2}\b', log()[mark:]))
    check('05 00 00 00 01 00 00 00' in hexes, 'event3: SW_LID 1, the lid shut')
    check('05 00 00 00 00 00 00 00' in hexes, 'event3: SW_LID 0, the lid open')
    check('01 00 8f 00 01 00 00 00' in hexes, 'event4: KEY_WAKEUP, the press that woke it')
    check('01 00 74 00 01 00 00 00' in hexes, 'event4: KEY_POWER, a press while awake')

    L = log()
    check(re.search(r'acpi: sleep: s2idle \(low-power S0 idle\) \w+, S3 \w+', L),
          'the boot log says which sleep the firmware offers')
    check(re.search(r'acpi: lid: \\_SB_?\.LID0, open', L), 'the lid found, open')
    check(re.search(r'acpi: lid: wakes from S3 by GPE 0x0d \(on, notifies the lid\)', L),
          "the lid's wake GPE turned on")
    check(re.search(r'fakelid: a CPU added .* \([1-9]\d* CPU slots\)', L), 'a CPU added moves the lid')
    check(re.search(r'^freeze mem$', L, re.M) and re.search(r'^\[s2idle\]$', L, re.M),
          '/sys/power/state and mem_sleep')
    check('NAME="Lid Switch"' in L and 'NAME="Power Button"' in L, 'input3 and input4: their names')
    for m in re.finditer(r'sleep: woken by (.*?) after ([\d.]+) s asleep \(the CMOS clock moved (-?\d+) s, '
                         r'the TSC counted ([\d.]+) s; the tick off (\d+)% of it, the CPU woke (\d+) '
                         r'times\), awake in (\d+) ms', L):
        print('      woken by %s after %s s (CMOS %s s, TSC %s s); the tick off %s%%; the CPU woke %s'
              ' times; awake in %s ms' % m.groups())
finally:
    monitor('quit')
    try:
        qemu.wait(timeout=10)
    except subprocess.TimeoutExpired:
        qemu.kill()

if fails:
    print('SLEEP TEST FAILED (%s): %d checks' % (ARCH, len(fails)))
    sys.exit(1)
print('SLEEP TEST OK (%s)' % ARCH)
