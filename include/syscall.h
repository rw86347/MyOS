#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/*
 * Linux i386 system call numbers. They MUST match Linux exactly
 * (arch/x86/entry/syscalls/syscall_32.tbl), because that's what compiled
 * programs have baked in. Add them here as we implement them.
 */
#define NR_exit          1
#define NR_write         4
#define NR_exit_group  252

/* Linux errno values. A syscall returns -ERRNO on failure; the C library
   turns that into -1 and sets errno. */
#define EBADF    9      /* bad file descriptor */
#define EFAULT  14      /* bad address */
#define ENOSYS  38      /* function not implemented */

/* Run system call `nr` with up to 6 arguments; returns the result for EAX.
   Called by the int 0x80 handler (interrupts.c). */
int32_t syscall_dispatch(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3,
                         uint32_t a4, uint32_t a5, uint32_t a6);

#endif
