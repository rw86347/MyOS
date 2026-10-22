/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * cpupower_test.c — rdmsr_safe / wrmsr_safe (cpu.h): an MSR that isn't
 * there must come back as an error, not stop the kernel. Only compiled
 * into `make test` builds.
 *
 * QEMU's emulated processor never faults on an unknown MSR (it reads 0
 * and ignores writes), so the test uses one it does fault on, as real
 * processors do: the x2APIC's registers (MSR 0x800 on) while the local
 * APIC is in its older xAPIC mode. The x2APIC ID (0x802) is read-only in
 * both modes, so writing it always faults.
 */
#include <cpu.h>
#include <selftest.h>

#define MSR_APIC_BASE  0x1B
#define BASE_X2APIC    (1u << 10)
#define MSR_X2APIC_ID  0x802

void cpupower_selftest(void)
{
    uint64_t base, v = 1;
    check(rdmsr_safe(MSR_APIC_BASE, &base) == 0 && base == rdmsr(MSR_APIC_BASE),
          "rdmsr_safe reads an MSR that is there");
    int r = rdmsr_safe(MSR_X2APIC_ID, &v);
    if (base & BASE_X2APIC)
        check(r == 0, "rdmsr_safe: the x2APIC ID, in x2APIC mode");
    else
        check(r == -1 && v == 0, "rdmsr_safe: a missing MSR faults, and returns -1 and 0");
    check(wrmsr_safe(MSR_X2APIC_ID, 0) == -1, "wrmsr_safe: a read-only MSR faults, and returns -1");
}
