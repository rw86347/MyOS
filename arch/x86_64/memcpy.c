/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * memcpy.c — fast memory copy and fill for x86
 *
 * The kernel copies and clears whole pages all the time (page tables, ELF
 * loading, user stacks), so these are hot. x86 has "string instructions"
 * built for exactly this:
 *
 *   rep movsl   copy ECX 4-byte words from [ESI] to [EDI]
 *   rep stosl   store EAX into ECX 4-byte words at [EDI]
 *
 * One instruction does the whole loop, 4 bytes per step, and since the
 * Pentium Pro the CPU runs them as "fast strings" in hardware. We do the
 * bulk in 4-byte words and the last 0-3 bytes with movsb/stosb.
 *
 * (They need the direction flag clear — copying upward — which the kernel
 * keeps as its normal state; isr.asm clears it on every entry.)
 */
#include <stdint.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    void *d = dst;
    size_t words = n / 4, bytes = n % 4;
    __asm__ volatile ("rep movsl" : "+D"(d), "+S"(src), "+c"(words) : : "memory");
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(src), "+c"(bytes) : : "memory");
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    void *d = dst;
    uint32_t pattern = (uint8_t)c * 0x01010101u;    /* the byte in all 4 lanes */
    size_t words = n / 4, bytes = n % 4;
    __asm__ volatile ("rep stosl" : "+D"(d), "+c"(words) : "a"(pattern) : "memory");
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(bytes) : "a"(pattern) : "memory");
    return dst;
}

/* Like memcpy, but correct when the areas overlap. Copying upward is safe
   when dst is below src; otherwise copy downward (direction flag set, byte
   by byte — overlapping moves are rare, so this path stays simple). */
void *memmove(void *dst, const void *src, size_t n)
{
    if ((uintptr_t)dst <= (uintptr_t)src || (uintptr_t)dst >= (uintptr_t)src + n)
        return memcpy(dst, src, n);
    if (n == 0)
        return dst;
    void *d = (char *)dst + n - 1;
    const void *s = (const char *)src + n - 1;
    __asm__ volatile ("std\n\t"
                      "rep movsb\n\t"
                      "cld"
                      : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}
