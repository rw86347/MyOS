/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
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
volatile void *ioremap_wc64(uint64_t phys, uint32_t size);

static inline uint8_t readb(const volatile void *a) { return *(const volatile uint8_t *)a; }
static inline uint16_t readw(const volatile void *a) { return *(const volatile uint16_t *)a; }
static inline uint32_t readl(const volatile void *a) { return *(const volatile uint32_t *)a; }
static inline void writeb(uint8_t v, volatile void *a) { *(volatile uint8_t *)a = v; }
static inline void writew(uint16_t v, volatile void *a) { *(volatile uint16_t *)a = v; }
static inline void writel(uint32_t v, volatile void *a) { *(volatile uint32_t *)a = v; }

/* 64 bits: one access on x86_64; on i386 two, low half first (Linux's
   lo_hi order, which is what devices that allow it expect). */
static inline uint64_t readq(const volatile void *a)
{
#ifdef ARCH_X86_64
    return *(const volatile uint64_t *)a;
#else
    uint32_t lo = readl(a), hi = readl((const volatile uint8_t *)a + 4);
    return (uint64_t)hi << 32 | lo;
#endif
}
static inline void writeq(uint64_t v, volatile void *a)
{
#ifdef ARCH_X86_64
    *(volatile uint64_t *)a = v;
#else
    writel((uint32_t)v, a);
    writel((uint32_t)(v >> 32), (volatile uint8_t *)a + 4);
#endif
}
#define readb_relaxed(a)      readb(a)
#define readw_relaxed(a)      readw(a)
#define readl_relaxed(a)      readl(a)
#define writeb_relaxed(v, a)  writeb(v, a)
#define writew_relaxed(v, a)  writew(v, a)
#define writel_relaxed(v, a)  writel(v, a)
#define ioread8(a)            readb(a)
#define ioread16(a)           readw(a)
#define ioread32(a)           readl(a)
#define ioread64(a)           readq(a)
#define iowrite8(v, a)        writeb(v, a)
#define iowrite16(v, a)       writew(v, a)
#define iowrite32(v, a)       writel(v, a)
#define iowrite64(v, a)       writeq(v, a)
#define lo_hi_readq(a)        readq(a)
#define lo_hi_writeq(v, a)    writeq(v, a)
static inline void ioread32_rep(const volatile void *a, void *buf, unsigned long n)
{ uint32_t *b = buf; while (n--) *b++ = readl(a); }
static inline void iowrite32_rep(volatile void *a, const void *buf, unsigned long n)
{ const uint32_t *b = buf; while (n--) writel(*b++, a); }
static inline void memcpy_fromio(void *dst, const volatile void *src, unsigned long n)
{ uint8_t *d = dst; const volatile uint8_t *s = src; while (n--) *d++ = *s++; }
static inline void memcpy_toio(volatile void *dst, const void *src, unsigned long n)
{ volatile uint8_t *d = dst; const uint8_t *s = src; while (n--) *d++ = *s++; }
static inline void memset_io(volatile void *dst, int c, unsigned long n)
{ volatile uint8_t *d = dst; while (n--) *d++ = (uint8_t)c; }

#endif
