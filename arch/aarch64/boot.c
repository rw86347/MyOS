/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * boot.c — ARM64 bring-up (step 14a, one milestone at a time)
 */
#include "exceptions.h"
#include "fdt.h"
#include "arm64_pmm.h"
#include "paging.h"
#include "arm64_timer.h"
#include "uart.h"

#include <kheap.h>
#include <kprintf.h>

#include "syscall.h"

extern char myos_dtb_start[];
extern char myos_dtb_end[];

static void stage(const char *name)
{
    uart_puts("Step - ");
    uart_puts(name);
    uart_puts("\n");
}

static void launch_init(void);

void kernel_main(unsigned long dtb_from_boot)
{
    unsigned long dtb = dtb_from_boot ? dtb_from_boot : (unsigned long)myos_dtb_start;
    (void)myos_dtb_end;
    uint64_t ram_base = 0;
    uint64_t ram_size = 0;

    uint64_t el;

    uart_init();
    uart_puts("MyOS aarch64 (QEMU virt)\n");
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    kprintf("  CPU: CurrentEL=%u\n", (unsigned)((el >> 2) & 3u));

    stage("Exceptions");
    exceptions_init();

    stage("Device tree");
    if (fdt_check_header(dtb) < 0) {
        uart_puts("  FDT: no valid DTB at 0x");
        uart_puthex(dtb);
        uart_puts(" (embedded virt.dtb missing?)\n");
    } else {
        uart_puts("  FDT: magic ok\n");
        if (fdt_first_ram(dtb, &ram_base, &ram_size) == 0) {
            uart_puts("  RAM: 0x");
            uart_puthex(ram_base);
            uart_puts(" + ");
            uart_puthex(ram_size);
            uart_puts(" bytes\n");
        } else {
            ram_base = 0x40000000UL;
            ram_size = 256UL * 1024 * 1024;
            uart_puts("  RAM: parse failed (virt fallback)\n");
        }
    }

    stage("Memory");
    arm64_pmm_init(dtb);
    uart_puts("  PMM: ");
    uart_puthex(arm64_pmm_free_frames());
    uart_puts(" free of ");
    uart_puthex(arm64_pmm_total_frames());
    uart_puts(" frames\n");

    stage("Paging");
    paging_init(ram_base, ram_size);

    stage("Timer");
    timer_init();
    arm64_irq_enable();
    while (timer_ms() < 5u)
        __asm__ volatile("wfe");
    uart_puts("  Timer: ");
    uart_puthex(timer_ms());
    uart_puts(" ms ticks (1 kHz)\n");

    stage("Heap");
    {
        void *p = kmalloc(128);
        uint32_t used, free_b, mapped;

        kheap_stats(&used, &free_b, &mapped);
        kprintf("  kheap: kmalloc(128)=%p pages=%u used=%u\n", p, (unsigned)kheap_pages(),
                (unsigned)used);
        kfree(p);
    }

    uart_puts("Hello world from MyOS!  (aarch64, QEMU virt)\n");

    stage("EL0");
    el0_set_resume((void *)launch_init);
    el0_run_databtest();
    el0_enter_user();

    for (;;)
        __asm__ volatile("wfe");
}

static void launch_init(void)
{
    uart_puts("Step - Initramfs\n");
    if (el0_run_initrd() < 0) {
        for (;;)
            __asm__ volatile("wfe");
    }
    el0_enter_user();
    for (;;)
        __asm__ volatile("wfe");
}
