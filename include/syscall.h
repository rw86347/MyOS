#ifndef SYSCALL_H
#define SYSCALL_H

#include <errno.h>
#include <stdint.h>

/*
 * Linux i386 system call numbers. They MUST match Linux exactly
 * (arch/x86/entry/syscalls/syscall_32.tbl), because that's what compiled
 * programs have baked in. Add them here as we implement them.
 */
#define NR_exit          1
#define NR_read          3
#define NR_write         4
#define NR_open          5
#define NR_close         6
#define NR_lseek        19
#define NR_getpid       20
#define NR__llseek     140
#define NR_exit_group  252
#define NR_openat      295
#define NR_statx       383

/* Arguments with Linux values */
#define AT_FDCWD      (-100)        /* "relative to the current directory" */
#define AT_EMPTY_PATH 0x1000        /* statx: the dirfd itself (fstat) */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Run system call `nr` with up to 6 arguments; returns the result for EAX.
   Called by the int 0x80 handler (interrupts.c). */
int32_t syscall_dispatch(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3,
                         uint32_t a4, uint32_t a5, uint32_t a6);

#endif
