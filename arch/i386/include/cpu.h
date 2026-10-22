/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_I386_CPU_H
#define ARCH_I386_CPU_H

#include <stdint.h>

/*
 * CPU features, from CPUID leaf 1, register EDX. Bit numbers are Intel's
 * (AMD uses the same ones here).
 */
#define CPU_FPU   (1u << 0)     /* x87 floating point */
#define CPU_PSE   (1u << 3)     /* 4 MiB pages */
#define CPU_TSC   (1u << 4)     /* rdtsc cycle counter */
#define CPU_MSR   (1u << 5)     /* rdmsr / wrmsr */
#define CPU_APIC  (1u << 9)     /* a local APIC (MSI, its timer) */
#define CPU_SEP   (1u << 11)    /* sysenter / sysexit (fast syscalls) */
#define CPU_PGE   (1u << 13)    /* global pages (survive a CR3 reload) */
#define CPU_CMOV  (1u << 15)
#define CPU_PAT   (1u << 16)    /* page attribute table (write-combining pages) */
#define CPU_PSE36 (1u << 17)    /* 4 MiB pages can reach physical > 4 GiB */
#define CPU_FXSR  (1u << 24)    /* fxsave / fxrstor */
#define CPU_SSE   (1u << 25)
#define CPU_SSE2  (1u << 26)

/* The cpuid instruction: leaf (EAX) and sub-leaf (ECX) in, EAX..EDX out
   (r[0..3]). /proc/cpuinfo is made from it. */
void cpu_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]);

/* What MyOS can't run without: a Pentium Pro / Pentium II or later.
   (CMOV: the kernel is compiled for i686, so the compiler uses it.) */
#define CPU_REQUIRED (CPU_FPU | CPU_PSE | CPU_TSC | CPU_MSR | CPU_CMOV)

extern uint32_t cpu_features;

static inline int cpu_has(uint32_t feature) { return (cpu_features & feature) != 0; }

/* Detect the CPU, print what it is, stop if it's too old. Call first. */
void cpu_init(void);

/* Model-specific registers: CPU settings outside the normal registers. */
static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                      "d"((uint32_t)(value >> 32)));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* The same for an MSR that may not be there: a processor (or a virtual
   machine) without it raises a general-protection fault, and interrupts.c
   finds the instruction in the exception table and continues after it, as
   it does for a bad user pointer in copy_from_user. 0, or -1 (*value = 0)
   if the MSR is missing. For MSRs CPUID doesn't promise (power settings
   whose MSRs vary by model). The xor that says "no fault" is skipped by a
   fault, so no separate fixup code is needed. */
#define EX_TABLE_ENTRY(insn, fixup) \
    ".pushsection __ex_table, \"a\"\n\t" \
    ".long " insn ", " fixup "\n\t" \
    ".popsection\n\t"
static inline int rdmsr_safe(uint32_t msr, uint64_t *value)
{
    uint32_t lo = 0, hi = 0;
    int err = -1;
    __asm__ volatile ("1: rdmsr\n\t"
                      "xor %[err], %[err]\n"
                      "2:\n\t"
                      EX_TABLE_ENTRY("1b", "2b")
                      : [err] "+r"(err), "+a"(lo), "+d"(hi) : "c"(msr));
    *value = ((uint64_t)hi << 32) | lo;
    return err;
}

static inline int wrmsr_safe(uint32_t msr, uint64_t value)
{
    int err = -1;
    __asm__ volatile ("1: wrmsr\n\t"
                      "xor %[err], %[err]\n"
                      "2:\n\t"
                      EX_TABLE_ENTRY("1b", "2b")
                      : [err] "+r"(err)
                      : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                      : "memory");
    return err;
}

/* The kernel config's processor power savings (cpupower.c): once, when the
   config is final (after storage_init). */
void cpupower_init(void);

/* The platform profile: 0 low-power, 1 balanced, 2 performance (cpupower.c;
   /sys/firmware/acpi/platform_profile). Setting one: 0, or -1. */
int cpupower_profile(void);
int cpupower_set_profile(int p);

/* Sleep until an interrupt: interrupts off on entry, and off again after.
   HLT, or MWAIT into a deeper idle state if the config asked for one. */
void cpu_sleep(void);

/* The processor's energy meter (RAPL): joules used so far, in microjoules,
   wrapping at *range_uj. 0, or -1: there is none. */
int cpu_energy_uj(uint64_t *uj, uint64_t *range_uj);

/* Page attribute table (pat.c): make write-combining available to page
   tables. Called by cpu_init. */
void pat_init(void);

/* Restart the CPU the hard way: with no interrupt table, any interrupt is
   a "triple fault", and the CPU resets itself. The last resort for a reboot. */
void cpu_reset(void) __attribute__((noreturn));

/* Interrupts off, stop for good. */
void cpu_halt_forever(void) __attribute__((noreturn));

#endif
