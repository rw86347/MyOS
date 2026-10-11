#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""Pack BusyBox, bash, musl and a login /init into a newc cpio."""
import os
import sys

INIT_SCRIPT = b"""#!/bin/sh
echo bash from MyOS
cat /hello.txt
exec /bin/bash --noediting -i
"""

APPLETS = ("sh", "ash", "cat", "echo", "ls", "mkdir", "pwd", "ln", "uname")


def align4(n: int) -> int:
    return (n + 3) & ~3


def put(out, name: str, mode: int, data: bytes, ino: int) -> None:
    raw = name.encode("utf-8") + b"\0"
    namesize = len(raw)
    hdr = (
        "070701"
        + f"{ino:08x}{mode:08x}{0:08x}{0:08x}{1:08x}{0:08x}"
        + f"{len(data):08x}{0:08x}{0:08x}{0:08x}{0:08x}{namesize:08x}{0:08x}"
    ).encode("ascii")
    out.write(hdr)
    out.write(raw)
    out.write(b"\0" * (align4(110 + namesize) - 110 - namesize))
    out.write(data)
    out.write(b"\0" * (align4(len(data)) - len(data)))


def put_file(out, name: str, path: str, mode: int, ino: int) -> None:
    put(out, name, mode, open(path, "rb").read(), ino)


def put_link(out, name: str, target: str, ino: int) -> None:
    put(out, name, 0o120777, target.encode("utf-8"), ino)


def main() -> None:
    if len(sys.argv) != 4:
        sys.exit("usage: pack-arm64-initrd.py busybox.static alpine-root out.cpio")
    bb, root, dest = sys.argv[1], sys.argv[2], sys.argv[3]
    ino = 1
    with open(dest, "wb") as out:
        put(out, "init", 0o100755, INIT_SCRIPT, ino)
        ino += 1
        put(out, "hello.txt", 0o100644, b"cat from initramfs\n", ino)
        ino += 1
        put_file(out, "bin/busybox", bb, 0o100755, ino)
        ino += 1
        for app in APPLETS:
            put_link(out, f"bin/{app}", "/bin/busybox", ino)
            ino += 1
        put_file(out, "bin/bash", os.path.join(root, "bin/bash"), 0o100755, ino)
        ino += 1
        put_file(
            out,
            "lib/ld-musl-aarch64.so.1",
            os.path.join(root, "lib/ld-musl-aarch64.so.1"),
            0o100755,
            ino,
        )
        ino += 1
        put_link(out, "lib/libc.musl-aarch64.so.1", "ld-musl-aarch64.so.1", ino)
        ino += 1
        put_file(
            out,
            "usr/lib/libreadline.so.8.2",
            os.path.join(root, "usr/lib/libreadline.so.8.2"),
            0o100755,
            ino,
        )
        ino += 1
        put_link(out, "usr/lib/libreadline.so.8", "libreadline.so.8.2", ino)
        ino += 1
        put_link(out, "lib/libreadline.so.8", "/usr/lib/libreadline.so.8.2", ino)
        ino += 1
        put_file(
            out,
            "usr/lib/libncursesw.so.6.4",
            os.path.join(root, "usr/lib/libncursesw.so.6.4"),
            0o100755,
            ino,
        )
        ino += 1
        put_link(out, "usr/lib/libncursesw.so.6", "libncursesw.so.6.4", ino)
        ino += 1
        put_link(out, "lib/libncursesw.so.6", "/usr/lib/libncursesw.so.6.4", ino)
        ino += 1
        put(out, "etc/ld-musl-aarch64.path", 0o100644, b"/lib:/usr/lib\n", ino)
        ino += 1
        term = os.path.join(root, "etc/terminfo/l/linux")
        if os.path.isfile(term):
            put_file(out, "etc/terminfo/l/linux", term, 0o100644, ino)
            ino += 1
        put(out, "TRAILER!!!", 0, b"", ino)


if __name__ == "__main__":
    main()
