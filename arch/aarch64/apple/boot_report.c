/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * boot_report.c — log Linux-style boot registers for m1n1 / hardware bring-up
 */
#include "apple/m1n1.h"

#include <stddef.h>
#include "apple/machine.h"

#include <kprintf.h>

void arm64_boot_report(uint64_t dtb_from_x0, uintptr_t dtb_used)
{
    char plat[96];
    const char *via;

    arm64_platform_label(dtb_used, plat, sizeof plat);
    if (dtb_from_x0 && dtb_from_x0 == (uint64_t)dtb_used)
        via = "x0";
    else
        via = "embedded";
    kprintf("  Boot: x0=0x%llx dtb=0x%llx (%s) platform=%s\n",
            (unsigned long long)dtb_from_x0, (unsigned long long)dtb_used, via, plat);
}
