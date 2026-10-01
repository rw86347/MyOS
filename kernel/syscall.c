/*
 * syscall.c — the Linux system calls, and the table that finds them
 *
 * A user program can't call kernel functions directly (it's in ring 3 and
 * kernel pages are off-limits). Instead it puts a number in EAX and runs
 * `int 0x80`. The CPU jumps into the kernel through the IDT, and we look the
 * number up in syscall_table[].
 *
 * Rule #1 of syscalls: never trust a pointer from user space. A program can
 * pass any number as a "buffer" — including a kernel address, hoping we'll
 * read or write kernel memory for it. vmm_user_ok() checks that every page
 * of the buffer is mapped AND marked as user memory.
 */
#include <kprintf.h>
#include <paging.h>
#include <syscall.h>
#include <thread.h>

typedef int32_t (*syscall_fn)(uint32_t, uint32_t, uint32_t,
                              uint32_t, uint32_t, uint32_t);

/* ssize_t write(int fd, const void *buf, size_t count)
   No files yet: fd 1 (stdout) and 2 (stderr) go to the console. */
static int32_t sys_write(uint32_t fd, uint32_t buf, uint32_t count,
                         uint32_t a4, uint32_t a5, uint32_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (fd != 1 && fd != 2)
        return -EBADF;
    if (!vmm_user_ok(buf, count, 0))
        return -EFAULT;
    const char *p = (const char *)buf;
    for (uint32_t i = 0; i < count; i++)
        kprintf("%c", p[i]);
    return count;
}

/* void exit(int status) — and exit_group, which ends all threads of a
   process. With one thread per program, they're the same. */
static int32_t sys_exit(uint32_t status, uint32_t a2, uint32_t a3,
                        uint32_t a4, uint32_t a5, uint32_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    thread_current()->exit_code = status & 0xFF;    /* Linux keeps 8 bits */
    thread_exit();                                  /* never returns */
    return 0;
}

static const syscall_fn syscall_table[] = {
    [NR_exit]       = sys_exit,
    [NR_write]      = sys_write,
    [NR_exit_group] = sys_exit,
};

#define NSYSCALLS (sizeof syscall_table / sizeof syscall_table[0])

int32_t syscall_dispatch(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3,
                         uint32_t a4, uint32_t a5, uint32_t a6)
{
    if (nr < NSYSCALLS && syscall_table[nr])
        return syscall_table[nr](a1, a2, a3, a4, a5, a6);

    /* Not implemented. Log it to serial, so running real programs shows
       us which syscalls to write next (docs/DESIGN.md). */
    klog("syscall %lu not implemented (%s)\n", nr, thread_current()->name);
    return -ENOSYS;
}
