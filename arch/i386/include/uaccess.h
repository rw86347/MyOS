#ifndef ARCH_I386_UACCESS_H
#define ARCH_I386_UACCESS_H

#include <errno.h>
#include <memlayout.h>
#include <stdint.h>
#include <user.h>

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
int  __fault_in_user(uint32_t addr, uint32_t n, int write);
long __strncpy_user(char *dst, const char *user_src, long max);

static inline int user_range_ok(uint32_t addr, uint32_t n)
{
    return n <= USER_LIMIT && addr <= USER_LIMIT - n;
}

/* `make test` builds can make a copy sleep on purpose, to produce the
   interleaving the fixes in docs/security.md are about. Absent from every
   other build: no branch, no symbol. */
#ifdef SELFTEST
#include <uaccess_test.h>
#define UACCESS_MAY_SLEEP() uaccess_test_pause()
#else
#define UACCESS_MAY_SLEEP() ((void)0)
#endif

static inline int copy_from_user(void *dst, const void *user_src, uint32_t n)
{
    if (!user_range_ok((uint32_t)user_src, n))
        return -EFAULT;
    UACCESS_MAY_SLEEP();
    return __copy_user(dst, user_src, n);
}

static inline int copy_to_user(void *user_dst, const void *src, uint32_t n)
{
    if (!user_range_ok((uint32_t)user_dst, n))
        return -EFAULT;
    UACCESS_MAY_SLEEP();
    return __copy_user(user_dst, src, n);
}

/*
 * Make a later copy of this range unable to fault, and so unable to sleep.
 *
 * A copy_to_user or copy_from_user is not a quiet instruction sequence: if
 * the page is a not-yet-read page of an mmap'd file, the fault goes to the
 * page cache, which waits for the disk, and another thread runs. So a syscall
 * that reads a shared counter, copies, and then writes the counter back is
 * one where two processes can act on the same value — the bug that was found
 * in the pipe, socket and TCP rings (docs/security.md, 2026-10-03).
 *
 * Call this *before* looking at any shared state. Sleeping here costs nothing
 * because nothing has been claimed yet, and after it returns the range is
 * resident and writable, so the copy is just a copy.
 *
 * `write` is from the kernel's point of view: 1 when the kernel will write
 * into the range (a read() buffer), because a page shared copy-on-write with
 * a fork'd child has to be unshared now rather than during the copy.
 *
 * What this is worth, exactly. It removes the sleep that happens with no
 * attacker present — the first touch of a page of an mmap'd file, which is
 * the ordinary case and was the one making pipe and socket reads racy. Page
 * cache reclaim never unmaps user pages, so nothing in the kernel undoes it.
 *
 * It is NOT a guarantee. clone(CLONE_THREAD) gives one address space several
 * threads, and a sibling thread can munmap the buffer and map a file over the
 * same address. It can only run while we are asleep, and after this returns
 * we do not sleep — but this function itself sleeps, so a thread woken during
 * the probe of a later page can pull an earlier one out from under us, and
 * the copy then faults and sleeps after all.
 *
 * So a site whose consequence is memory-unsafe (an out-of-bounds write, a
 * use-after-free) must still be correct across a sleep by construction:
 * commit the ring index before copying, or hold a reference on the object.
 * This makes the race improbable; it does not make the site safe.
 */
static inline int fault_in_user(const void *user, uint32_t n, int write)
{
    if (!user_range_ok((uint32_t)user, n))
        return -EFAULT;
    return __fault_in_user((uint32_t)user, n, write);
}

static inline int get_user32(uint32_t *dst, uint32_t user_addr)
{
    if (!user_range_ok(user_addr, 4))
        return -EFAULT;
    return __get_user32(dst, user_addr);
}

/* One user word the size of a pointer: an entry of argv, a field of an
   iovec. 4 bytes here, 8 on x86_64. */
static inline int get_user_word(user_addr_t *dst, uintptr_t user_addr)
{
    return get_user32(dst, user_addr);
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
