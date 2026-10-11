/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef USER_H
#define USER_H

#include <stdint.h>

/*
 * User virtual addresses. On i386 they fit in 32 bits; on x86_64 musl uses
 * the full lower canonical range.
 */
#ifdef ARCH_X86_64
/* The bottom half of the 48-bit space, like Linux (arch/x86_64/include/
   memlayout.h has the kernel's top half). It ends one page short of
   0x800000000000, as on Linux: a `syscall` in that last page would leave a
   non-canonical return address, which sysret cannot take. */
typedef uint64_t user_addr_t;
#define USER_ADDR_MIN     0x0000000000001000ULL   /* page 0 stays unmapped (NULL) */
#define USER_ADDR_END     0x00007FFFFFFFF000ULL
#define USER_ADDR_MAX     0x00007FFFFFFFEFFFULL
#define USER_STACK_TOP    0x00007FFFFFFFE000ULL
#define USER_STACK_MAX    (8ull << 20)
#define MMAP_BASE         0x00007F0000000000ULL   /* where Linux's libraries end up, too */
#define ELF_DYN_BASE      0x0000555555554000ULL   /* Linux's spot for a PIE program */
#else
typedef uint32_t user_addr_t;
#define USER_ADDR_MIN     0x00001000u     /* page 0 stays unmapped (NULL) */
#define USER_ADDR_END     0xC0000000u     /* the kernel starts here */
#define USER_ADDR_MAX     0xBFFFFFFFull
#define USER_STACK_TOP    0xBFFFF000u
#define USER_STACK_MAX    (8u << 20)
#define MMAP_BASE         0x40000000u
#define ELF_DYN_BASE      0x00400000u
#endif

#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_MAX)

/* Structs a program hands the kernel, as the program lays them out. Their
   pointer and size_t fields are user_addr_t, so one definition is right on
   both: i386 gets 4-byte fields, x86_64 8-byte ones (and the padding its C
   compiler puts after namelen and flags). */
struct user_iovec {                     /* struct iovec: 8 / 16 bytes */
    user_addr_t base, len;
};
struct user_msghdr {                    /* struct msghdr: 28 / 56 bytes */
    user_addr_t name;
    uint32_t namelen;
    user_addr_t iov, iovlen;
    user_addr_t control, controllen;
    int32_t flags;
};
struct user_cmsghdr {                   /* struct cmsghdr, the header part */
    user_addr_t len;
    int32_t level, type;
};

#endif
