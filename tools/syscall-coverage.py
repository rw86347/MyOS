#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""syscall-coverage.py — which Linux system calls MyOS has, by CPU.

Compares the kernel's tables (kernel/syscall.c: [NR_x] and [NR64_x] entries,
numbers from include/syscalls.h and include/syscall64.h) with Linux's own
lists, arch/x86/entry/syscalls/syscall_{32,64}.tbl, read from the Linux
source tarball the Wi-Fi work already fetched (build/ext/linux-7.2.9.tar.xz).

  tools/syscall-coverage.py            both CPUs
  tools/syscall-coverage.py x86_64     one

"missing": Linux has it, MyOS answers ENOSYS (and logs "syscall N not
implemented"). "ignored": MyOS says success and does nothing (sys_nop).
On i386 many missing ones are the legacy calls docs/DESIGN.md leaves out on
purpose (16-bit uids, old stat, ...): musl uses the modern ones.
"""
import os, re, sys, tarfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
TARBALL = os.path.join(ROOT, 'build/ext/linux-7.2.9.tar.xz')
TBL = 'linux-7.2.9/arch/x86/entry/syscalls/syscall_%s.tbl'


def linux_table(bits):
    with tarfile.open(TARBALL) as t:
        text = t.extractfile(TBL % bits).read().decode()
    abis = ('common', '64') if bits == '64' else ('i386',)
    out = {}
    for line in text.splitlines():
        f = line.split()
        if len(f) >= 4 and not line.startswith('#') and f[1] in abis:
            out[int(f[0])] = f[2]           # only entries with an entry point
    return out


def our_table(prefix, header):
    nums = {m.group(1): int(m.group(2)) for m in
            re.finditer(r'#define\s+%s(\w+)\s+(\d+)' % prefix,
                        open(os.path.join(ROOT, header)).read())}
    src = open(os.path.join(ROOT, 'kernel/syscall.c')).read()
    return {nums[m.group(1)]: m.group(2) for m in
            re.finditer(r'\[%s(\w+)\]\s*=\s*(\w+)' % prefix, src) if m.group(1) in nums}


def report(arch):
    bits, prefix, header = (('64', 'NR64_', 'include/syscall64.h') if arch == 'x86_64'
                            else ('32', 'NR_', 'include/syscalls.h'))
    linux, ours = linux_table(bits), our_table(prefix, header)
    missing = sorted(n for n in linux if n not in ours)
    ignored = sorted(n for n in linux if ours.get(n) == 'sys_nop')
    print('%s: Linux has %d system calls; MyOS implements %d, ignores %d, lacks %d'
          % (arch, len(linux), len(linux) - len(missing) - len(ignored), len(ignored), len(missing)))
    print('  ignored:', ' '.join('%s(%d)' % (linux[n], n) for n in ignored))
    print('  missing:', ' '.join('%s(%d)' % (linux[n], n) for n in missing))


for arch in sys.argv[1:] or ['x86_64', 'i386']:
    report(arch)
