/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * Pick early UART MMIO base from the boot DTB (Apple s5l / harness PL011 / virt).
 */
#include "apple/machine.h"
#include "apple/uart_early.h"

#include "fdt.h"
#include "uart.h"

#define PL011_VIRT 0x09000000u

int arm64_early_console_mmio(uintptr_t dtb_phys, uint64_t *base_out, uint64_t *size_out)
{
    uint64_t base = 0;
    uint64_t size = 0;

    if (arm64_platform_from_dtb(dtb_phys) == ARM64_PLAT_APPLE &&
        fdt_mmio_by_compatible(dtb_phys, "apple,s5l-uart", &base, &size) == 0)
        goto ok;
    if (arm64_platform_from_dtb(dtb_phys) == ARM64_PLAT_APPLE &&
        fdt_mmio_by_compatible(dtb_phys, "apple,uart", &base, &size) == 0)
        goto ok;
    if (fdt_mmio_by_compatible(dtb_phys, "arm,pl011", &base, &size) == 0 ||
        fdt_mmio_by_compatible(dtb_phys, "arm,primecell", &base, &size) == 0)
        goto ok;
    return -1;
ok:
    if (base_out)
        *base_out = base;
    if (size_out)
        *size_out = size ? size : 0x1000u;
    return 0;
}

void arm64_uart_init_from_dtb(uintptr_t dtb_phys)
{
    uint64_t base = 0;

    if (arm64_platform_from_dtb(dtb_phys) == ARM64_PLAT_APPLE &&
        fdt_mmio_by_compatible(dtb_phys, "apple,s5l-uart", &base, NULL) == 0) {
        uart_use_apple_s5l((uintptr_t)base);
        return;
    }
    if (arm64_platform_from_dtb(dtb_phys) == ARM64_PLAT_APPLE &&
        fdt_mmio_by_compatible(dtb_phys, "apple,uart", &base, NULL) == 0) {
        uart_set_base((uintptr_t)base);
        return;
    }
    if (fdt_mmio_by_compatible(dtb_phys, "arm,pl011", &base, NULL) == 0 ||
        fdt_mmio_by_compatible(dtb_phys, "arm,primecell", &base, NULL) == 0) {
        uart_set_base((uintptr_t)base);
        return;
    }
    uart_set_base(PL011_VIRT);
}
