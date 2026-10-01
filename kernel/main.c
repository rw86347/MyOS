/*
 * main.c — the kernel's C entry point
 *
 * By the time we get here:
 *   BIOS -> MBR -> stage 2 (real mode: memory map, kernel + initramfs loaded
 *   above 1 MiB, protected mode) -> entry.asm (cleared .bss, turned on
 *   paging, jumped to the higher half at 0xC0000000) -> kernel_main()
 *
 * Three kinds of build (see the Makefile):
 *   make         a normal boot: no test or benchmark code at all
 *   make test    SELFTEST: runs the self-tests, then the normal loop
 *   make bench   BENCH: runs the benchmarks, then switches QEMU off
 */
#include <cpu.h>
#include <cycles.h>
#include <fpu.h>
#include <gdt.h>
#include <initrd.h>
#include <interrupts.h>
#include <kheap.h>
#include <keyboard.h>
#include <kprintf.h>
#include <memlayout.h>
#include <paging.h>
#include <pmm.h>
#include <sched.h>
#include <serial.h>
#include <sysenter.h>
#include <thread.h>
#include <timer.h>
#include <vga.h>
#ifdef SELFTEST
#include <selftest.h>
#endif
#ifdef BENCH
#include <bench.h>
#endif

extern char _start[];       /* first byte of the kernel (entry.asm) */

/* Show "up 0:01:23" in the top-right corner of the screen. */
static void show_uptime(uint32_t secs)
{
    char s[] = "up 0:00:00";
    uint32_t h = secs / 3600, m = secs / 60 % 60, sec = secs % 60;
    s[3] = '0' + h % 10;
    s[5] = '0' + m / 10;   s[6] = '0' + m % 10;
    s[8] = '0' + sec / 10; s[9] = '0' + sec % 10;
    vga_write_at(80 - sizeof s + 1, 0, s, VGA_BLACK, VGA_LIGHT_GREY);
}

/* Echo typed keys. Control characters are shown as ^C, ^D, ... */
static void echo(int c)
{
    if (c == '\n')
        kprintf("\n> ");
    else if (c == '\b')
        kprintf("\b");
    else if (c == '\t')
        kprintf(" ");
    else if (c < 32)
        kprintf("^%c", c + '@');
    else
        kprintf("%c", c);
}

void kernel_main(void)
{
#ifdef BENCH
    uint64_t boot_start = cycles_now();     /* for the boot-time benchmark */
#endif
    serial_init();
    vga_clear();
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("Hello world from MyOS!  (protected mode, own boot loader)\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    cpu_init();             /* what can this CPU do? (stops if too old) */

    /* CPU tables and interrupts. */
    gdt_init();             /* our own segment table + TSS */
    idt_init();             /* interrupt table + exception handlers + PIC remap */
    serial_irq_init();      /* serial output now queued, sent by IRQ 4 */
    fpu_init();             /* x87 + SSE on; saved per user thread */

    /* Memory. */
    pmm_init();             /* which physical RAM is free (BIOS memory map) */
    vmm_init();             /* map all RAM with 4 MiB pages; unmap NULL */
    sysenter_init();        /* fast syscalls, if the CPU has sysenter */
    timer_init();           /* IRQ 0: 1 ms tick */
    keyboard_init();        /* IRQ 1 */
    interrupts_enable();    /* sti: from now on, IRQs can arrive */

    kprintf("Memory: %lu MiB usable, paging on, kernel at %p, heap at %p\n",
            pmm_usable_count() / 256, (void *)_start, (void *)KHEAP_START);

    initrd_init();          /* the initramfs stage 2 loaded */
    thread_init();          /* kernel_main becomes thread 0, "main" */

#if defined(SELFTEST)
    run_selftests();        /* ends with the slot scheduler running */
    kprintf("Type here (F10/F11/F12 = test a CPU fault)> ");
#elif defined(BENCH)
    run_benchmarks(boot_start);     /* switches QEMU off when done */
#else
    sched_start();          /* the 20 x 1 ms slot scheduler */
    kprintf("Type here> ");
#endif

    /* Main loop: sleep until an interrupt wakes us (timer tick or key),
       then handle whatever is new. main owns the system slots 1 and 10. */
    uint32_t last_secs = 0xFFFFFFFF;
    for (;;) {
        cpu_wait();

        uint32_t secs = timer_ms() / 1000;
        if (secs != last_secs) {
            show_uptime(secs);
            last_secs = secs;
        }

        int c;
        while ((c = keyboard_getc()) != -1) {
#ifdef SELFTEST
            if (selftest_key(c))    /* F10/F11/F12: crash on purpose */
                continue;
#endif
            echo(c);
        }
    }
}
