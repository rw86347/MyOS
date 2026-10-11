#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# Interactive ARM64 virt (serial on this terminal). Needs a real TTY.
set -e
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
kernel=$root/build/arm64/kernel.elf
if [ ! -f "$kernel" ]; then
    echo "missing $kernel — run: make ARCH=arm64" >&2
    exit 1
fi
if ! [ -t 0 ] || ! [ -t 1 ]; then
    echo "run-arm64: stdin/stdout must be a terminal (use Cursor Terminal, not a pipe/task)." >&2
    exit 1
fi
echo "MyOS ARM64 on QEMU virt — type at bash-5.2#, Enter to run, Ctrl+A then X to quit QEMU."
exec qemu-system-aarch64 -machine virt -cpu max -m 256M -display none \
    -kernel "$kernel" -serial stdio
