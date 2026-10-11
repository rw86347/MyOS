/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * cpu.c — what CPU are we on, and what can it do?
 *
 * CPUID is the instruction that answers. Leaf 0 gives the vendor string
 * ("GenuineIntel", "AuthenticAMD"), leaf 1 the family/model and a bitmap of
 * features (cpu.h). We check once at boot and remember the bitmap, so the
 * rest of the kernel can ask cheaply: cpu_has(CPU_SEP).
 *
 * Also here: the cycle counter's calibration (cycles.h).
 */
#include <cpu.h>
#include <cycles.h>
#include <interrupts.h>
#include <kprintf.h>
#include <string.h>

uint32_t cpu_features;
static uint32_t per_us;

void cpu_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile ("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                      : "a"(leaf), "c"(sub));
}

static void cpuid(uint32_t leaf, uint32_t r[4])
{
    cpu_cpuid(leaf, 0, r);
}

void cpu_init(void)
{
    uint32_t r[4];
    char vendor[13];

    cpuid(0, r);
    memcpy(&vendor[0], &r[1], 4);           /* the 12 characters come in */
    memcpy(&vendor[4], &r[3], 4);           /* EBX, EDX, ECX — in that order */
    memcpy(&vendor[8], &r[2], 4);
    vendor[12] = '\0';

    cpuid(1, r);
    uint32_t family = (r[0] >> 8) & 0xF, model = (r[0] >> 4) & 0xF;
    uint32_t stepping = r[0] & 0xF;
    cpu_features = r[3];

    /* A famous quirk: the Pentium Pro claims SEP (sysenter) but doesn't
       really have it. Intel's rule: ignore SEP on family 6, model < 3,
       stepping < 3. */
    if (family == 6 && model < 3 && stepping < 3)
        cpu_features &= ~CPU_SEP;

    klog("CPU: %s family %lu model %lu:%s%s%s%s%s%s%s%s\n", vendor, family, model,
            cpu_has(CPU_PSE) ? " pse" : "", cpu_has(CPU_PSE36) ? " pse36" : "",
            cpu_has(CPU_PGE) ? " pge" : "",
            cpu_has(CPU_SEP) ? " sysenter" : "", cpu_has(CPU_FXSR) ? " fxsr" : "",
            cpu_has(CPU_SSE) ? " sse" : "", cpu_has(CPU_SSE2) ? " sse2" : "",
            cpu_has(CPU_PAT) ? " pat" : "");

    if ((cpu_features & CPU_REQUIRED) != CPU_REQUIRED) {
        kprintf("This CPU is too old for MyOS (needs a Pentium Pro or later).\n");
        for (;;)
            __asm__ volatile ("cli; hlt");
    }
    pat_init();
}

/* Calibration runs in the background, so boot doesn't wait 10 ms for it:
   the timer interrupt calls cycles_tick() every millisecond, and we read
   the counter at tick 2 and tick 12. Starting on a tick edge means we
   measure whole ticks; the error is only the difference in interrupt
   latency at the two ends — far below 0.1%. */
static uint64_t cal_start;
static volatile int calibrated;

void cycles_tick(uint32_t ms)
{
    if (ms == 2) {
        cal_start = cycles_now();
    } else if (ms == 12) {
        per_us = (uint32_t)((cycles_now() - cal_start) / 10000);
        calibrated = 1;
    }
}

uint32_t cycles_per_us(void)
{
    while (!calibrated)         /* only the first ~12 ms after boot */
        cpu_wait();
    return per_us;
}

void cpu_reset(void)
{
    static const struct { uint16_t limit; uint32_t base; } __attribute__((packed))
        no_idt = { 0, 0 };
    __asm__ volatile ("cli; lidt %0; int3" : : "m"(no_idt));
    cpu_halt_forever();
}

void cpu_halt_forever(void)
{
    for (;;)
        __asm__ volatile ("cli; hlt");
}
