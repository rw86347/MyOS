#ifndef LINUX_IO_H
#define LINUX_IO_H

#include <stdint.h>

/*
 * Device memory, with Linux's names (include/linux/io.h), for drivers.
 *
 * ioremap: map a device's registers into the kernel, uncached — every read
 * and write reaches the device, in order.
 * ioremap_wc: map device memory "write-combining" — the CPU gathers writes
 * into bursts and may reorder them, which is right for a framebuffer (and
 * several times faster than uncached) and wrong for registers.
 *
 * Mappings are made at boot and never undone (no iounmap yet).
 *
 * Note: Linux's port I/O is outb(value, port); MyOS's <io.h> has the
 * opposite order, outb(port, value). Swap the arguments when porting.
 */
volatile void *ioremap(uint32_t phys, uint32_t size);
volatile void *ioremap_wc(uint32_t phys, uint32_t size);

static inline uint8_t readb(const volatile void *a) { return *(const volatile uint8_t *)a; }
static inline uint16_t readw(const volatile void *a) { return *(const volatile uint16_t *)a; }
static inline uint32_t readl(const volatile void *a) { return *(const volatile uint32_t *)a; }
static inline void writeb(uint8_t v, volatile void *a) { *(volatile uint8_t *)a = v; }
static inline void writew(uint16_t v, volatile void *a) { *(volatile uint16_t *)a = v; }
static inline void writel(uint32_t v, volatile void *a) { *(volatile uint32_t *)a = v; }

#endif
