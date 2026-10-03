#ifndef ARCH_I386_UACCESS_H
#define ARCH_I386_UACCESS_H

/*
 * Host stand-in for arch/i386/include/uaccess.h. The real one does a range
 * check against KERNEL_VBASE and then a copy with exception-table fixups,
 * neither of which means anything in a host process.
 *
 * net/tcp.c only reaches these when a socket passes user=1, which the fuzz
 * targets never do; they exist so the file compiles. Pointers here are real
 * host pointers, so the copy is a plain memcpy and ASan guards both ends.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define USER_LIMIT 0xC0000000u

static inline int user_range_ok(uint32_t addr, uint32_t n)
{
    (void)addr; (void)n;
    return 1;
}

static inline int copy_from_user(void *dst, const void *user_src, uint32_t n)
{
    memcpy(dst, user_src, n);
    return 0;
}

static inline int copy_to_user(void *user_dst, const void *src, uint32_t n)
{
    memcpy(user_dst, src, n);
    return 0;
}

/* Nothing is paged out on the host, so there is nothing to fault in. */
static inline int fault_in_user(const void *user, uint32_t n, int write)
{
    (void)user; (void)n; (void)write;
    return 0;
}

static inline int get_user32(uint32_t *dst, uint32_t user_addr)
{
    (void)dst; (void)user_addr;
    return -EFAULT;
}

static inline long strncpy_from_user(char *dst, const char *user_src, long max)
{
    (void)dst; (void)user_src; (void)max;
    return -EFAULT;
}

#endif
