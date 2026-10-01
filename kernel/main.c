/*
 * main.c — the kernel's C entry point
 *
 * By the time we get here:
 *   BIOS -> MBR (real mode: loaded kernel, read memory map, enabled A20,
 *   entered protected mode) -> entry.asm (cleared .bss, turned on paging,
 *   jumped to the higher half at 0xC0000000) -> kernel_main()
 */
#include <disk_demo.h>
#include <fpu.h>
#include <gdt.h>
#include <initrd.h>
#include <interrupts.h>
#include <kheap.h>
#include <keyboard.h>
#include <kprintf.h>
#include <memlayout.h>
#include <mem_test.h>
#include <paging.h>
#include <pmm.h>
#include <sched_test.h>
#include <selftest.h>
#include <serial.h>
#include <thread_test.h>
#include <timer.h>
#include <user_test.h>
#include <vga.h>

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

/* Deliberately crash, to show the exception handlers work. */
static void trigger_fault(int key)
{
    if (key == KEY_F10) {
        kprintf("\n[F10] Reading through a NULL pointer...\n");
        volatile uint32_t *null = 0;
        kprintf("%lu\n", *null);           /* page 0 is not mapped */
    } else if (key == KEY_F12) {
        kprintf("\n[F12] Dividing by zero...\n");
        /* Written in assembly on purpose: in C, dividing by zero is
           "undefined behaviour", so GCC may assume it never happens and
           replace `1 / x` with a comparison — no divide, no fault. */
        __asm__ volatile ("mov $1, %%eax\n\t"
                          "xor %%edx, %%edx\n\t"
                          "xor %%ecx, %%ecx\n\t"
                          "div %%ecx"           /* EDX:EAX / 0 -> exception 0 */
                          : : : "eax", "ecx", "edx");
    } else {
        kprintf("\n[F11] Loading an invalid segment selector...\n");
        __asm__ volatile ("mov $0x99, %%ax\n\tmov %%ax, %%ds" : : : "eax");
    }
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
    serial_init();
    vga_clear();
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("Hello world from MyOS!  (protected mode, own MBR)\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    /* Step 2: CPU tables and interrupts. */
    gdt_init();             /* our own segment table */
    idt_init();             /* interrupt table + exception handlers + PIC remap */
    fpu_init();             /* x87 + SSE on; saved per user thread */

    /* Step 3: memory. */
    pmm_init();             /* which physical RAM is free (BIOS memory map) */
    vmm_init();             /* page tables for all RAM; unmap NULL */
    timer_init();           /* IRQ 0: 1 ms tick */
    keyboard_init();        /* IRQ 1 */
    interrupts_enable();    /* sti: from now on, IRQs can arrive */

    uint32_t t0 = timer_ms();
    sleep_ms(100);
    kprintf("Interrupts on: timer %u Hz (sleep 100 ms = %lu ticks), keyboard ready\n",
            TIMER_HZ, timer_ms() - t0);

    kprintf("Memory: %lu MiB usable, paging on, kernel at %p, heap at %p\n",
            pmm_usable_count() / 256, (void *)_start, (void *)KHEAP_START);

    disk_demo();
    initrd_init();          /* Step 7: the initramfs stage 2 loaded */
    mem_selftest();
    thread_selftest();      /* Step 4: kernel threads + context switch */
    user_selftest();        /* Step 5: ring 3 programs, int 0x80 write/exit */
    sched_selftest();       /* Step 4: start the 20 x 1 ms slot scheduler */
    selftest_summary();

    kprintf("Type here (F10/F11/F12 = test a CPU fault)> ");

    /* Main loop: sleep until an interrupt wakes us (timer tick or key),
       then handle whatever is new. */
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
            if (c == KEY_F10 || c == KEY_F11 || c == KEY_F12)
                trigger_fault(c);
            else
                echo(c);
        }
    }
}
