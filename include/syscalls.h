#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>

/*
 * System call handlers that live outside syscall.c. Every handler gets the
 * six argument registers (EBX, ECX, EDX, ESI, EDI, EBP) and returns the
 * value for EAX: a result, or -errno.
 */
#define SYSCALL_ARG __attribute__((unused)) uint32_t
#define SYSCALL_DEF(name) \
    int32_t name(SYSCALL_ARG a1, SYSCALL_ARG a2, SYSCALL_ARG a3, \
                 SYSCALL_ARG a4, SYSCALL_ARG a5, SYSCALL_ARG a6)

/* kernel/sys_info.c */
void time_init(void);                   /* read the CMOS clock */
SYSCALL_DEF(sys_uname);
SYSCALL_DEF(sys_getid);                 /* getuid & co.: we're all root */
SYSCALL_DEF(sys_setid);
SYSCALL_DEF(sys_getgroups);
SYSCALL_DEF(sys_getresid);
SYSCALL_DEF(sys_time);
SYSCALL_DEF(sys_gettimeofday);
SYSCALL_DEF(sys_clock_gettime);
SYSCALL_DEF(sys_clock_gettime64);
SYSCALL_DEF(sys_clock_getres);
SYSCALL_DEF(sys_clock_getres64);
SYSCALL_DEF(sys_nanosleep);
SYSCALL_DEF(sys_clock_nanosleep);
SYSCALL_DEF(sys_clock_nanosleep64);
SYSCALL_DEF(sys_getrlimit);
SYSCALL_DEF(sys_setrlimit);
SYSCALL_DEF(sys_prlimit64);
SYSCALL_DEF(sys_sysinfo);
SYSCALL_DEF(sys_getrandom);
SYSCALL_DEF(sys_set_thread_area);
SYSCALL_DEF(sys_get_thread_area);
SYSCALL_DEF(sys_set_tid_address);

/* fs/poll.c */
SYSCALL_DEF(sys_poll);
SYSCALL_DEF(sys_ppoll64);
SYSCALL_DEF(sys_ppoll32);
SYSCALL_DEF(sys_pselect32);
SYSCALL_DEF(sys_select);
SYSCALL_DEF(sys_pselect64);

#endif
