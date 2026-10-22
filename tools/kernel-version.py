#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""kernel-version — the record of one kernel build, as KEY=value lines.

    tools/kernel-version.py --version 20261021-gabc1234 --arch x86_64 build/x86_64/kernel.bin

The live disk's /usr/lib/myos/release carries these lines (live/live.mk), so
an upgrade can say which kernel a disk has and check that the file it starts
is that kernel (docs/INSTALLER-PLAN.md, "Upgrading from the stick").
"""
import argparse
import hashlib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--arch", required=True)
    ap.add_argument("kernel")
    a = ap.parse_args()
    with open(a.kernel, "rb") as f:
        data = f.read()
    print("KERNEL_VERSION=%s" % a.version)
    print("KERNEL_ARCH=%s" % a.arch)
    print("KERNEL_SIZE=%d" % len(data))
    print("KERNEL_SHA256=%s" % hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    main()
