/*
 * main.c — the kernel's C entry point
 *
 * By the time we get here:
 *   BIOS -> MBR (real mode) -> loaded kernel, enabled A20, entered protected
 *   mode -> entry.asm cleared .bss -> kernel_main()
 */
#include <disk_demo.h>
#include <kprintf.h>
#include <serial.h>
#include <vga.h>

void kernel_main(void)
{
    serial_init();
    vga_clear();

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("Hello world from MyOS! (32-bit protected mode, booted by our own MBR)\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    disk_demo();

    /* Nothing else to do yet. Halt the CPU; with interrupts off it stays put. */
    for (;;)
        __asm__ volatile ("cli; hlt");
}
