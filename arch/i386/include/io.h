/*
 * io.h — x86 port I/O
 *
 * Besides normal memory, x86 CPUs have a separate 64K "I/O port" address
 * space used by legacy devices (serial ports, PIC, PIT, keyboard, ...).
 * It is reached only with the IN and OUT instructions, which C cannot emit
 * by itself, so we wrap them in tiny inline-assembly functions.
 */
#ifndef ARCH_I386_IO_H
#define ARCH_I386_IO_H

#include <stdint.h>

/* Write one byte to an I/O port. */
static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* Read one byte from an I/O port. */
static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

#endif
