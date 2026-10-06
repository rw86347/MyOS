#!/usr/bin/env python3
"""iwlwifi-check.py — compile Intel's Wi-Fi driver (the AX201's files,
drivers/net/wireless/intel/iwlwifi) against MyOS's Linux-API headers,
syntax only, and report which files compile and the first errors of those
that don't. The measure of docs/WIFI-PLAN.md W2a: every file compiling.

  tools/iwlwifi-check.py            summary + the most common errors
  tools/iwlwifi-check.py FILE.c     every error of one file
  tools/iwlwifi-check.py --link     compile them all for real, and list what
                                    they call that the kernel lacks (W6)
"""
import collections, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRV = 'drivers/net/wireless/intel/iwlwifi'
CC = 'x86_64-elf-gcc'
CONFIG = ['CONFIG_IWLWIFI', 'CONFIG_IWLMVM', 'CONFIG_PCI', 'CONFIG_MAC80211', 'CONFIG_CFG80211',
          'CONFIG_PM']
FLAGS = ['-std=gnu11', '-ffreestanding', '-fsyntax-only', '-O2', '-m64', '-mno-red-zone',
         '-mcmodel=kernel', '-mgeneral-regs-only', '-DARCH_X86_64', '-D__KERNEL__',
         '-DKBUILD_MODNAME="iwlwifi"', '-Wno-override-init',
         '-Iinclude', '-Iarch/x86_64/include', '-I' + DRV, '-include', 'linux/compiler_types.h',
         '-fmax-errors=50', '-Wno-address-of-packed-member', '-Wall', '-funsigned-char', '-Wno-pointer-sign', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-but-set-variable'] + ['-D%s=1' % c for c in CONFIG]

def sources():
    out = []
    for root, _, files in os.walk(os.path.join(ROOT, DRV)):
        for f in files:
            if f.endswith('.c'):
                out.append(os.path.relpath(os.path.join(root, f), ROOT))
    return sorted(out)

def compile_one(src):
    p = subprocess.run([CC] + FLAGS + [src], cwd=ROOT, capture_output=True, text=True)
    errs = [l for l in p.stderr.splitlines() if ': error:' in l or 'fatal error' in l]
    return p.returncode == 0, errs, p.stderr

def link():
    """Every file to an object, then the symbols they need that neither they
    nor build/x86_64/kernel.elf define: what the shim must still provide."""
    out = os.path.join(ROOT, 'build/x86_64/iwlwifi')
    objs, failed = [], []
    for s in sources():
        o = os.path.join(out, s[len(DRV) + 1:].replace('/', '_')[:-2] + '.o')
        os.makedirs(out, exist_ok=True)
        flags = [f for f in FLAGS if f != '-fsyntax-only'] + ['-c', '-o', o]
        p = subprocess.run([CC] + flags + [s], cwd=ROOT, capture_output=True, text=True)
        (objs.append(o) if p.returncode == 0 else failed.append(s))
    if failed:
        print('did not compile:', ' '.join(failed))
    whole = os.path.join(out, 'iwlwifi.o')
    subprocess.run(['x86_64-elf-ld', '-r', '-o', whole] + objs, check=True)
    nm = lambda args: subprocess.run(['x86_64-elf-nm'] + args, capture_output=True, text=True).stdout
    need = {l.split()[-1] for l in nm(['-u', whole]).splitlines()}
    have = {l.split()[-1] for l in nm(['--defined-only', os.path.join(ROOT, 'build/x86_64/kernel.elf')]).splitlines()
            if len(l.split()) == 3}
    missing = sorted(need - have)
    users = collections.defaultdict(list)
    for o in objs:
        for l in nm(['-u', o]).splitlines():
            if l.split()[-1] in missing:
                users[l.split()[-1]].append(os.path.basename(o)[:-2])
    print('%d objects; %d symbols needed from outside, %d still missing:' % (len(objs), len(need), len(missing)))
    for m in missing:
        print('  %-44s %s' % (m, ' '.join(users[m][:4]) + (' ...' if len(users[m]) > 4 else '')))

def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--link':
        link()
        return
    if len(sys.argv) > 1:
        ok, errs, text = compile_one(sys.argv[1])
        print(text)
        print('OK' if ok else '%d errors' % len(errs))
        return
    srcs = sources()
    good, bad = [], {}
    for s in srcs:
        ok, errs, _ = compile_one(s)
        (good.append(s) if ok else bad.__setitem__(s, errs))
    print('compiling: %d of %d files' % (len(good), len(srcs)))
    first = collections.Counter()
    for s, errs in bad.items():
        for e in errs:
            msg = re.sub(r'^.*?error: ', '', e)
            first[msg] += 1
    for f, errs in sorted(bad.items(), key=lambda x: len(x[1])):
        print('  %-40s %3d  %s' % (f[len(DRV) + 1:], len(errs), re.sub(r'^.*?error: ', '', errs[0])[:90] if errs else ''))
    print('\nmost common errors:')
    for msg, n in first.most_common(40):
        print('  %4d  %s' % (n, msg[:150]))

main()
