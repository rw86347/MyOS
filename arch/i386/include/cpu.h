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
#define CPU_SEP   (1u << 11)    /* sysenter / sysexit (fast syscalls) */
#define CPU_PGE   (1u << 13)    /* global pages (survive a CR3 reload) */
#define CPU_CMOV  (1u << 15)
#define CPU_PAT   (1u << 16)    /* page attribute table (write-combining pages) */
#define CPU_FXSR  (1u << 24)    /* fxsave / fxrstor */
#define CPU_SSE   (1u << 25)
#define CPU_SSE2  (1u << 26)

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

/* Page attribute table (pat.c): make write-combining available to page
   tables. Called by cpu_init. */
void pat_init(void);

/* Restart the CPU the hard way: with no interrupt table, any interrupt is
   a "triple fault", and the CPU resets itself. The last resort for a reboot. */
void cpu_reset(void) __attribute__((noreturn));

/* Interrupts off, stop for good. */
void cpu_halt_forever(void) __attribute__((noreturn));

#endif
