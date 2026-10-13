#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""kconfig.py — the kernel config: its layout on disk, its safety checks, and
how it is written little- or big-endian. Documentation you can run
(docs/KERNEL-CONFIG.md); later, the build uses it to make the kernel's
built-in default, and the tests use it to check the kernel's reader.

    python3 tools/kconfig/kconfig.py demo                  the example, start to end
    python3 tools/kconfig/kconfig.py check CONF [--cpu F] [--root DIR]
                                                           the safety checks Save runs
    python3 tools/kconfig/kconfig.py partition CONF [--arch A] [--big] -o IMAGE
                                                           a myos-config partition image
    python3 tools/kconfig/kconfig.py decode IMAGE          what a partition holds
    python3 tools/kconfig/kconfig.py header                the C structs for the kernel
    python3 tools/kconfig/kconfig.py cdefault SLOTS --arch A -o FILE.c
                                                           the kernel's built-in default
                                                           (from config/slots.conf), as C
    python3 tools/kconfig/kconfig.py layout                the field tables

CONF is the text form (docs/kernel-config/example.conf).

The config has two parts, saved together and never mixed up:

    the boot block   512 bytes, read by the kernel at boot, before any file
                     system: the mode, which task owns which slot, the
                     latency budget, the power savings.
    the task table   5 KiB, for after boot: for each task, the program to run
                     (/bin/brakes), its arguments, the user it runs as,
                     whether it starts at boot and is restarted when it
                     stops. The kernel keeps it and lets only that program
                     take the task's slots; myos-taskd starts the programs.

Both carry the same generation number. Where they sit in the myos-config
partition (in 512-byte sectors; two copies, A and B):

    sector  0        boot block, copy A
    sector  1        boot block, copy B
    sectors 16-25    task table, copy A
    sectors 32-41    task table, copy B

Save writes the older copy: its task table first, then its boot block, each
flushed to the drive. A copy counts only if both parts are whole and their
generations match, so a power cut in between leaves the other copy in use.

The boot block (every field at a fixed offset, naturally aligned, so the
kernel can use it as a plain C struct once the byte order matches):

    0x000  header      magic "MYOSKCFG", byte order, version, arch, mode,
                       size, slot length, generation, CRC-32
    0x018  slots       20 owners (0 = free, 1..18 = a task, 0xFF = the OS),
                       the OS slots (0 and 10), real-time flags, latency budget
    0x040  task names  18 x 16 bytes
    0x160  power       features on, features the machine had, speed limits,
                       energy preference, deepest idle, disk spin-down
    0x170  reserved    zeros, for later versions

The task table: a 64-byte header (magic "MYOSTASK", the boot block's byte
order and generation, its own CRC-32), then one 256-byte entry per task, in
the boot block's task order:

    0x00  name       16   the same name as the boot block's
    0x10  program   128   absolute path, NUL-terminated
    0x90  args       96   arguments, each NUL-terminated, then an empty one
    0xF0  uid, gid    8   who it runs as
    0xF8  flags       2   start at boot, restart when it stops
    0xFA  restart_ms  2   how long to wait before a restart
"""
import os
import re
import stat
import struct
import sys
import zlib
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(__file__))
import powerfeatures as pf  # noqa: E402  (next to this file)

BLOCK = 512
MAGIC = b"MYOSKCFG"
VERSION = 1
NSLOTS = 20                 # the major frame: 20 slots ...
SLOT_US = 1000              # ... of 1 ms: it repeats every 20 ms
OS_SLOTS = (0, 10)          # always the system's: every 10 ms exactly
MAX_TASKS = NSLOTS - len(OS_SLOTS)
NAME_MAX = 15               # + the NUL: 16 bytes, as Linux's thread names
OWNER_FREE, OWNER_OS = 0, 0xFF

MODES = {"realtime": 0, "soft-realtime": 1, "power-saving": 2}
MODE_NAMES = {v: k for k, v in MODES.items()}

# Targets: the arch code and the byte order its kernel reads natively.
# x86 is always little-endian; ARM can be either (the Pi runs little).
ARCHES = {"i386": (1, "little"), "x86_64": (2, "little"),
          "arm": (3, "little"), "arm64": (4, "little")}
ARCH_NAMES = {v[0]: k for k, v in ARCHES.items()}
ORDER_CODE = {"little": 1, "big": 2}
ORDER_NAME = {1: "little", 2: "big"}
FAMILY_OF = {"i386": "pentium-m", "x86_64": "zen"}  # the default family to check against

# Real-time flags (rt_flags).
RT_LEFTOVER = 1 << 0        # free slots and unused owned slots run ordinary programs
RT_DEFER_IRQ = 1 << 1       # realtime: device work after the interrupt waits for an OS slot

# The boot block: (offset, name, struct code, C type, comment). The offsets
# are checked below: no gaps, no overlaps, 512 bytes in all.
LAYOUT = [
    (0x000, "magic", "8s", "char", "\"MYOSKCFG\""),
    (0x008, "byte_order", "B", "uint8_t", "1 = little-endian, 2 = big-endian"),
    (0x009, "version", "B", "uint8_t", "1"),
    (0x00A, "arch", "B", "uint8_t", "1 i386, 2 x86_64, 3 arm, 4 arm64"),
    (0x00B, "mode", "B", "uint8_t", "0 realtime, 1 soft realtime, 2 power saving"),
    (0x00C, "size", "H", "uint16_t", "512"),
    (0x00E, "slot_us", "H", "uint16_t", "length of one slot: 1000 us"),
    (0x010, "generation", "I", "uint32_t", "+1 on every save; the task table has the same"),
    (0x014, "crc32", "I", "uint32_t", "CRC-32 (IEEE) of the block with this field 0"),
    (0x018, "nslots", "B", "uint8_t", "20"),
    (0x019, "ntasks", "B", "uint8_t", "names used in task_name[]"),
    (0x01A, "os_slot", "2s", "uint8_t", "{0, 10}: the system's slots"),
    (0x01C, "rt_flags", "I", "uint32_t", "KCFG_RT_LEFTOVER | KCFG_RT_DEFER_IRQ"),
    (0x020, "slot_owner", "20s", "uint8_t", "0 free, 1..18 task_name[n-1], 0xFF the OS"),
    (0x034, "latency_budget_us", "H", "uint16_t", "soft realtime: slowest wake-up allowed"),
    (0x036, "reserved0", "10s", "uint8_t", "0"),
    (0x040, "task_name", "288s", "char", "18 names of 16 bytes, NUL-padded"),
    (0x160, "power_on", "I", "uint32_t", "power saving: the features switched on"),
    (0x164, "power_seen", "I", "uint32_t", "the features this machine had at the save"),
    (0x168, "cpu_min_pct", "B", "uint8_t", "slowest speed, % of the highest non-boost"),
    (0x169, "cpu_max_pct", "B", "uint8_t", "fastest speed, same scale"),
    (0x16A, "epp", "B", "uint8_t", "energy preference: 0 speed .. 255 energy"),
    (0x16B, "cstate_max", "B", "uint8_t", "deepest idle state: 1 = C1 ... 6 = C6"),
    (0x16C, "spindown_min", "H", "uint16_t", "idle minutes before a disk stops (0 never)"),
    (0x16E, "reserved1", "2s", "uint8_t", "0"),
    (0x170, "reserved2", "144s", "uint8_t", "0: room for later versions"),
]

# The task table: its header, then MAX_TASKS entries.
TASK_MAGIC = b"MYOSTASK"
TASK_TABLE = 5120           # 10 sectors: the header, 18 entries, room to spare
TASK_ENTRY = 256
TASKS_AT = 0x40             # where the first entry starts
PROGRAM_MAX = 127           # + the NUL
ARGS_MAX = 96               # all arguments with their NULs, and the final empty one
TASK_START_AT_BOOT = 1 << 0  # myos-taskd starts it when the system comes up
TASK_RESTART = 1 << 1        # ... and again whenever it stops
TASK_HEADER = [
    (0x00, "magic", "8s", "char", "\"MYOSTASK\""),
    (0x08, "byte_order", "B", "uint8_t", "the boot block's"),
    (0x09, "version", "B", "uint8_t", "1"),
    (0x0A, "ntasks", "B", "uint8_t", "the boot block's"),
    (0x0B, "reserved0", "B", "uint8_t", "0"),
    (0x0C, "size", "I", "uint32_t", "5120"),
    (0x10, "generation", "I", "uint32_t", "the boot block's"),
    (0x14, "crc32", "I", "uint32_t", "CRC-32 of the 5120 bytes with this field 0"),
    (0x18, "entry_size", "H", "uint16_t", "256"),
    (0x1A, "reserved1", "38s", "uint8_t", "0"),
]
TASK_FIELDS = [
    (0x00, "name", "16s", "char", "the task, as in the boot block"),
    (0x10, "program", "128s", "char", "absolute path, NUL-terminated"),
    (0x90, "args", "96s", "char", "each NUL-terminated, then an empty one"),
    (0xF0, "uid", "I", "uint32_t", "runs as this user ..."),
    (0xF4, "gid", "I", "uint32_t", "... and group"),
    (0xF8, "flags", "H", "uint16_t", "KCFG_TASK_START_AT_BOOT | KCFG_TASK_RESTART"),
    (0xFA, "restart_ms", "H", "uint16_t", "wait before a restart"),
    (0xFC, "reserved", "4s", "uint8_t", "0"),
]

# Where the parts sit in the myos-config partition (512-byte sectors).
SECTOR = 512
PLACES = {"A": (0, 16), "B": (1, 32)}       # copy: (boot block, task table)
PARTITION_USED = 42 * SECTOR                # the rest of the 1 MiB is room for later


def _check_layout(layout, total, aligned=True):
    at = 0
    for off, name, code, _, _ in layout:
        assert off == at, f"{name} at {off:#x}, expected {at:#x}"
        size = struct.calcsize("<" + code)
        if aligned and code[-1] in "HI":
            assert off % size == 0, f"{name} isn't aligned"
        at += size
    assert at == total, f"the layout is {at} bytes, not {total}"


_check_layout(LAYOUT, BLOCK)
_check_layout(TASK_HEADER, TASKS_AT)
_check_layout(TASK_FIELDS, TASK_ENTRY)
assert TASKS_AT + MAX_TASKS * TASK_ENTRY <= TASK_TABLE
FIELD_OFFSET = {name: off for off, name, *_ in LAYOUT}


@dataclass
class Task:
    """The run part of one task: what myos-taskd starts for it after boot."""
    program: str = ""
    args: list = field(default_factory=list)
    uid: int = 65534                        # nobody, unless chosen
    gid: int = 65534
    start_at_boot: bool = True
    restart: bool = True
    restart_ms: int = 1000


@dataclass
class KConfig:
    """One kernel config, as Settings holds it before Save."""
    mode: int = MODES["soft-realtime"]
    arch: str = "x86_64"
    byte_order: str = ""                    # "" = the arch's own
    slots: list = field(default_factory=lambda: ["os" if s in OS_SLOTS else "" for s in range(NSLOTS)])
    rt_flags: int = RT_LEFTOVER | RT_DEFER_IRQ
    latency_budget_us: int = 100
    power_on: set = field(default_factory=set)
    power_seen: set = field(default_factory=set)
    cpu_min_pct: int = 30
    cpu_max_pct: int = 100
    epp: int = 128
    cstate_max: int = 3
    spindown_min: int = 10
    generation: int = 0
    programs: dict = field(default_factory=dict)    # task name -> Task: the run part

    def order(self):
        return self.byte_order or ARCHES[self.arch][1]

    def tasks(self):
        """The task names in first-slot order: task 1 is the earliest."""
        out = []
        for name in self.slots:
            if name and name != "os" and name not in out:
                out.append(name)
        return out


# --- the text form -----------------------------------------------------------

def parse_text(text):
    """example.conf -> KConfig. `key = value` lines; [slots], [power] and one
    [task NAME] section per task."""
    cfg, section = KConfig(), ""
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            if section.startswith("task "):
                cfg.programs.setdefault(section[5:].strip(), Task())
            continue
        if "=" not in line:
            raise ValueError(f"line {n}: expected key = value")
        key, value = (x.strip() for x in line.split("=", 1))
        if section.startswith("task "):
            t = cfg.programs[section[5:].strip()]
            if key == "program":
                t.program = value
            elif key == "args":
                t.args = value.split()
            elif key in ("uid", "gid", "restart_ms"):
                setattr(t, key, int(value, 0))
            elif key == "start":
                t.start_at_boot = value == "boot"
            elif key == "restart":
                t.restart = value == "yes"
            else:
                raise ValueError(f"line {n}: unknown task setting '{key}'")
        elif section == "slots":
            if not key.isdigit() or int(key) >= NSLOTS:
                raise ValueError(f"line {n}: '{key}' is not a slot (0-{NSLOTS - 1})")
            cfg.slots[int(key)] = value
        elif section == "power" and key in ("on", "seen"):
            setattr(cfg, "power_" + key, set(value.split()))
        elif key == "mode":
            if value not in MODES:
                raise ValueError(f"line {n}: mode is one of {', '.join(MODES)}")
            cfg.mode = MODES[value]
        elif key in ("arch", "byte_order"):
            setattr(cfg, key, value)
        elif hasattr(cfg, key) and isinstance(getattr(cfg, key), int):
            setattr(cfg, key, int(value, 0))
        else:
            raise ValueError(f"line {n}: unknown setting '{key}'")
    return cfg


# --- the safety checks Save runs (and the kernel runs again) -----------------

NAME_RE = re.compile(r"^[a-z][a-z0-9_-]{0,%d}$" % (NAME_MAX - 1))
RESERVED_NAMES = {"os", "idle", "main", "kernel"}


def cadence(slots):
    """'every 5 ms', or the gaps when they differ. The frame wraps."""
    if len(slots) == 1:
        return "every 20 ms"
    gaps = [(slots[(i + 1) % len(slots)] - s) % NSLOTS * SLOT_US // 1000 for i, s in enumerate(slots)]
    if len(set(gaps)) == 1:
        return f"every {gaps[0]} ms"
    return "uneven: gaps of " + "/".join(str(g) for g in gaps) + " ms"


def check_program(task, root=None):
    """The program's path, and with root (the system's "/"), the file itself:
    it must exist, be a program, and be writable only by root (nor may its
    folders be), or whoever could change it would own a real-time slot."""
    p = task.program
    if not p.startswith("/") or "/../" in p + "/" or "//" in p:
        return [f"'{p}' must be an absolute path without .."]
    if len(p.encode()) > PROGRAM_MAX or not p.isprintable():
        return [f"the program path is longer than {PROGRAM_MAX} bytes or has odd characters"]
    if root is None:
        return []
    full = os.path.join(root, p.lstrip("/"))
    try:
        st = os.stat(full)
    except OSError:
        return [f"{p} isn't there"]
    out = []
    if not stat.S_ISREG(st.st_mode) or not st.st_mode & 0o111:
        out.append(f"{p} isn't a program (a file with x permission)")
    if st.st_mode & 0o022 or (task.uid == 0 and st.st_uid != 0):
        out.append(f"{p} can be changed by others: only root may write it")
    d = os.path.dirname(full)
    while len(d) > len(root.rstrip("/")):
        if os.stat(d).st_mode & 0o002:
            out.append(f"{os.path.dirname(p)} (or a folder above) can be written by anyone")
            break
        d = os.path.dirname(d)
    return out


def check(cfg, family=None, root=None, boot_only=False):
    """Errors stop Save; warnings ask "save anyway?"; notes are information.
    root: the system's "/" to look at the programs in (Settings uses "/").
    boot_only: just the boot block (what the kernel checks before any file
    system, and before it has the task table)."""
    errors, warnings, notes = [], [], []
    family = family or FAMILY_OF.get(cfg.arch, "zen")
    if cfg.arch not in ARCHES:
        errors.append(f"unknown arch '{cfg.arch}' (one of {', '.join(ARCHES)})")
    if cfg.order() not in ORDER_CODE:
        errors.append("byte order is little or big")
    if cfg.mode not in MODE_NAMES:
        errors.append("mode is realtime, soft realtime or power saving")
    if len(cfg.slots) != NSLOTS:
        errors.append(f"there are {NSLOTS} slots")
        return errors, warnings, notes

    # The OS slots, and nobody else's name in them.
    for s, name in enumerate(cfg.slots):
        if s in OS_SLOTS and name != "os":
            errors.append(f"slot {s} belongs to the OS")
        elif s not in OS_SLOTS and name == "os":
            errors.append(f"slot {s}: only slots 0 and 10 are the OS")
        elif name and name != "os":
            if name in RESERVED_NAMES:
                errors.append(f"slot {s}: '{name}' is a name the kernel uses itself")
            elif not NAME_RE.match(name):
                errors.append(f"slot {s}: '{name}' must start with a small letter and use only "
                              f"a-z, 0-9, - and _ (at most {NAME_MAX})")

    tasks = cfg.tasks()
    free = [s for s, name in enumerate(cfg.slots) if not name]
    if cfg.mode != MODES["power-saving"]:
        for t in tasks:
            mine = [s for s, n in enumerate(cfg.slots) if n == t]
            notes.append(f"{t}: slot{'s' if len(mine) > 1 else ''} {', '.join(map(str, mine))} - {cadence(mine)}")
        if len(free) < 2:
            warnings.append(f"only {len(free)} free slot{'s' if len(free) != 1 else ''}: ordinary programs "
                            "(the desktop too) get little more than what the named tasks leave over")
        if not cfg.rt_flags & RT_LEFTOVER and len(free) == 0:
            errors.append("no free slots and leftover time is off: nothing but the named tasks could run")
    if cfg.mode == MODES["soft-realtime"]:
        if not 1 <= cfg.latency_budget_us <= 1000:
            errors.append("the latency budget is 1 to 1000 us")
        else:
            c = pf.soft_realtime_cstate(family, cfg.latency_budget_us)
            notes.append(f"soft realtime may idle down to {c[1]} (wakes in about {c[2]} us)")

    # The run part: every task has a program, and one that can't be swapped
    # behind the administrator's back.
    for t in ([] if boot_only else tasks):
        task = cfg.programs.get(t)
        if not task or not task.program:
            errors.append(f"{t}: which program runs in its slots? (e.g. /bin/{t})")
            continue
        errors += [f"{t}: {m}" for m in check_program(task, root)]
        if len(b"".join(a.encode() + b"\0" for a in task.args) + b"\0") > ARGS_MAX:
            errors.append(f"{t}: the arguments are longer than {ARGS_MAX - 1} bytes")
        if any(not a.isprintable() for a in task.args):
            errors.append(f"{t}: an argument has a character it can't have")
        if task.uid == 0:
            warnings.append(f"{t}: runs as root, with full control of the computer")
        if not task.start_at_boot:
            warnings.append(f"{t}: doesn't start at boot; its slots stay empty until something starts it")
        if not 0 <= task.restart_ms <= 60000:
            errors.append(f"{t}: restart after 0 to 60000 ms")
    for name in ([] if boot_only else cfg.programs):
        if name not in tasks:
            notes.append(f"{name}: has a program but no slots; it isn't saved")
    if cfg.mode == MODES["power-saving"] and tasks and not boot_only:
        notes.append("the tasks still start, as ordinary programs: there are no slots in this mode")

    # Power features: only real ones, and only what this processor can have.
    for key in sorted(cfg.power_on | cfg.power_seen):
        if key not in pf.BY_KEY:
            errors.append(f"'{key}' is not a power feature")
    for key in sorted(cfg.power_on):
        f = pf.BY_KEY.get(key)
        if f and f.support.get(family, "no") == "no":
            errors.append(f"{f.label}: a {pf.FAMILY_NAMES[family]} doesn't have it")
        elif f and f.support.get(family) == "smp":
            warnings.append(f"{f.label}: does nothing until MyOS runs on more than one core")
    if cfg.mode == MODES["power-saving"]:
        if not cfg.power_on:
            warnings.append("power saving with every feature off saves nothing: soft realtime does more")
        if not 10 <= cfg.cpu_min_pct <= cfg.cpu_max_pct <= 100:
            errors.append("processor speed: 10% <= slowest <= fastest <= 100%")
        if not 0 <= cfg.epp <= 255:
            errors.append("the energy preference is 0 to 255")
        if cfg.cstate_max not in [c[0] for c in pf.CSTATES if family in c[3]]:
            errors.append(f"C{cfg.cstate_max} isn't an idle state of a {pf.FAMILY_NAMES[family]}")
        if "disk_spindown" in cfg.power_on:
            if not 1 <= cfg.spindown_min <= 600:
                errors.append("spin down after 1 to 600 minutes")
            elif cfg.spindown_min < 10:
                warnings.append("a disk stopped and started every few minutes wears out sooner (10 min or more)")
    else:
        notes.append("power features are kept but not used: this mode turns power management off"
                     if cfg.mode == MODES["realtime"] else
                     "power features are kept but not used, except the idle states above")
    return errors, warnings, notes


# --- the boot block ----------------------------------------------------------

def _crc(data, at):
    """CRC-32 (IEEE 802.3, as zlib) of data with its crc32 field (at) zero."""
    return zlib.crc32(data[:at] + b"\0\0\0\0" + data[at + 4:]) & 0xFFFFFFFF


def crc32(block):
    return _crc(bytes(block), FIELD_OFFSET["crc32"])


def _bits(keys):
    return sum(1 << pf.BY_KEY[k].bit for k in keys)


def _keys(bits):
    return {f.key for f in pf.FEATURES if bits >> f.bit & 1}


def encode(cfg, generation=None):
    """KConfig -> the 512-byte boot block, in the target's byte order.
    Refuses a config that fails the checks: a bad block is never written."""
    errors, _, _ = check(cfg, boot_only=True)
    if errors:
        raise ValueError("; ".join(errors))
    tasks = cfg.tasks()
    owner = bytes(OWNER_OS if n == "os" else (tasks.index(n) + 1 if n else OWNER_FREE) for n in cfg.slots)
    names = b"".join(t.encode("ascii").ljust(16, b"\0") for t in tasks).ljust(MAX_TASKS * 16, b"\0")
    values = {
        "magic": MAGIC, "byte_order": ORDER_CODE[cfg.order()], "version": VERSION,
        "arch": ARCHES[cfg.arch][0], "mode": cfg.mode, "size": BLOCK, "slot_us": SLOT_US,
        "generation": cfg.generation if generation is None else generation, "crc32": 0,
        "nslots": NSLOTS, "ntasks": len(tasks), "os_slot": bytes(OS_SLOTS), "rt_flags": cfg.rt_flags,
        "slot_owner": owner, "latency_budget_us": cfg.latency_budget_us, "reserved0": b"",
        "task_name": names, "power_on": _bits(cfg.power_on), "power_seen": _bits(cfg.power_seen),
        "cpu_min_pct": cfg.cpu_min_pct, "cpu_max_pct": cfg.cpu_max_pct, "epp": cfg.epp,
        "cstate_max": cfg.cstate_max, "spindown_min": cfg.spindown_min,
        "reserved1": b"", "reserved2": b"",
    }
    e = "<" if cfg.order() == "little" else ">"     # the one place byte order matters
    block = bytearray(BLOCK)
    for off, name, code, _, _ in LAYOUT:
        struct.pack_into(e + code, block, off, values[name])
    struct.pack_into(e + "I", block, FIELD_OFFSET["crc32"], crc32(block))
    return bytes(block)


def decode(block, expect_arch=None):
    """512 bytes -> KConfig (without programs), with the checks the kernel
    makes at boot. Raises ValueError saying why a block can't be used."""
    if len(block) != BLOCK:
        raise ValueError(f"a block is {BLOCK} bytes, not {len(block)}")
    if block[:8] != MAGIC:
        raise ValueError("no MYOSKCFG magic: not a config block (or never written)")
    order = ORDER_NAME.get(block[8])
    if not order:
        raise ValueError(f"byte order {block[8]} is neither 1 (little) nor 2 (big)")
    e = "<" if order == "little" else ">"
    v = {name: struct.unpack_from(e + code, block, off)[0] for off, name, code, _, _ in LAYOUT}
    if v["version"] != VERSION or v["size"] != BLOCK:
        raise ValueError(f"version {v['version']}, size {v['size']}: not one this reader knows")
    if v["crc32"] != crc32(block):
        raise ValueError("the CRC doesn't match: the block was damaged (or half written)")
    arch = ARCH_NAMES.get(v["arch"])
    if expect_arch and arch != expect_arch:
        raise ValueError(f"written for {arch}, not {expect_arch}")
    if v["nslots"] != NSLOTS or v["slot_us"] != SLOT_US or tuple(v["os_slot"]) != OS_SLOTS:
        raise ValueError("a slot layout this kernel doesn't have")
    if v["ntasks"] > MAX_TASKS:
        raise ValueError(f"{v['ntasks']} tasks: at most {MAX_TASKS}")
    names = [v["task_name"][i * 16:(i + 1) * 16].split(b"\0", 1)[0].decode("ascii", "replace")
             for i in range(v["ntasks"])]
    slots = []
    for s, o in enumerate(v["slot_owner"]):
        if o == OWNER_OS:
            slots.append("os")
        elif o == OWNER_FREE:
            slots.append("")
        elif o <= v["ntasks"]:
            slots.append(names[o - 1])
        else:
            raise ValueError(f"slot {s} names task {o}, but there are {v['ntasks']}")
    cfg = KConfig(mode=v["mode"], arch=arch, byte_order=order, slots=slots, rt_flags=v["rt_flags"],
                  latency_budget_us=v["latency_budget_us"], power_on=_keys(v["power_on"]),
                  power_seen=_keys(v["power_seen"]), cpu_min_pct=v["cpu_min_pct"],
                  cpu_max_pct=v["cpu_max_pct"], epp=v["epp"], cstate_max=v["cstate_max"],
                  spindown_min=v["spindown_min"], generation=v["generation"])
    errors, _, _ = check(cfg, boot_only=True)
    if errors:
        raise ValueError("; ".join(errors))
    if cfg.tasks() != names:
        raise ValueError("a task name no slot uses")
    return cfg


# --- the task table ----------------------------------------------------------

def encode_tasks(cfg, generation):
    """The run part -> 5120 bytes, in the boot block's byte order and order
    of tasks, with the boot block's generation."""
    e = "<" if cfg.order() == "little" else ">"
    tasks = cfg.tasks()
    table = bytearray(TASK_TABLE)
    head = {"magic": TASK_MAGIC, "byte_order": ORDER_CODE[cfg.order()], "version": VERSION,
            "ntasks": len(tasks), "reserved0": 0, "size": TASK_TABLE, "generation": generation,
            "crc32": 0, "entry_size": TASK_ENTRY, "reserved1": b""}
    for off, name, code, _, _ in TASK_HEADER:
        struct.pack_into(e + code, table, off, head[name])
    for i, t in enumerate(tasks):
        task = cfg.programs[t]
        v = {"name": t.encode(), "program": task.program.encode(),
             "args": b"".join(a.encode() + b"\0" for a in task.args) + b"\0",
             "uid": task.uid, "gid": task.gid,
             "flags": (TASK_START_AT_BOOT if task.start_at_boot else 0) | (TASK_RESTART if task.restart else 0),
             "restart_ms": task.restart_ms, "reserved": b""}
        for off, name, code, _, _ in TASK_FIELDS:
            struct.pack_into(e + code, table, TASKS_AT + i * TASK_ENTRY + off, v[name])
    struct.pack_into(e + "I", table, 0x14, _crc(bytes(table), 0x14))
    return bytes(table)


def decode_tasks(table, boot):
    """5120 bytes + the decoded boot block -> {name: Task}, or ValueError.
    It must belong to that boot block: same byte order, generation, names."""
    if len(table) != TASK_TABLE or table[:8] != TASK_MAGIC:
        raise ValueError("no MYOSTASK table")
    order = ORDER_NAME.get(table[8])
    if order != boot.order():
        raise ValueError("the task table's byte order isn't the boot block's")
    e = "<" if order == "little" else ">"
    h = {name: struct.unpack_from(e + code, table, off)[0] for off, name, code, _, _ in TASK_HEADER}
    if h["crc32"] != _crc(bytes(table), 0x14):
        raise ValueError("the task table's CRC doesn't match (damaged or half written)")
    if h["generation"] != boot.generation:
        raise ValueError(f"task table generation {h['generation']}, boot block {boot.generation}: "
                         "a save was cut off")
    names = boot.tasks()
    if h["ntasks"] != len(names) or h["entry_size"] != TASK_ENTRY or h["size"] != TASK_TABLE:
        raise ValueError("the task table doesn't list the boot block's tasks")
    out = {}
    for i, t in enumerate(names):
        v = {name: struct.unpack_from(e + code, table, TASKS_AT + i * TASK_ENTRY + off)[0]
             for off, name, code, _, _ in TASK_FIELDS}
        if v["name"].split(b"\0", 1)[0].decode("ascii", "replace") != t:
            raise ValueError(f"entry {i} isn't '{t}', as the boot block says")
        args = v["args"].split(b"\0")
        args = args[:args.index(b"")] if b"" in args else args
        out[t] = Task(program=v["program"].split(b"\0", 1)[0].decode("ascii", "replace"),
                      args=[a.decode("ascii", "replace") for a in args], uid=v["uid"], gid=v["gid"],
                      start_at_boot=bool(v["flags"] & TASK_START_AT_BOOT),
                      restart=bool(v["flags"] & TASK_RESTART), restart_ms=v["restart_ms"])
        if check_program(out[t]):
            raise ValueError(f"{t}: {check_program(out[t])[0]}")
    return out


# --- both parts, on the partition ---------------------------------------------

def save(cfg, image, copy, generation, root=None):
    """Save, as Settings (through the kernel) does it: the full checks, then
    the task table, then the boot block, both into `copy` of the image."""
    errors, _, _ = check(cfg, root=root)
    if errors:
        raise ValueError("; ".join(errors))
    image = bytearray(image)
    boot_at, tasks_at = PLACES[copy]
    image[tasks_at * SECTOR:tasks_at * SECTOR + TASK_TABLE] = encode_tasks(cfg, generation)
    # (flush the drive here: the table is on the disk before the block that points to it)
    image[boot_at * SECTOR:(boot_at + 1) * SECTOR] = encode(cfg, generation)
    return bytes(image)


def newer(a, b):
    """Is generation a newer than b? Serial-number arithmetic, so the count
    may wrap past 2^32 (one save a second for 136 years) and still work."""
    return 0 < ((a - b) & 0xFFFFFFFF) < 0x80000000


def read_copy(image, copy, arch=None):
    boot_at, tasks_at = PLACES[copy]
    cfg = decode(image[boot_at * SECTOR:(boot_at + 1) * SECTOR], arch)
    cfg.programs = decode_tasks(image[tasks_at * SECTOR:tasks_at * SECTOR + TASK_TABLE], cfg)
    return cfg


def pick(image, arch=None, why=None):
    """A myos-config partition -> (which copy, KConfig with its programs): the
    newer copy whose boot block and task table are both good and agree. Save
    always overwrites the other copy, so a power cut mid-save leaves the last
    good one in use. (None, None): use the built-in default."""
    good = []
    for which in PLACES:
        try:
            good.append((which, read_copy(image, which, arch)))
        except ValueError as e:
            if why is not None:
                why.append(f"copy {which}: {e}")
    if not good:
        return None, None
    if len(good) == 2 and newer(good[1][1].generation, good[0][1].generation):
        return good[1]
    return good[0]


def older_copy(image, arch=None):
    """Which copy Save overwrites: the one not in use (or A on a new disk)."""
    which, cfg = pick(image, arch)
    return ("B" if which == "A" else "A"), (cfg.generation if cfg else 0)


# --- pictures ----------------------------------------------------------------

def timeline(cfg):
    """The 20 ms frame as text, 10 slots a row."""
    out = []
    for row in (0, 10):
        out.append("  ms " + "".join(f"{s:<9}" for s in range(row, row + 10)))
        out.append("     " + "".join(f"[{(n or '.')[:7]:<7}]" for n in cfg.slots[row:row + 10]))
    return "\n".join(out)


def hexdump(block, start=0, end=None, layout=LAYOUT, base=0):
    """Rows of 16 bytes from start to end, with the fields that begin in each."""
    end = len(block) if end is None else end
    names = {base + off: name for off, name, *_ in layout}
    lines = []
    for row in range(start, end, 16):
        chunk = block[row:row + 16]
        if end == len(block) and row >= start + 0x40 and not any(block[row:]):
            lines.append("  ...       (zeros to the end)")
            break
        here = [names[o] for o in range(row, row + 16) if o in names]
        lines.append(f"  {row:04x}  {chunk.hex(' ')}   {', '.join(here)}")
    return "\n".join(lines)


def _c_struct(name, layout, special=None):
    out = [f"struct {name} {{"]
    for off, field_name, code, ctype, comment in layout:
        count = int(code[:-1]) if code[-1] == "s" else 1
        decl = (special or {}).get(field_name) or (f"{field_name}[{count}]" if count > 1 else field_name)
        out.append(f"    {ctype + ' ' + decl + ';':<40}/* 0x{off:03X} {comment} */")
    return out + ["};"]


def c_header():
    """The structs the kernel reads the two parts into (include/kconfig.h)."""
    out = ["/* Generated by tools/kconfig/kconfig.py header: the kernel config",
           "   (docs/KERNEL-CONFIG.md). Every field sits at a naturally aligned",
           "   offset: no packing needed. */",
           "#define KCFG_MAGIC        \"MYOSKCFG\"",
           "#define KCFG_TASK_MAGIC   \"MYOSTASK\"",
           f"#define KCFG_VERSION      {VERSION}",
           f"#define KCFG_SLOTS        {NSLOTS}",
           f"#define KCFG_MAX_TASKS    {MAX_TASKS}",
           f"#define KCFG_OWNER_FREE   0x{OWNER_FREE:02X}",
           f"#define KCFG_OWNER_OS     0x{OWNER_OS:02X}",
           "#define KCFG_RT_LEFTOVER  (1u << 0)",
           "#define KCFG_RT_DEFER_IRQ (1u << 1)",
           "#define KCFG_TASK_START_AT_BOOT (1u << 0)",
           "#define KCFG_TASK_RESTART       (1u << 1)",
           f"#define KCFG_BOOT_SECTOR_A  {PLACES['A'][0]}   /* in the myos-config partition */",
           f"#define KCFG_BOOT_SECTOR_B  {PLACES['B'][0]}",
           f"#define KCFG_TASKS_SECTOR_A {PLACES['A'][1]}",
           f"#define KCFG_TASKS_SECTOR_B {PLACES['B'][1]}", ""]
    for f in pf.FEATURES:
        out.append(f"#define KCFG_PM_{f.key.upper():<16} (1u << {f.bit:2})  /* {f.label} */")
    out += [""] + _c_struct("kconfig", LAYOUT, {"task_name": "task_name[KCFG_MAX_TASKS][16]",
                                                "slot_owner": "slot_owner[KCFG_SLOTS]"})
    out += ["_Static_assert(sizeof(struct kconfig) == 512, \"one sector\");", ""]
    out += _c_struct("kconfig_task", TASK_FIELDS)
    out += ["_Static_assert(sizeof(struct kconfig_task) == 256, \"one entry\");", ""]
    head = _c_struct("kconfig_tasks", TASK_HEADER)
    head[-1:] = ["    struct kconfig_task task[KCFG_MAX_TASKS];                /* 0x040 */",
                 f"    uint8_t reserved2[{TASK_TABLE - TASKS_AT - MAX_TASKS * TASK_ENTRY}];"
                 f"                                /* 0x{TASKS_AT + MAX_TASKS * TASK_ENTRY:03X} */", "};"]
    out += head + [f"_Static_assert(sizeof(struct kconfig_tasks) == {TASK_TABLE}, \"ten sectors\");"]
    return "\n".join(out)


# --- the command line --------------------------------------------------------

EXAMPLE = """\
# The demo: soft realtime, three named tasks and the programs they run.
mode = soft-realtime
arch = x86_64
latency_budget_us = 100

[slots]
2  = audio
4  = brakes
6  = radio
9  = brakes
12 = audio
14 = brakes
16 = radio
19 = brakes

[task audio]
program = /usr/bin/myos-audiod
args = --period 10ms
uid = 65534
gid = 18                    # audio

[task brakes]
program = /bin/brakes
args = --hz 200
uid = 65534
gid = 65534

[task radio]
program = /usr/local/bin/radio
uid = 1000
gid = 1000
restart_ms = 5000

[power]
on   = cpu_speed deep_idle tickless nvme_apst usb_autosuspend wifi_ps
seen = cpu_speed no_boost deep_idle c1e tickless nvme_apst pcie_aspm usb_autosuspend wifi_ps audio_pm
cpu_min_pct = 30
cpu_max_pct = 100
cstate_max = 3
"""


def _show_checks(errors, warnings, notes, indent="   "):
    for kind, msgs in (("error:  ", errors), ("warning:", warnings), ("note:   ", notes)):
        for m in msgs:
            print(indent + kind, m)


def demo():
    cfg = parse_text(EXAMPLE)
    print("1. The config, as Settings holds it (docs/kernel-config/example.conf).\n"
          "   The boot part, the slots:\n")
    print(timeline(cfg))
    print("\n   The run part, what starts after boot:\n")
    for t in cfg.tasks():
        p = cfg.programs[t]
        print(f"   {t:8} {p.program} {' '.join(p.args):14} uid {p.uid:<5} "
              f"{'starts at boot' if p.start_at_boot else 'manual'}, "
              f"{'restarted after ' + str(p.restart_ms) + ' ms' if p.restart else 'not restarted'}")
    print("\n2. The checks Save runs:")
    _show_checks(*check(cfg))

    image = save(cfg, bytes(PARTITION_USED), "A", 7)
    boot = image[:BLOCK]
    print("\n3. The boot block for x86_64 (little-endian), generation 7, in sector 0:\n")
    print(hexdump(boot))
    print("\n   The task table, in sectors 16-25 (the header and the first entry):\n")
    tasks = image[16 * SECTOR:16 * SECTOR + TASK_TABLE]
    print(hexdump(tasks, 0, 0x40, TASK_HEADER))
    print(hexdump(tasks, 0x40, 0x140, [(TASKS_AT + o, *rest) for o, *rest in TASK_FIELDS]))

    big = parse_text(EXAMPLE)
    big.arch, big.byte_order = "arm", "big"
    be = encode(big, 7)
    print("\n   The same boot block for a big-endian ARM: only the multi-byte numbers")
    print("   change (and the CRC with them):\n")
    print(hexdump(be, 0, 0x20))
    print(hexdump(be, 0x160, 0x170))

    print("\n4. Reading it back, as at boot: the kernel picks a copy, then myos-taskd")
    print("   starts what the task table says:")
    which, got = pick(image)
    print(f"   copy {which}: {MODE_NAMES[got.mode]}, generation {got.generation}")
    for t, p in got.programs.items():
        print(f"   start {p.program} {' '.join(p.args)} as uid {p.uid}, bound to task '{t}'")

    print("\n5. Two copies, A and B. Save overwrites the one not in use:")
    older = save(cfg, image, "B", 6)        # B holds an older save
    print("   A gen 7, B gen 6:                       uses", pick(older)[0])
    newer_save = save(cfg, older, "B", 8)
    print("   Save writes B as gen 8:                 uses", pick(newer_save)[0])
    cut = bytearray(older)                  # power cut: B's new table written, its boot block not
    cut[32 * SECTOR:32 * SECTOR + TASK_TABLE] = encode_tasks(cfg, 8)
    why = []
    print("   power cut between B's two writes:       uses", pick(bytes(cut), why=why)[0])
    print("     (" + why[0] + ")")
    torn = bytearray(newer_save)
    torn[1 * SECTOR + 0x100] ^= 1
    why = []
    print("   B's boot block damaged:                 uses", pick(bytes(torn), why=why)[0])
    print("     (" + why[0] + ")")
    print("   a blank partition:                      uses",
          pick(bytes(PARTITION_USED))[0] or "the built-in default")

    print("\n6. A slot whose task has no program can't be saved:")
    bad = parse_text(EXAMPLE)
    bad.slots[7] = "pump"
    _show_checks([m for m in check(bad)[0]], [], [])


def parse_slots_conf(text, where="config/slots.conf"):
    """config/slots.conf ("brakes 4 9 14 19" per line) -> KConfig: the
    kernel's built-in default. Realtime, as the scheduler runs today (the
    modes come with docs/KERNEL-CONFIG.md step 6); each task's program is
    /bin/NAME, not started at boot (nothing starts tasks yet: step 5).
    Mistakes stop the build, as slots2c.awk's did."""
    cfg = KConfig(mode=MODES["realtime"], rt_flags=RT_LEFTOVER)
    errors = []
    for n, raw in enumerate(text.splitlines(), 1):
        words = raw.split("#", 1)[0].split()
        if not words:
            continue
        name, slots = words[0], words[1:]
        if not slots:
            errors.append(f"{where}:{n}: '{name}' has no slots")
        for s in slots:
            if not s.isdigit() or int(s) >= NSLOTS:
                errors.append(f"{where}:{n}: '{s}' is not a slot number (0-{NSLOTS - 1})")
            elif int(s) in OS_SLOTS:
                errors.append(f"{where}:{n}: slot {s} belongs to the OS")
            elif cfg.slots[int(s)]:
                errors.append(f"{where}:{n}: slot {s} given to both {cfg.slots[int(s)]} and {name}")
            else:
                cfg.slots[int(s)] = name
        cfg.programs[name] = Task(program="/bin/" + name, start_at_boot=False, restart=False)
    return cfg, errors


def c_default(cfg, source):
    """Both parts as two C arrays, for the kernel to link in."""
    boot, tasks = encode(cfg, 1), encode_tasks(cfg, 1)
    out = [f"/* Generated from {source} by tools/kconfig/kconfig.py cdefault: do not edit.",
           "   The kernel's built-in config (docs/KERNEL-CONFIG.md §10 step 2), in the",
           "   same bytes a myos-config partition holds; kernel/kconfig.c checks it",
           "   at boot as it checks one read from the disk. */",
           "#include <stdint.h>", ""]
    for name, data in (("kconfig_default_boot", boot), ("kconfig_default_tasks", tasks)):
        out.append(f"const uint8_t {name}[{len(data)}] __attribute__((aligned(8))) = {{")
        for i in range(0, len(data), 16):
            out.append("    " + " ".join(f"0x{b:02x}," for b in data[i:i + 16]))
        out.append("};")
    return "\n".join(out) + "\n"


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    cmd, args = argv[0], argv[1:]
    opt = dict(zip(args[1::2], args[2::2]))
    if cmd == "demo":
        demo()
    elif cmd == "header":
        print(c_header())
    elif cmd == "layout":
        for title, layout in (("boot block", LAYOUT), ("task table header", TASK_HEADER),
                              ("task table entry (18 of them, from 0x040)", TASK_FIELDS)):
            print(f"{title}:")
            for off, name, code, ctype, comment in layout:
                print(f"  0x{off:03X}  {struct.calcsize('<' + code):4}  {name:18} {comment}")
    elif cmd in ("check", "partition") and args:
        cfg = parse_text(open(args[0]).read())
        if "--arch" in opt:
            cfg.arch = opt["--arch"]
        if "--big" in args:
            cfg.byte_order = "big"
        errors, warnings, notes = check(cfg, opt.get("--cpu"), opt.get("--root"))
        _show_checks(errors, warnings, notes, "")
        if cmd == "partition" and not errors:
            out = opt.get("-o", "myos-config.img")
            image = save(cfg, bytes(PARTITION_USED), "A", int(opt.get("--generation", "1")), opt.get("--root"))
            with open(out, "wb") as f:
                f.write(image)
                f.truncate(1 << 20)         # the whole 1 MiB partition
            print(f"wrote {out}: copy A, {cfg.arch}, {cfg.order()}-endian")
        return 1 if errors else 0
    elif cmd == "cdefault" and args:
        cfg, errors = parse_slots_conf(open(args[0]).read(), args[0])
        cfg.arch = opt.get("--arch", "i386")
        if not errors:
            errors = check(cfg)[0]
        for e in errors:
            print(e, file=sys.stderr)
        if errors:
            return 1
        text = c_default(cfg, args[0])
        if "-o" in opt:
            with open(opt["-o"], "w") as f:
                f.write(text)
        else:
            sys.stdout.write(text)
        return 0
    elif cmd == "decode" and args:
        image = open(args[0], "rb").read()
        why = []
        which, cfg = pick(image, why=why)
        for w in why:
            print(w)
        if not cfg:
            print("no good copy: the kernel would use its built-in default")
            return 1
        print(f"copy {which}: {MODE_NAMES[cfg.mode]}, {cfg.arch}, {cfg.order()}-endian, "
              f"generation {cfg.generation}")
        print(timeline(cfg))
        for t, p in cfg.programs.items():
            print(f"{t}: {p.program} {' '.join(p.args)} (uid {p.uid})")
        print("power on:", " ".join(sorted(cfg.power_on)) or "-")
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
