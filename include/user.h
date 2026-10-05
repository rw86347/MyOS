#ifndef USER_H
#define USER_H

#include <stdint.h>

/*
 * User virtual addresses. On i386 they fit in 32 bits; on x86_64 musl uses
 * the full lower canonical range.
 */
#ifdef ARCH_X86_64
typedef uint64_t user_addr_t;
#define USER_ADDR_MAX     0x00007FFFFFFFFFFFULL
#define USER_STACK_TOP    0x00007FFFFFFFE000ULL
#define USER_STACK_MAX    (8ull << 20)
#define MMAP_BASE         0x000000400000ULL
#define ELF_DYN_BASE      0x0000555555540000ULL
#else
typedef uint32_t user_addr_t;
#define USER_ADDR_MAX     0xBFFFFFFFull
#define USER_STACK_TOP    0xBFFFF000u
#define USER_STACK_MAX    (8u << 20)
#define MMAP_BASE         0x40000000u
#define ELF_DYN_BASE      0x00400000u
#endif

#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_MAX)

#endif
