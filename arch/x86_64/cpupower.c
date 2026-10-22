/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * cpupower.c — the processor's power savings (docs/KERNEL-CONFIG.md §5)
 *
 * arch/i386/cpupower.c and arch/x86_64/cpupower.c are the same file: the
 * CPUID bits and MSRs are the same in both modes.
 *
 * In "max power savings" mode the kernel config switches some of these on,
 * and cpupower_init sets them up once, when the config is known:
 *
 *   cpu_speed  The processor picks its own speed inside the config's range.
 *              Intel HWP ("Speed Shift", Skylake and later) and AMD CPPC
 *              both take a lowest and a highest performance level and an
 *              energy preference (EPP: 0 speed .. 255 energy), then change
 *              speed by themselves within microseconds. No governor in the
 *              kernel, so nothing on any hot path.
 *   no_boost   Never above the rated speed: Intel Turbo Boost off
 *              (IA32_MISC_ENABLE bit 38), AMD Core Performance Boost off
 *              (HWCR bit 25), and HWP's / CPPC's highest level the rated one.
 *   c1e        Intel: a halted core also drops its clock and voltage
 *              (POWER_CTL bit 1).
 *   deep_idle  The idle thread sleeps with MWAIT and a C-state hint instead
 *              of HLT, down to the config's cstate_max (Intel Core).
 *   throttle   Clock modulation: the processor skips clock cycles, down to
 *              cpu_max_pct of its speed (IA32_CLOCK_MODULATION). The way to
 *              slow a Pentium 4 that has no SpeedStep.
 *
 * And in every mode, the processor's energy meter (RAPL) for measuring
 * them: /sys/class/powercap/intel-rapl:0/energy_uj (sysfs.c), as on Linux.
 *
 * And at any time, the platform profile (/sys/firmware/acpi/platform_profile,
 * the battery tray's slider), as on Linux:
 *
 *   low-power    boost off, at most 60% of the rated speed, the energy
 *                preference at "energy"; a processor without HWP or CPPC
 *                is slowed by clock modulation instead (to half its clock)
 *   balanced     the whole range, boost allowed, the preference between
 *   performance  the whole range, boost allowed, the preference at "speed"
 *
 * Until it is first set it reads "balanced", and the kernel config's
 * settings above stand. Setting it is a few MSR writes, in the writer's
 * system call.
 *
 * Each is checked against CPUID first, and every MSR access may fault
 * (rdmsr_safe): an MSR a model or a virtual machine lacks is reported, not
 * a crash. The log says what was done, what wasn't, and why.
 *
 * Not yet: SpeedStep without HWP (Pentium M, Pentium 4 6x0: it needs the
 * ACPI _PSS table of speeds and voltages, and a governor), ACPI C2/C3 idle
 * (Pentium M, and Zen's CC6: the ACPI _CST table), and tickless (the local
 * APIC timer in one-shot mode). The realtime modes change nothing here yet
 * (the firmware's settings stay): the scheduler's modes are step 6 (§10).
 */
#include <cpu.h>
#include <interrupts.h>
#include <kconfig.h>
#include <kprintf.h>

#define MSR_MISC_ENABLE       0x1A0         /* Intel; bit 38: turbo off */
#define MISC_TURBO_OFF        (1ull << 38)
#define MSR_CLOCK_MODULATION  0x19A
#define MSR_ENERGY_PERF_BIAS  0x1B0         /* 0 speed .. 15 energy */
#define MSR_POWER_CTL         0x1FC         /* Intel Core; bit 1: C1E */
#define POWER_CTL_C1E         (1ull << 1)
#define MSR_PM_ENABLE         0x770         /* HWP on: can't be undone until reset */
#define MSR_HWP_CAPABILITIES  0x771         /* highest, guaranteed, efficient, lowest */
#define MSR_HWP_REQUEST       0x774         /* min, max, desired, EPP */
#define MSR_AMD_HWCR          0xC0010015    /* bit 25: boost off */
#define HWCR_CPB_OFF          (1ull << 25)
#define MSR_AMD_CPPC_CAP1     0xC00102B0    /* highest, nominal, lowest non-linear, lowest */
#define MSR_AMD_CPPC_ENABLE   0xC00102B1
#define MSR_AMD_CPPC_REQ      0xC00102B3    /* max, min, desired, EPP */
#define MSR_RAPL_POWER_UNIT   0x606         /* bits 12:8: energy in 1/2^n J */
#define MSR_PKG_ENERGY_STATUS 0x611         /* the package's joules, 32 bits */
#define MSR_AMD_RAPL_UNIT     0xC0010299    /* the same two, AMD's numbers */
#define MSR_AMD_PKG_ENERGY    0xC001029B

/* CPUID */
#define C1_ECX_MONITOR   (1u << 3)          /* leaf 1 */
#define C1_ECX_HYPERVISOR (1u << 31)
#define C1_EDX_ACPI      (1u << 22)         /* clock modulation */
#define C6_EAX_TURBO     (1u << 1)          /* leaf 6 */
#define C6_EAX_ECMD      (1u << 5)          /* clock modulation in 6.25% steps */
#define C6_EAX_HWP       (1u << 7)
#define C6_EAX_HWP_EPP   (1u << 10)
#define C6_ECX_EPB       (1u << 3)
#define E7_EDX_CPB       (1u << 9)          /* leaf 0x80000007: AMD boost */
#define E7_EDX_RAPL      (1u << 14)         /* AMD's energy meter */
#define E8_EBX_CPPC      (1u << 27)         /* leaf 0x80000008 */

#define HLT 0xFFFFFFFFu
static uint32_t idle_hint = HLT;            /* MWAIT's C-state hint, or HLT */
static volatile uint8_t wake_line[64] __attribute__((aligned(64)));  /* MONITOR's */
static uint32_t energy_msr, energy_shift;   /* RAPL: 0 if none; its unit, 1/2^shift J */

/* Intel Core, Skylake to Tiger Lake: MWAIT hints for the C-states (Linux's
   intel_idle, skl_cstates). The P2 Max's Amber Lake (model 0x8E) and the
   MSI Katana's Tiger Lake (0x8D) are in this family. A hint the processor
   doesn't have still works: it takes the nearest shallower state. */
static const struct { uint8_t c, hint; } core_cstates[] = {
    { 1, 0x00 }, { 3, 0x10 }, { 6, 0x20 }, { 7, 0x33 }, { 8, 0x40 }, { 9, 0x50 }, { 10, 0x60 },
};
static const uint8_t core_models[] = {
    0x4E, 0x5E,         /* Skylake */
    0x8E, 0x9E,         /* Kaby Lake, Amber Lake, Whiskey Lake, Coffee Lake */
    0xA5, 0xA6,         /* Comet Lake */
    0x7D, 0x7E,         /* Ice Lake */
    0x8C, 0x8D,         /* Tiger Lake */
};

static struct {
    int intel, amd, vm;
    uint32_t family, model;
    uint32_t c1_ecx, c1_edx, c5_ecx, c5_edx, c6_eax, c6_ecx, e7_edx, e8_ebx;
} cpu;

static void detect(void)
{
    uint32_t r[4];
    cpu_cpuid(0, 0, r);
    uint32_t max = r[0];
    cpu.intel = r[1] == 0x756E6547 && r[3] == 0x49656E69 && r[2] == 0x6C65746E;  /* GenuineIntel */
    cpu.amd = r[1] == 0x68747541 && r[3] == 0x69746E65 && r[2] == 0x444D4163;    /* AuthenticAMD */
    cpu_cpuid(1, 0, r);
    cpu.family = r[0] >> 8 & 0xF;
    cpu.model = r[0] >> 4 & 0xF;
    if (cpu.family == 0xF)
        cpu.family += r[0] >> 20 & 0xFF;
    if (cpu.family == 6 || cpu.family >= 0xF)
        cpu.model |= (r[0] >> 16 & 0xF) << 4;
    cpu.c1_ecx = r[2];
    cpu.c1_edx = r[3];
    cpu.vm = (r[2] & C1_ECX_HYPERVISOR) != 0;
    if (max >= 5 && (cpu.c1_ecx & C1_ECX_MONITOR)) {
        cpu_cpuid(5, 0, r);
        cpu.c5_ecx = r[2];
        cpu.c5_edx = r[3];
    }
    if (max >= 6) {
        cpu_cpuid(6, 0, r);
        cpu.c6_eax = r[0];
        cpu.c6_ecx = r[2];
    }
    cpu_cpuid(0x80000000, 0, r);
    uint32_t emax = r[0];
    if (emax >= 0x80000007) {
        cpu_cpuid(0x80000007, 0, r);
        cpu.e7_edx = r[3];
    }
    if (emax >= 0x80000008) {
        cpu_cpuid(0x80000008, 0, r);
        cpu.e8_ebx = r[1];
    }
}

/* Read-modify-write of an MSR that may be missing, then read it back:
   firmware can lock one, and an emulator may ignore the write. 0, -1 (no
   such MSR) or -2 (the write didn't take). */
static int msr_set(uint32_t msr, uint64_t clear, uint64_t set)
{
    uint64_t v;
    if (rdmsr_safe(msr, &v) < 0 || wrmsr_safe(msr, (v & ~clear) | set) < 0)
        return -1;
    if (rdmsr_safe(msr, &v) < 0 || (v & (clear | set)) != set)
        return -2;
    return 0;
}

static const char *why(int err)
{
    if (err == -1)
        return cpu.vm ? "the virtual machine has no such MSR" : "the processor has no such MSR";
    return cpu.vm ? "the virtual machine ignored the setting"
                  : "the processor kept its old setting (locked by the firmware?)";
}

/* pct of the rated (non-boost) level, never below the lowest. */
static uint32_t level(uint32_t rated, uint32_t lowest, uint32_t pct)
{
    uint32_t l = rated * pct / 100;
    return l < lowest ? lowest : l;
}

/* The speed range the config asks for, on a scale where `rated` is the
   highest non-boost level: [*lo, *hi], hi = highest when boost may run. */
static void range(const struct kconfig *k, uint32_t lowest, uint32_t rated, uint32_t highest,
                  uint32_t *lo, uint32_t *hi)
{
    int boost = !(k->power_on & KCFG_PM_NO_BOOST) && k->cpu_max_pct >= 100;
    *hi = boost ? highest : level(rated, lowest, k->cpu_max_pct);
    if (*hi > highest)
        *hi = highest;
    *lo = level(rated, lowest, k->cpu_min_pct);
    if (*lo > *hi)
        *lo = *hi;
}

static void speed(const struct kconfig *k)
{
    uint64_t caps;
    uint32_t lo, hi;
    if (cpu.intel && (cpu.c6_eax & C6_EAX_HWP)) {
        /* HWP: once on, the processor chooses its speed itself, between
           min and max, leaning to speed or energy by EPP. "desired" 0
           leaves the choice to it. */
        int err = msr_set(MSR_PM_ENABLE, 0, 1);
        if (!err && rdmsr_safe(MSR_HWP_CAPABILITIES, &caps) < 0)
            err = -1;
        if (err) {
            klog("cpupower: speed: HWP: %s\n", why(err));
            return;
        }
        uint32_t highest = caps & 0xFF, rated = caps >> 8 & 0xFF, lowest = caps >> 24 & 0xFF;
        if (!rated || rated > highest || lowest > rated) {
            klog("cpupower: speed: HWP's levels make no sense (%lx): left alone\n", (unsigned long)caps);
            return;
        }
        range(k, lowest, rated, highest, &lo, &hi);
        uint64_t req = lo | hi << 8;
        if (cpu.c6_eax & C6_EAX_HWP_EPP)
            req |= (uint64_t)k->epp << 24;
        else if (cpu.c6_ecx & C6_ECX_EPB)       /* the older, 4-bit preference */
            msr_set(MSR_ENERGY_PERF_BIAS, 0xF, k->epp * 15u / 255);
        if ((err = msr_set(MSR_HWP_REQUEST, ~0ull, req)) != 0) {
            klog("cpupower: speed: HWP request: %s\n", why(err));
            return;
        }
        klog("cpupower: speed: HWP, levels %lu-%lu (it has %lu-%lu, rated %lu), energy preference %u\n",
             (unsigned long)lo, (unsigned long)hi, (unsigned long)lowest, (unsigned long)highest,
             (unsigned long)rated, k->epp);
    } else if (cpu.amd && (cpu.e8_ebx & E8_EBX_CPPC)) {
        /* CPPC: the same idea, AMD's registers (Linux's amd-pstate). */
        int err = rdmsr_safe(MSR_AMD_CPPC_CAP1, &caps) < 0 ? -1 : msr_set(MSR_AMD_CPPC_ENABLE, 0, 1);
        if (err) {
            klog("cpupower: speed: CPPC: %s\n", why(err));
            return;
        }
        uint32_t highest = caps >> 24 & 0xFF, rated = caps >> 16 & 0xFF, lowest = caps & 0xFF;
        if (!rated || rated > highest || lowest > rated) {
            klog("cpupower: speed: CPPC's levels make no sense (%lx): left alone\n", (unsigned long)caps);
            return;
        }
        range(k, lowest, rated, highest, &lo, &hi);
        if ((err = msr_set(MSR_AMD_CPPC_REQ, ~0ull, hi | lo << 8 | (uint64_t)k->epp << 24)) != 0) {
            klog("cpupower: speed: CPPC request: %s\n", why(err));
            return;
        }
        klog("cpupower: speed: CPPC, levels %lu-%lu (it has %lu-%lu, rated %lu), energy preference %u\n",
             (unsigned long)lo, (unsigned long)hi, (unsigned long)lowest, (unsigned long)highest,
             (unsigned long)rated, k->epp);
    } else {
        klog("cpupower: speed: not yet on this processor (no HWP or CPPC; SpeedStep "
             "needs ACPI's table of speeds)\n");
    }
}

static void boost(int on)
{
    uint64_t v;
    int err;
    if (cpu.intel && ((cpu.c6_eax & C6_EAX_TURBO) ||
                      (rdmsr_safe(MSR_MISC_ENABLE, &v) == 0 && (v & MISC_TURBO_OFF)))) {
        err = msr_set(MSR_MISC_ENABLE, on ? MISC_TURBO_OFF : 0, on ? 0 : MISC_TURBO_OFF);
        klog("cpupower: boost: %s\n", err ? why(err) : on ? "Turbo Boost on" : "Turbo Boost off");
    } else if (cpu.amd && (cpu.e7_edx & E7_EDX_CPB)) {
        err = msr_set(MSR_AMD_HWCR, on ? HWCR_CPB_OFF : 0, on ? 0 : HWCR_CPB_OFF);
        klog("cpupower: boost: %s\n", err ? why(err) : on ? "Core Performance Boost on"
                                                         : "Core Performance Boost off");
    } else {
        klog("cpupower: boost: this processor has none\n");
    }
}

static void c1e(void)
{
    int err;
    if (!cpu.intel || cpu.family != 6)
        klog("cpupower: C1E: left as the firmware set it (Intel Core only)\n");
    else
        klog("cpupower: C1E: %s\n", (err = msr_set(MSR_POWER_CTL, 0, POWER_CTL_C1E)) ? why(err) : "on");
}

static int is_core(void)
{
    if (!cpu.intel || cpu.family != 6)
        return 0;
    for (unsigned i = 0; i < sizeof core_models; i++)
        if (core_models[i] == cpu.model)
            return 1;
    return 0;
}

/* Does CPUID leaf 5 list the hint's sub-state? Its EDX has 4 bits per
   MWAIT C-state: bits 7:4 count C1's sub-states, 11:8 the next one's... */
static int has_hint(uint32_t hint)
{
    return (cpu.c5_edx >> (4 * ((hint >> 4) + 1)) & 0xF) > (hint & 0xF);
}

static void deep_idle(const struct kconfig *k)
{
    if (!(cpu.c1_ecx & C1_ECX_MONITOR)) {
        klog("cpupower: deep idle: no MWAIT: halt only\n");
        return;
    }
    if (!cpu.intel) {
        klog("cpupower: deep idle: not yet on this processor (its deep states need ACPI's "
             "table): halt only\n");
        return;
    }
    uint32_t c = 1, hint = 0;               /* MWAIT C1: any Intel with MWAIT */
    if (is_core())
        for (unsigned i = 1; i < sizeof core_cstates / sizeof core_cstates[0]; i++)
            if (core_cstates[i].c <= k->cstate_max && has_hint(core_cstates[i].hint)) {
                c = core_cstates[i].c;
                hint = core_cstates[i].hint;
            }
    idle_hint = hint;
    klog("cpupower: deep idle: MWAIT, down to C%lu (hint %02lx)%s\n", (unsigned long)c,
         (unsigned long)hint, is_core() ? "" : ": the deeper states of this model aren't known");
}

static void throttle(const struct kconfig *k)
{
    if (!cpu.intel || !(cpu.c1_edx & C1_EDX_ACPI)) {      /* an Intel MSR */
        klog("cpupower: clock modulation: this processor has none\n");
        return;
    }
    if (k->cpu_max_pct >= 100) {
        uint64_t v;
        if (rdmsr_safe(MSR_CLOCK_MODULATION, &v) == 0 && (v & 0x10))
            msr_set(MSR_CLOCK_MODULATION, 0x10, 0);     /* a profile had it on */
        klog("cpupower: clock modulation: off (the fastest speed is 100%%)\n");
        return;
    }
    /* Bit 4 on; the duty cycle (the share of clock cycles kept) in bits
       3:1, in 12.5% steps, or with ECMD in bits 3:0, in 6.25% steps. */
    uint32_t steps = cpu.c6_eax & C6_EAX_ECMD ? 16 : 8;
    uint32_t duty = k->cpu_max_pct * steps / 100;
    if (duty < 1)
        duty = 1;
    uint64_t v = 0x10 | (steps == 16 ? duty : duty << 1);
    int err = msr_set(MSR_CLOCK_MODULATION, 0x1F, v);
    if (err)
        klog("cpupower: clock modulation: %s\n", why(err));
    else
        klog("cpupower: clock modulation: %lu/%lu of the clock\n", (unsigned long)duty,
             (unsigned long)steps);
}

/* The energy meter: Intel's since Sandy Bridge, AMD's since Zen. An
   emulated processor reads 0 for the unit: no meter. */
static void meter(void)
{
    uint64_t unit, e;
    uint32_t unit_msr = 0;
    if (cpu.intel && cpu.family == 6) {
        unit_msr = MSR_RAPL_POWER_UNIT;
        energy_msr = MSR_PKG_ENERGY_STATUS;
    } else if (cpu.amd && (cpu.e7_edx & E7_EDX_RAPL)) {
        unit_msr = MSR_AMD_RAPL_UNIT;
        energy_msr = MSR_AMD_PKG_ENERGY;
    }
    if (!unit_msr || rdmsr_safe(unit_msr, &unit) < 0 || !(unit >> 8 & 0x1F) ||
        rdmsr_safe(energy_msr, &e) < 0) {
        energy_msr = 0;
        return;
    }
    energy_shift = unit >> 8 & 0x1F;
    klog("cpupower: energy meter: RAPL, in 1/%lu J\n", 1ul << energy_shift);
}

int cpu_energy_uj(uint64_t *uj, uint64_t *range_uj)
{
    uint64_t raw;
    if (!energy_msr || rdmsr_safe(energy_msr, &raw) < 0)
        return -1;
    *uj = ((raw & 0xFFFFFFFF) * 1000000) >> energy_shift;   /* < 2^52: no overflow */
    *range_uj = (1000000ull << 32) >> energy_shift;
    return 0;
}

void cpupower_init(void)
{
    static const char *const modes[] = { "realtime", "soft realtime", "power saving" };
    const struct kconfig *k = kconfig_boot();
    detect();
    meter();
    if (k->mode != KCFG_POWER_SAVING) {
        klog("cpupower: %s: no power savings (the firmware's settings stay)\n", modes[k->mode]);
        return;
    }
    uint32_t on = k->power_on;
    klog("cpupower: power saving on %s family %lu model %02lx%s\n",
         cpu.intel ? "Intel" : cpu.amd ? "AMD" : "x86", (unsigned long)cpu.family,
         (unsigned long)cpu.model, cpu.vm ? " (a virtual machine)" : "");
    if (on & KCFG_PM_NO_BOOST)
        boost(0);
    if (on & KCFG_PM_CPU_SPEED)
        speed(k);
    if (on & KCFG_PM_C1E)
        c1e();
    if (on & KCFG_PM_DEEP_IDLE)
        deep_idle(k);
    if (on & KCFG_PM_THROTTLE)
        throttle(k);
    if (on & KCFG_PM_TICKLESS)
        klog("cpupower: tickless: not yet (the 1 ms tick stays)\n");
}

static int profile = 1;                     /* balanced: the config's settings */

int cpupower_profile(void)
{
    return profile;
}

int cpupower_set_profile(int p)
{
    static const char *const names[] = { "low-power", "balanced", "performance" };
    if (p < 0 || p > 2)
        return -1;
    struct kconfig k = *kconfig_boot();
    k.cpu_max_pct = p == 0 ? 60 : 100;
    k.epp = p == 0 ? 255 : p == 1 ? 128 : 0;
    k.power_on = p == 0 ? k.power_on | KCFG_PM_NO_BOOST : k.power_on & ~KCFG_PM_NO_BOOST;
    uint32_t f = irq_save();                /* one writer at a time */
    klog("cpupower: profile %s\n", names[p]);
    boost(p != 0);
    if ((cpu.intel && (cpu.c6_eax & C6_EAX_HWP)) || (cpu.amd && (cpu.e8_ebx & E8_EBX_CPPC))) {
        speed(&k);
    } else {
        k.cpu_max_pct = p == 0 ? 50 : 100;  /* the old way: skip clock cycles */
        throttle(&k);
    }
    profile = p;
    irq_restore(f);
    return 0;
}

/* The idle thread's sleep (thread.c cpu_idle). MONITOR arms a watch on one
   cache line and MWAIT sleeps in the hinted C-state until an interrupt
   comes. sti delays interrupts by one instruction, so one arriving after
   it wakes the mwait (or the hlt) instead of being missed before it. */
void cpu_sleep(void)
{
    if (idle_hint == HLT) {
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
        return;
    }
    __asm__ volatile ("monitor" : : "a"(wake_line), "c"(0), "d"(0));
    __asm__ volatile ("sti; mwait; cli" : : "a"(idle_hint), "c"(0) : "memory");
}
