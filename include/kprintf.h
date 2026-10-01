#ifndef KPRINTF_H
#define KPRINTF_H

/*
 * kprintf — the kernel's printf. Output goes to both the VGA screen and the
 * serial port. Supports: %s %c %d %u %x %p %%, with an optional width and
 * '0' padding (e.g. %8u, %08x) and '-' for left-align (e.g. %-12s).
 * Use %lu / %lx for uint32_t (it is "unsigned long" with i686-elf-gcc).
 */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Same, but to the serial port only: for detail that would clutter the
   25-line screen (memory maps, debug output). */
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Write n bytes to the screen and the serial port, as one piece. */
#include <stddef.h>
void console_write(const char *s, size_t n);

#endif
