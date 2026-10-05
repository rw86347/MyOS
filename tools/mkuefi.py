#!/usr/bin/env python3
"""mkuefi.py — a GPT disk with one FAT32 EFI System Partition.

    tools/mkuefi.py --out build/myos-uefi.img --esp-mb 32 \
        --file EFI/BOOT/BOOTX64.EFI=build/BOOTX64.EFI \
        --file EFI/MYOS/KERNEL.BIN=build/kernel.bin ...

Firmware on a USB stick looks in the ESP for /EFI/BOOT/BOOTX64.EFI.
mtools writes the FAT; we only write the GPT and the protective MBR.
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib
from pathlib import Path

ESP_GUID = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
LINUX_GUID = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")
SECTOR = 512


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def guid_bytes(u: uuid.UUID) -> bytes:
    # GPT stores GUIDs mixed-endian: first three fields little-endian.
    return u.bytes_le


def run(cmd: list[str]) -> None:
    env = os.environ.copy()
    env["MTOOLS_SKIP_CHECK"] = "1"
    subprocess.check_call(cmd, env=env)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--esp-mb", type=int, default=32)
    ap.add_argument("--file", action="append", default=[],
                    help="ESP_PATH=host_path (repeatable)")
    ap.add_argument("--root-file", help="raw ext2 image (or a MyOS live disk)")
    ap.add_argument("--root-skip", type=int, default=0,
                    help="sectors to skip in --root-file")
    ap.add_argument("--root-sectors", type=int, default=0,
                    help="sectors to copy (0 = rest of the file)")
    ap.add_argument("--root-from-mbr", action="store_true",
                    help="take the 0x83 partition from --root-file's MBR")
    args = ap.parse_args()
    files = []
    for item in args.file:
        if "=" not in item:
            sys.exit(f"bad --file {item!r}, want ESP_PATH=host_path")
        dest, src = item.split("=", 1)
        files.append((dest.lstrip("/"), Path(src)))
        if not files[-1][1].is_file():
            sys.exit(f"missing {src}")

    start = 2048  # 1 MiB
    esp_sectors = args.esp_mb * 1024 * 1024 // SECTOR
    root_path = Path(args.root_file) if args.root_file else None
    root_skip = args.root_skip
    root_sectors = args.root_sectors
    if root_path:
        if args.root_from_mbr:
            mbr = root_path.read_bytes()[:512]
            for i in range(4):
                typ = mbr[446 + i * 16 + 4]
                lba, n = struct.unpack_from("<II", mbr, 446 + i * 16 + 8)
                if typ == 0x83 and n:
                    root_skip, root_sectors = lba, n
                    break
            else:
                sys.exit(f"no 0x83 partition in {root_path}")
        root_bytes = root_path.stat().st_size
        if not root_sectors:
            root_sectors = (root_bytes - root_skip * SECTOR) // SECTOR
        if root_skip * SECTOR + root_sectors * SECTOR > root_bytes:
            sys.exit("root image shorter than skip+sectors")
    else:
        root_sectors = 0
    root_start = start + esp_sectors
    # GPT backup at the end: 1 header + 32 entry sectors (128 entries)
    disk_sectors = root_start + root_sectors + 33
    # round up so the image is a whole megabyte
    disk_sectors = (disk_sectors + 2047) // 2048 * 2048

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as td:
        part = Path(td) / "esp.img"
        part.write_bytes(b"\0" * (esp_sectors * SECTOR))
        # FAT32: many real firmwares ignore a FAT16 ESP. 64 MiB with
        # 512-byte clusters is above the 65525-cluster FAT32 minimum.
        fmt = ["mformat", "-i", str(part), "-c", "1",
               "-T", str(esp_sectors), "-h", "64", "-s", "32",
               "-H", str(start), "-v", "EFI", "::"]
        if esp_sectors >= 131072:
            fmt.insert(3, "-F")
        run(fmt)
        made = set()
        for dest, src in files:
            parts = dest.split("/")
            for i in range(1, len(parts)):
                d = "/".join(parts[:i])
                if d not in made:
                    run(["mmd", "-i", str(part), f"::{d}"])
                    made.add(d)
            run(["mcopy", "-i", str(part), "-o", str(src), f"::{dest}"])
        fat = part.read_bytes()

    disk_guid = uuid.uuid4()
    entries = bytearray(128 * 128)
    name = "EFI System".encode("utf-16le").ljust(72, b"\0")
    # bit 0 = Required Partition; bit 63 = Windows "do not automount"
    esp_attr = 0x8000000000000001
    entries[0:128] = (
        guid_bytes(ESP_GUID) + guid_bytes(uuid.uuid4())
        + struct.pack("<QQQ", start, start + esp_sectors - 1, esp_attr)
        + name
    )
    if root_sectors:
        rname = "myos-root".encode("utf-16le").ljust(72, b"\0")
        entries[128:256] = (
            guid_bytes(LINUX_GUID) + guid_bytes(uuid.uuid4())
            + struct.pack("<QQQ", root_start, root_start + root_sectors - 1, 0)
            + rname
        )
    entries_crc = crc32(entries)

    def header(current: int, backup: int, entries_lba: int) -> bytes:
        h = bytearray(SECTOR)
        h[0:8] = b"EFI PART"
        struct.pack_into("<IIIIQQQQ", h, 8,
                         0x00010000, 92, 0, 0,
                         current, backup, 34, disk_sectors - 34)
        h[56:72] = guid_bytes(disk_guid)
        struct.pack_into("<QII", h, 72, entries_lba, 128, 128)
        struct.pack_into("<I", h, 88, entries_crc)
        struct.pack_into("<I", h, 16, crc32(h[:92]))
        return bytes(h)

    # Hybrid MBR: a 0xEF ESP the USB firmware can boot, plus Linux if
    # present. Protective-only 0xEE is what many sticks skip.
    mbr = bytearray(SECTOR)
    mbr[446:462] = struct.pack("<BBBBBBBBII",
                               0x80, 0x00, 0x02, 0x00,
                               0xEF, 0xFE, 0xFF, 0xFF,
                               start, esp_sectors)
    if root_sectors:
        mbr[462:478] = struct.pack("<BBBBBBBBII",
                                   0x00, 0xFE, 0xFF, 0xFF,
                                   0x83, 0xFE, 0xFF, 0xFF,
                                   root_start, root_sectors)
    mbr[510:512] = b"\x55\xAA"

    with out.open("wb") as f:
        f.truncate(disk_sectors * SECTOR)
        f.seek(0)
        f.write(mbr)
        f.write(header(1, disk_sectors - 1, 2))
        f.write(entries)
        f.seek(start * SECTOR)
        f.write(fat)
        if root_sectors:
            with root_path.open("rb") as src:
                src.seek(root_skip * SECTOR)
                left = root_sectors * SECTOR
                while left:
                    chunk = src.read(min(left, 8 * 1024 * 1024))
                    if not chunk:
                        sys.exit("root image ended early")
                    f.write(chunk)
                    left -= len(chunk)
        f.seek((disk_sectors - 33) * SECTOR)
        f.write(entries)
        f.write(header(disk_sectors - 1, 1, disk_sectors - 33))

    extra = f", root {root_sectors // 2048} MiB" if root_sectors else ""
    print(f"wrote {out} ({disk_sectors * SECTOR} bytes, ESP {args.esp_mb} MiB{extra}, {len(files)} files)")


if __name__ == "__main__":
    main()
