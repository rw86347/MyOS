#ifndef ARCH_I386_UACCESS_H
#define ARCH_I386_UACCESS_H

#include <errno.h>
#include <memlayout.h>
#include <stdint.h>

/*
 * The only ways the kernel touches user memory (see uaccess.asm for how
 * faults are caught). Each returns 0 or -EFAULT.
 *
 * First a range check: the whole range must lie below KERNEL_VBASE, or a
 * program could ask us to copy kernel memory. That's two compares — no
 * page-table walk.
 */

#define USER_LIMIT KERNEL_VBASE

int  __copy_user(void *dst, const void *src, uint32_t n);
int  __get_user32(uint32_t *dst, uint32_t user_addr);
long __strncpy_user(char *dst, const char *user_src, long max);

static inline int user_range_ok(uint32_t addr, uint32_t n)
{
    return n <= USER_LIMIT && addr <= USER_LIMIT - n;
}

static inline int copy_from_user(void *dst, const void *user_src, uint32_t n)
{
    if (!user_range_ok((uint32_t)user_src, n))
        return -EFAULT;
    return __copy_user(dst, user_src, n);
}

static inline int copy_to_user(void *user_dst, const void *src, uint32_t n)
{
    if (!user_range_ok((uint32_t)user_dst, n))
        return -EFAULT;
    return __copy_user(user_dst, src, n);
}

static inline int get_user32(uint32_t *dst, uint32_t user_addr)
{
    if (!user_range_ok(user_addr, 4))
        return -EFAULT;
    return __get_user32(dst, user_addr);
}

/* Copy a user string (e.g. a file name) into dst[max]. Returns its length,
   -EFAULT, or -ENAMETOOLONG if it doesn't fit. */
static inline long strncpy_from_user(char *dst, const char *user_src, long max)
{
    uint32_t a = (uint32_t)user_src;
    if (a >= USER_LIMIT)
        return -EFAULT;
    long room = max;
    if ((uint32_t)room > USER_LIMIT - a)
        room = USER_LIMIT - a;          /* stop at the end of user space */
    long n = __strncpy_user(dst, user_src, room);
    if (n >= 0 && n == room)
        return room == max ? -ENAMETOOLONG : -EFAULT;
    return n;
}

#endif
