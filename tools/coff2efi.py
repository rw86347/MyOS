#!/usr/bin/env python3
"""coff2efi.py — link one or more clang COFF objects into a PE32+ EFI app.

Apple clang can compile x86_64 Windows objects, but this tree has no lld.
One object file with no libc is enough for the loader; this script resolves
its relocations and writes BOOTX64.EFI (subsystem 10, with a .reloc table
so firmware can load it anywhere).
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

AMD64 = 0x8664
REL_ADDR64 = 1
REL_ADDR32 = 2
REL_ADDR32NB = 3
REL_REL32 = 4  # +0; 5..9 are REL32_1 .. REL32_5

# Storage classes we care about
CLASS_EXTERNAL = 2
CLASS_STATIC = 3

IMAGE_SCN_CNT_CODE = 0x00000020
IMAGE_SCN_CNT_INIT_DATA = 0x00000040
IMAGE_SCN_CNT_UNINIT_DATA = 0x00000080
IMAGE_SCN_MEM_DISCARDABLE = 0x02000000
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_READ = 0x40000000
IMAGE_SCN_MEM_WRITE = 0x80000000

KEEP = {b".text", b".data", b".rdata", b".bss"}
SKIP_PREFIX = (b".debug", b".drectve", b".llvm", b".pdata", b".xdata", b".sxdata")


def read_name(raw: bytes, strtab: bytes) -> bytes:
    if raw[:4] == b"\0\0\0\0":
        off = struct.unpack_from("<I", raw, 4)[0]
        return strtab[off:].split(b"\0", 1)[0]
    return raw.split(b"\0", 1)[0]


class Section:
    def __init__(self, name: bytes, data: bytes, chars: int, rels: list, bss: int = 0):
        self.name = name
        self.data = bytearray(data)
        self.chars = chars
        self.rels = rels  # (offset, symindex, type)
        self.bss = bss
        self.rva = 0
        self.size = max(len(data), bss)


class Object:
    def __init__(self, path: Path):
        blob = path.read_bytes()
        machine, nsect, _ts, ptrsym, nsym, optsz, _ch = struct.unpack_from("<HHIIHHH", blob)
        if machine != AMD64:
            raise SystemExit(f"{path}: not AMD64 COFF")
        strtab_off = ptrsym + nsym * 18
        strtab = blob[strtab_off:]
        self.symbols = []
        i = 0
        while i < nsym:
            rec = blob[ptrsym + i * 18 : ptrsym + i * 18 + 18]
            name = read_name(rec[:8], strtab)
            val, sec, ty, cls, aux = struct.unpack_from("<IHHBB", rec, 8)
            self.symbols.append((name, val, sec, cls, i))
            i += 1 + aux
        self.sections: list[Section] = []
        off = 20 + optsz
        for s in range(nsect):
            name, _vs, _va, rawsz, rawptr, relptr, _lp, nrel, _nl, chars = struct.unpack_from(
                "<8sIIIIIIHHI", blob, off + s * 40
            )
            name = read_name(name, strtab) if name.startswith(b"/") else name.split(b"\0", 1)[0]
            if any(name.startswith(p) for p in SKIP_PREFIX) or name not in KEEP:
                self.sections.append(Section(name, b"", chars, []))
                continue
            data = blob[rawptr : rawptr + rawsz] if rawsz else b""
            rels = []
            for r in range(nrel):
                ro, sym, rt = struct.unpack_from("<IIH", blob, relptr + r * 10)
                rels.append((ro, sym, rt))
            bss = 0
            if chars & IMAGE_SCN_CNT_UNINIT_DATA:
                # clang stores BSS size in SizeOfRawData or VirtualSize; raw is 0
                _name, vsize, _va, rawsz2, *_ = struct.unpack_from("<8sIIIIIIHHI", blob, off + s * 40)
                bss = vsize or rawsz2
            self.sections.append(Section(name, data, chars, rels, bss))


def align(n: int, a: int) -> int:
    return (n + a - 1) & ~(a - 1)


def link(objects: list[Object], entry: bytes, image_base: int) -> bytes:
    sections: list[Section] = []
    # Merge same-named sections in order, recording per-object section -> (sect, addend)
    loc: list[list[tuple[Section, int]]] = []
    by_name: dict[bytes, Section] = {}
    for obj in objects:
        places = []
        for s in obj.sections:
            if s.name not in KEEP:
                places.append((s, 0))
                continue
            merged = by_name.get(s.name)
            if merged is None:
                merged = Section(s.name, bytes(s.data), s.chars, [], s.bss)
                by_name[s.name] = merged
                sections.append(merged)
                places.append((merged, 0))
            else:
                add = align(merged.size, 16)
                merged.data.extend(b"\0" * (add - len(merged.data)))
                merged.data.extend(s.data)
                merged.bss = max(merged.bss, add + s.bss)
                merged.size = max(len(merged.data), merged.bss)
                places.append((merged, add))
            merged.rels.extend((off + places[-1][1], sym, rt) for off, sym, rt in s.rels)
            # rels above used the object's symbol index; fix after we know addend
            # (we stored with addend already)
        loc.append(places)

    order = [b".text", b".rdata", b".data", b".bss"]
    sections.sort(key=lambda s: order.index(s.name) if s.name in order else 99)

    rva = 0x1000
    for s in sections:
        s.rva = rva
        s.size = max(len(s.data), s.bss)
        rva = align(s.rva + s.size, 0x1000)

    # Global symbols: name -> (rva, defined)
    defined: dict[bytes, int] = {}
    for obj, places in zip(objects, loc):
        for name, val, sec, cls, _idx in obj.symbols:
            if sec == 0 or sec >= 0xFFFE:
                continue
            if cls not in (CLASS_EXTERNAL, CLASS_STATIC):
                continue
            if sec < 1 or sec > len(places):
                continue
            dest, add = places[sec - 1]
            if dest.name not in KEEP:
                continue
            defined[name] = dest.rva + add + val

    if entry not in defined:
        raise SystemExit(f"entry {entry!r} not found. have: {sorted(defined)}")

    # Apply relocations; collect base relocs (page -> list of (off, type))
    # PE base reloc types: DIR64=10, HIGHLOW=3
    base_relocs: dict[int, list[tuple[int, int]]] = {}

    def add_base(rva_: int, ty: int) -> None:
        page = rva_ & ~0xFFF
        base_relocs.setdefault(page, []).append((rva_ & 0xFFF, ty))

    for obj, places in zip(objects, loc):
        syms = obj.symbols
        # map symbol index -> (name, val, sec, cls)
        by_idx = {idx: (name, val, sec, cls) for name, val, sec, cls, idx in syms}
        for dest, _add in places:
            if dest.name not in KEEP:
                continue
        # Re-walk this object's original sections for correct dest/addend
        for s_i, s in enumerate(obj.sections):
            dest, add = places[s_i]
            if dest.name not in KEEP:
                continue
            for off, symi, rt in s.rels:
                dest_off = add + off
                if dest_off + 8 > len(dest.data) + 8:
                    dest.data.extend(b"\0" * (dest_off + 8 - len(dest.data)))
                name, val, sec, cls = by_idx[symi]
                if sec == 0:
                    if name not in defined:
                        raise SystemExit(f"undefined symbol {name!r}")
                    target = defined[name]
                elif sec >= 0xFFFE:
                    target = val
                else:
                    d2, a2 = places[sec - 1]
                    target = d2.rva + a2 + val
                site = dest.rva + dest_off
                if rt == REL_ADDR64:
                    old = struct.unpack_from("<Q", dest.data, dest_off)[0]
                    struct.pack_into("<Q", dest.data, dest_off, old + image_base + target)
                    add_base(site, 10)
                elif rt == REL_ADDR32:
                    old = struct.unpack_from("<I", dest.data, dest_off)[0]
                    struct.pack_into("<I", dest.data, dest_off, (old + image_base + target) & 0xFFFFFFFF)
                    add_base(site, 3)
                elif rt == REL_ADDR32NB:
                    old = struct.unpack_from("<I", dest.data, dest_off)[0]
                    struct.pack_into("<I", dest.data, dest_off, (old + target) & 0xFFFFFFFF)
                elif REL_REL32 <= rt <= REL_REL32 + 5:
                    extra = rt - REL_REL32
                    old = struct.unpack_from("<I", dest.data, dest_off)[0]
                    # rip = site + 4 + extra
                    rel = (old + target) - (site + 4 + extra)
                    struct.pack_into("<I", dest.data, dest_off, rel & 0xFFFFFFFF)
                else:
                    raise SystemExit(f"unsupported reloc type {rt} for {name!r}")

    # .reloc section. Firmware that cannot load at ImageBase needs this;
    # an empty directory is why some PCs skip BOOTX64.EFI entirely.
    if not base_relocs:
        base_relocs[0x1000] = [(0, 0)]
    reloc_blob = bytearray()
    for page in sorted(base_relocs):
        entries = base_relocs[page]
        if len(entries) & 1:
            entries = entries + [(0, 0)]  # pad to 4 bytes
        block = struct.pack("<II", page, 8 + 2 * len(entries))
        for off, ty in entries:
            block += struct.pack("<H", (ty << 12) | off)
        reloc_blob.extend(block)

    reloc = Section(b".reloc", bytes(reloc_blob),
                    IMAGE_SCN_CNT_INIT_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_DISCARDABLE, [])
    reloc.rva = rva
    reloc.size = len(reloc_blob)
    if reloc.size:
        sections.append(reloc)
        rva = align(reloc.rva + reloc.size, 0x1000)

    file_align = 0x200
    sect_align = 0x1000
    # Headers: DOS + PE + COFF + opt + 16 dirs + section headers
    nsect = len(sections)
    opt_size = 112 + 16 * 8  # PE32+ optional + 16 data directories? 112 includes first 16 dirs?
    # PE32+ optional header is 112 bytes including NumberOfRvaAndSizes and 16 dirs (16*8=128)?
    # Standard: 112 bytes up to NumberOfRvaAndSizes (4), then 16*8 = 128, total 240.
    opt_size = 240
    headers = 64 + 4 + 20 + opt_size + nsect * 40  # dos(64)+PE\0\0(4)+coff(20)+opt+sects
    headers = align(headers, file_align)
    size_of_headers = headers

    file_off = size_of_headers
    file_offs = []
    for s in sections:
        file_offs.append(file_off)
        raw = 0 if s.name == b".bss" else align(max(len(s.data), 1) if s.size else 0, file_align)
        if s.name == b".bss":
            raw = 0
        else:
            raw = align(s.size, file_align) if s.size else 0
        file_off = file_off + raw

    size_of_image = align(rva, sect_align)
    entry_rva = defined[entry]

    # Data directories: 5 = basereloc
    dirs = [(0, 0)] * 16
    if reloc.size:
        dirs[5] = (reloc.rva, reloc.size)

    dos = bytearray(64)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 64)  # e_lfanew

    pe = b"PE\0\0"
    coff = struct.pack("<HHIIIHH",
                       AMD64, nsect, 0, 0, 0, opt_size,
                       0x0002 | 0x0020)  # EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE

    opt = b"".join([
        struct.pack("<HBB", 0x20B, 14, 0),
        struct.pack("<III", 0, 0, 0),           # SizeOfCode, SizeOfInit, SizeOfUninit
        struct.pack("<I", entry_rva),
        struct.pack("<I", 0x1000),              # BaseOfCode
        struct.pack("<Q", image_base),
        struct.pack("<II", sect_align, file_align),
        struct.pack("<HH", 0, 0),               # OS
        struct.pack("<HH", 0, 0),               # image
        struct.pack("<HH", 0, 0),               # subsystem ver
        struct.pack("<I", 0),
        struct.pack("<II", size_of_image, size_of_headers),
        struct.pack("<I", 0),
        struct.pack("<HH", 10, 0x0160),  # EFI app; DYNAMIC_BASE | NX | high-entropy
        struct.pack("<QQ", 0x10000, 0x1000),
        struct.pack("<QQ", 0x10000, 0x1000),
        struct.pack("<II", 0, 16),
    ])
    assert len(opt) == 112, len(opt)
    for rva_, sz in dirs:
        opt += struct.pack("<II", rva_, sz)
    assert len(opt) == 240, len(opt)

    shdrs = b""
    raw_chunks = []
    for s, foff in zip(sections, file_offs):
        vsize = s.size
        rawsz = 0 if s.name == b".bss" else align(s.size, file_align) if s.size else 0
        name = (s.name + b"\0" * 8)[:8]
        chars = s.chars
        if s.name == b".text":
            chars = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ
        elif s.name == b".bss":
            chars = IMAGE_SCN_CNT_UNINIT_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE
        elif s.name == b".reloc":
            chars = IMAGE_SCN_CNT_INIT_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_DISCARDABLE
        elif s.name == b".rdata":
            chars = IMAGE_SCN_CNT_INIT_DATA | IMAGE_SCN_MEM_READ
        else:
            chars = IMAGE_SCN_CNT_INIT_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE
        shdrs += struct.pack("<8sIIIIIIHHI", name, vsize, s.rva, rawsz, foff if rawsz else 0,
                             0, 0, 0, 0, chars)
        if rawsz:
            data = bytes(s.data)[:s.size].ljust(rawsz, b"\0")
            raw_chunks.append((foff, data))

    out = bytearray(file_off)
    out[0:64] = dos
    out[64:68] = pe
    out[68:88] = coff
    out[88:88 + 240] = opt
    out[88 + 240:88 + 240 + len(shdrs)] = shdrs
    for foff, data in raw_chunks:
        out[foff:foff + len(data)] = data
    return bytes(out)


def main() -> None:
    if len(sys.argv) < 4:
        sys.exit(f"usage: {sys.argv[0]} -o BOOTX64.EFI [-e efi_main] a.obj [b.obj ...]")
    args = sys.argv[1:]
    out = None
    entry = b"efi_main"
    objs = []
    i = 0
    while i < len(args):
        if args[i] == "-o":
            out = Path(args[i + 1]); i += 2
        elif args[i] == "-e":
            entry = args[i + 1].encode(); i += 2
        else:
            objs.append(Path(args[i])); i += 1
    if not out or not objs:
        sys.exit("need -o and at least one object")
    # Below 4 GiB: some firmware identity-maps only that and will not
    # place an image at the Windows default 0x140000000.
    image_base = 0x10000000
    pe = link([Object(p) for p in objs], entry, image_base)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(pe)
    print(f"wrote {out} ({len(pe)} bytes)")


if __name__ == "__main__":
    main()
