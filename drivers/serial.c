/*
 * serial.c — COM1 serial port output (16550 UART)
 *
 * The serial port is the kernel developer's best friend: it works before
 * anything else, and QEMU can show it in your terminal (-serial stdio).
 * Real old PCs have a COM1 port too, and a null-modem cable lets another
 * computer capture the output.
 *
 * COM1's registers live at I/O ports 0x3F8..0x3FF.
 */
#include <io.h>
#include <serial.h>

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* disable UART interrupts (we poll instead) */
    outb(COM1 + 3, 0x80);   /* set DLAB bit: next two writes set the baud divisor */
    outb(COM1 + 0, 0x01);   /* divisor low byte:  115200 / 1 = 115200 baud */
    outb(COM1 + 1, 0x00);   /* divisor high byte */
    outb(COM1 + 3, 0x03);   /* 8 data bits, no parity, 1 stop bit ("8N1"); DLAB off */
    outb(COM1 + 2, 0xC7);   /* enable and clear the FIFOs */
    outb(COM1 + 4, 0x03);   /* DTR + RTS: tell the other side we're ready */
}

void serial_putc(char c)
{
    if (c == '\n')
        serial_putc('\r');  /* terminals want CR+LF for a new line */

    /* Wait until bit 5 of the Line Status Register says the transmit
       buffer is empty, then hand over the byte. */
    while ((inb(COM1 + 5) & 0x20) == 0)
        ;
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s)
{
    while (*s)
        serial_putc(*s++);
}
