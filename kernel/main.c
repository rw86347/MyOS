/*
 * main.c — the kernel's C entry point
 *
 * By the time we get here:
 *   BIOS -> MBR (real mode) -> loaded kernel, enabled A20, entered protected
 *   mode -> entry.asm cleared .bss -> kernel_main()
 */
#include <serial.h>
#include <vga.h>

void kernel_main(void)
{
    serial_init();
    serial_write("Hello world from MyOS! (serial)\n");

    vga_clear();
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("Hello world from MyOS!\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_write("Booted via our own MBR, now running in 32-bit protected mode.\n");

    /* Nothing else to do yet. Halt the CPU; with interrupts off it stays put. */
    for (;;)
        __asm__ volatile ("cli; hlt");
}
