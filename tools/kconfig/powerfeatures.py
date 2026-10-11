#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""powerfeatures.py — the power-saving features the kernel config can switch,
and which processors and machines have them.

Documentation you can run (docs/KERNEL-CONFIG.md). Each feature is one bit
in the config block's `power_on` and `power_seen` words; a bit number, once
given, is never reused for something else (old blocks must keep meaning
what they meant).

    python3 tools/kconfig/powerfeatures.py              every family, side by side
    python3 tools/kconfig/powerfeatures.py pentium-m    one family, with how and why
    python3 tools/kconfig/powerfeatures.py --budget 50  what soft realtime allows
    python3 tools/kconfig/powerfeatures.py --json       the catalogue, for the mockups
    python3 tools/kconfig/powerfeatures.py --c          the catalogue, for Settings (C)

The three processor families MyOS is built for (CLAUDE.md):
    pentium4    old desktop PCs: Pentium 4 (NetBurst), 2000-2006
    pentium-m   old laptops: Pentium M (Banias, Dothan), 2003-2006
    zen         the GPD Pocket 4: AMD Ryzen (Zen 4 / Zen 5), x86-64

Support is "yes", "no", "some" (only some models: the kernel asks CPUID),
"machine" (depends on the devices in the computer, not the processor) or
"smp" (needs the kernel to run on more than one core first).

Wake-up latencies are typical numbers for deciding what soft realtime may
use. The kernel uses the machine's own figures where it has them (ACPI
_CST for idle states, NVMe's power-state table for APST).
"""
import sys
from dataclasses import dataclass, field

FAMILIES = ("pentium4", "pentium-m", "zen")
FAMILY_NAMES = {
    "pentium4": "Pentium 4 (old desktop)",
    "pentium-m": "Pentium M (old laptop)",
    "zen": "Ryzen, Zen 4/5 (GPD Pocket 4)",
}


@dataclass(frozen=True)
class Feature:
    bit: int                # its bit in power_on / power_seen
    key: str                # short name, for config files and code
    label: str              # what Settings shows
    group: str              # Processor, Storage or Devices
    what: str               # one sentence: what it does
    support: dict           # family -> yes / no / some / machine / smp
    how: dict = field(default_factory=dict)     # family -> the mechanism
    wake_us: int = 0        # worst wake-up cost it adds, in microseconds
    slows_cpu: bool = False # it changes how fast running code runs
    saves: int = 1          # rough energy saved: 1 little, 2 some, 3 a lot
    param: str = ""         # the block field that tunes it, if any


FEATURES = [
    Feature(0, "cpu_speed", "Change processor speed", "Processor",
            "Runs the processor slower (and at a lower voltage) when there is little to do.",
            {"pentium4": "some", "pentium-m": "yes", "zen": "yes"},
            {"pentium4": "Enhanced SpeedStep on late models (Pentium 4 6x0): CPUID.1:ECX bit 7, "
                         "IA32_PERF_CTL (MSR 0x199)",
             "pentium-m": "Enhanced SpeedStep: IA32_MISC_ENABLE (0x1A0) bit 16 on, then "
                          "IA32_PERF_CTL (0x199); IA32_PERF_STATUS (0x198) says where it is",
             "zen": "CPPC (CPUID 0x80000008 EBX bit 27): the CPPC request MSR (0xC00102B3) "
                    "sets min / max / desired performance and the energy preference"},
            wake_us=10, slows_cpu=True, saves=3, param="cpu_min_pct, cpu_max_pct, epp"),
    Feature(1, "no_boost", "Turn off boost", "Processor",
            "Never runs above the rated speed: less heat, less power, steadier timing.",
            {"pentium4": "no", "pentium-m": "no", "zen": "yes"},
            {"zen": "Core Performance Boost off: HWCR (MSR 0xC0010015) bit 25"},
            slows_cpu=True, saves=2),
    Feature(2, "deep_idle", "Deep idle states", "Processor",
            "When idle, stops more of the processor than a plain halt does (C2, C3, ...).",
            {"pentium4": "no", "pentium-m": "yes", "zen": "yes"},
            {"pentium-m": "ACPI C2/C3 (and C4, Deeper Sleep, on Dothan): an I/O read of P_LVL2 / "
                          "P_LVL3 from the FADT's P_BLK; the local APIC timer stops in C3",
             "zen": "ACPI _CST: MWAIT hints (C1, C2) and CC6; the APIC timer keeps running (ARAT)"},
            wake_us=100, saves=3, param="cstate_max"),
    Feature(3, "c1e", "Enhanced halt (C1E)", "Processor",
            "A halted processor also drops its voltage and clock.",
            {"pentium4": "some", "pentium-m": "no", "zen": "yes"},
            {"pentium4": "Pentium 4 5x1 / 6xx and Pentium D; usually switched on by the BIOS",
             "zen": "set up by the firmware; the kernel only leaves it on or asks it off"},
            wake_us=10, saves=2),
    Feature(4, "throttle", "Clock modulation", "Processor",
            "Skips clock cycles to cool down. Saves little energy: the voltage stays the same.",
            {"pentium4": "yes", "pentium-m": "yes", "zen": "no"},
            {"pentium4": "on-demand clock modulation: IA32_CLOCK_MODULATION (MSR 0x19A), "
                         "CPUID.1:EDX bit 22",
             "pentium-m": "IA32_CLOCK_MODULATION (MSR 0x19A)"},
            slows_cpu=True, saves=1),
    Feature(5, "tickless", "Skip unneeded timer ticks", "Processor",
            "Wakes the processor only when a timer is due, not 1000 times a second.",
            {"pentium4": "yes", "pentium-m": "yes", "zen": "yes"},
            {"pentium4": "local APIC timer, one-shot mode",
             "pentium-m": "local APIC one-shot; in C3 the APIC timer stops, so the PIT or HPET "
                          "must wake it then",
             "zen": "local APIC one-shot (ARAT: it runs in every idle state)"},
            saves=2),
    Feature(6, "smt_off", "Turn off second threads", "Processor",
            "Hyper-Threading / SMT off: one thread per core, less power, steadier timing.",
            {"pentium4": "smp", "pentium-m": "no", "zen": "smp"},
            {"pentium4": "Hyper-Threading models only", "zen": "SMT siblings stay offline"},
            saves=1),
    Feature(7, "core_park", "Park idle cores", "Processor",
            "Puts whole cores to sleep while the others can do the work.",
            {"pentium4": "no", "pentium-m": "no", "zen": "smp"},
            {"zen": "a core with nothing scheduled stays in CC6"},
            wake_us=400, saves=2),
    Feature(8, "disk_spindown", "Spin down hard disks", "Storage",
            "Stops a hard disk's motor after minutes without use. The next read waits seconds.",
            {"pentium4": "machine", "pentium-m": "machine", "zen": "machine"},
            {"pentium4": "ATA STANDBY timer (IDLE command count); rotating disks only "
                         "(IDENTIFY word 217 is not 1)",
             "pentium-m": "same", "zen": "same (if a SATA hard disk is attached)"},
            saves=3, param="spindown_min"),
    Feature(9, "sata_lpm", "SATA link power management", "Storage",
            "Lets the SATA cable's link sleep between commands.",
            {"pentium4": "machine", "pentium-m": "machine", "zen": "machine"},
            {"pentium4": "AHCI CAP.SALP (ICH6 and later, in AHCI mode)",
             "pentium-m": "AHCI CAP.SALP (ICH6-M and later)", "zen": "AHCI CAP.SALP"},
            saves=1),
    Feature(10, "nvme_apst", "NVMe power states", "Storage",
            "The SSD drops into its own low-power states when idle (APST).",
            {"pentium4": "no", "pentium-m": "no", "zen": "machine"},
            {"zen": "Identify Controller APSTA, then Set Features 0x0C with the drive's table"},
            saves=2),
    Feature(11, "pcie_aspm", "PCIe link power management", "Devices",
            "PCI Express links sleep when no data moves (ASPM L0s / L1).",
            {"pentium4": "machine", "pentium-m": "machine", "zen": "machine"},
            {"pentium4": "ICH6 and later have PCIe ports; older PCs have plain PCI (nothing to do)",
             "pentium-m": "ICH6-M and later", "zen": "link control register of each port"},
            saves=2),
    Feature(12, "usb_autosuspend", "Suspend idle USB devices", "Devices",
            "Idle USB devices sleep. Keyboard, mouse and touchscreen never do.",
            {"pentium4": "machine", "pentium-m": "machine", "zen": "machine"},
            {"pentium4": "UHCI / EHCI port suspend", "pentium-m": "UHCI / EHCI port suspend",
             "zen": "xHCI U1/U2 link states and port suspend"},
            saves=2),
    Feature(13, "wifi_ps", "Wi-Fi power save", "Devices",
            "The Wi-Fi radio sleeps between beacons while connected.",
            {"pentium4": "machine", "pentium-m": "machine", "zen": "machine"},
            {"pentium4": "802.11 power-save mode, if the Wi-Fi driver has it (iwlwifi)",
             "pentium-m": "same", "zen": "same"},
            saves=2),
    Feature(14, "audio_pm", "Power down the sound chip", "Devices",
            "The sound chip powers down after a few seconds of silence.",
            {"pentium4": "machine", "pentium-m": "machine", "zen": "machine"},
            {"pentium4": "HD Audio codec D3 (AC'97 chips: nothing to do)",
             "pentium-m": "HD Audio codec D3", "zen": "HD Audio codec D3"},
            saves=1),
    Feature(15, "nic_idle", "Idle unplugged Ethernet", "Devices",
            "A wired network card with no cable plugged in powers down.",
            {"pentium4": "machine", "pentium-m": "machine", "zen": "machine"},
            {"pentium4": "e1000 / RTL8139 power states", "pentium-m": "same", "zen": "same"},
            saves=1),
    Feature(16, "gpu_pm", "Graphics power management", "Devices",
            "Lower graphics clocks and panel self-refresh while the picture doesn't change.",
            {"pentium4": "no", "pentium-m": "no", "zen": "machine"},
            {"zen": "amdgpu DPM and PSR, once MyOS has an amdgpu driver"},
            saves=2),
]

BY_KEY = {f.key: f for f in FEATURES}
BY_BIT = {f.bit: f for f in FEATURES}

# Idle states: what deep_idle's cstate_max can be, with typical wake-up costs.
# Soft realtime allows the deepest one whose wake-up fits its latency budget.
CSTATES = [  # (number, name, typical wake-up in microseconds, families)
    (1, "C1 (halt)", 1, FAMILIES),
    (2, "C2", 20, ("pentium-m", "zen")),
    (3, "C3", 100, ("pentium-m", "zen")),
    (4, "C4 (Deeper Sleep)", 200, ("pentium-m",)),
    (6, "C6 (core off)", 400, ("zen",)),
]


def available(family):
    """Features this processor family may have (the machine decides the rest)."""
    return [f for f in FEATURES if f.support.get(family, "no") != "no"]


def soft_realtime_cstate(family, budget_us):
    """The deepest idle state soft realtime may use: it must wake within the
    budget. Nothing else saves power in soft realtime (docs/KERNEL-CONFIG.md)."""
    best = CSTATES[0]
    for c in CSTATES:
        if family in c[3] and c[2] <= budget_us:
            best = c
    return best


def as_json():
    """The catalogue for the Settings mockups (docs/kernel-config/mockups.html)."""
    import json
    return json.dumps({
        "families": FAMILY_NAMES,
        "features": [{"bit": f.bit, "key": f.key, "label": f.label, "group": f.group, "what": f.what,
                      "support": f.support, "wake_us": f.wake_us, "slows_cpu": f.slows_cpu,
                      "saves": f.saves, "param": f.param} for f in FEATURES],
        "cstates": [{"n": c[0], "name": c[1], "wake_us": c[2], "families": list(c[3])} for c in CSTATES],
    }, indent=1)


def as_c():
    """The catalogue as a C header, for Settings and its helper (made by the
    build; live/myos-settings/kconfig.c includes it)."""
    def q(t):
        return '"' + t.replace("\\", "\\\\").replace('"', '\\"') + '"'
    sup = {"no": "KF_NO", "yes": "KF_YES", "some": "KF_SOME", "machine": "KF_MACHINE", "smp": "KF_SMP"}
    out = ["/* Generated by tools/kconfig/powerfeatures.py --c: do not edit. */",
           "/* family order: " + ", ".join(FAMILIES) + " */",
           f"#define KF_FAMILIES {len(FAMILIES)}", f"#define KF_FEATURES {len(FEATURES)}",
           f"#define KF_CSTATES {len(CSTATES)}",
           "static const char *const kf_family_key[KF_FAMILIES] = {" + ", ".join(q(f) for f in FAMILIES) + "};",
           "static const char *const kf_family_name[KF_FAMILIES] = {" +
           ", ".join(q(FAMILY_NAMES[f]) for f in FAMILIES) + "};",
           "static const struct kfeature kfeatures[KF_FEATURES] = {"]
    for f in FEATURES:
        out.append(f"    {{{f.bit}, {q(f.key)}, {q(f.label)}, {q(f.group)}, {q(f.what)}, "
                   "{" + ", ".join(sup[f.support.get(x, 'no')] for x in FAMILIES) + "}, "
                   f"{f.wake_us}, {1 if f.slows_cpu else 0}, {f.saves}}},")
    out.append("};")
    out.append("static const struct kcstate kcstates[KF_CSTATES] = {")
    for n, name, wake, fams in CSTATES:
        mask = sum(1 << i for i, x in enumerate(FAMILIES) if x in fams)
        out.append(f"    {{{n}, {q(name)}, {wake}, 0x{mask:x}}},")
    out.append("};")
    return "\n".join(out)


def main(argv):
    if argv and argv[0] == "--c":
        print(as_c())
        return 0
    if argv and argv[0] == "--json":
        print(as_json())
        return 0
    if argv and argv[0] == "--budget":
        budget = int(argv[1]) if len(argv) > 1 else 100
        print(f"Soft realtime with a {budget} us wake-up budget may idle down to:")
        for fam in FAMILIES:
            c = soft_realtime_cstate(fam, budget)
            print(f"  {FAMILY_NAMES[fam]:32} {c[1]} (wakes in about {c[2]} us)")
        print("Everything else stays off: fixed speed, no boost, devices always on.")
        return 0
    if argv and argv[0] in FAMILIES:
        fam = argv[0]
        print(f"{FAMILY_NAMES[fam]}\n")
        for f in FEATURES:
            s = f.support.get(fam, "no")
            print(f"bit {f.bit:2}  {f.label}  [{s}]")
            print(f"        {f.what}")
            if fam in f.how:
                print(f"        how: {f.how[fam]}")
            if f.param:
                print(f"        tuned by: {f.param}")
        return 0
    w = 34
    print(f"{'bit  feature':{w}}" + "".join(f"{x:>11}" for x in FAMILIES))
    for f in FEATURES:
        print(f"{f.bit:3}  {f.label:{w - 5}}" + "".join(f"{f.support.get(x, 'no'):>11}" for x in FAMILIES))
    print("\nyes: the processor has it   some: some models (CPUID tells)   "
          "machine: depends on the devices\nsmp: needs MyOS on more than one core   no: not on this processor")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
