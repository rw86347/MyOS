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

/* 16-bit versions (the ATA data register is 16 bits wide). */
static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Read `count` 16-bit words from a port into memory ("rep insw").
   One instruction moves a whole disk sector — much faster than a C loop. */
static inline void insw(uint16_t port, void *buf, uint32_t count)
{
    __asm__ volatile ("cld; rep insw"
                      : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}

/* Write `count` 16-bit words from memory to a port ("rep outsw"). */
static inline void outsw(uint16_t port, const void *buf, uint32_t count)
{
    __asm__ volatile ("cld; rep outsw"
                      : "+S"(buf), "+c"(count) : "d"(port) : "memory");
}

#endif
