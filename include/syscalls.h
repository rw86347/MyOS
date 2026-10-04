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
SYSCALL_DEF(sys_sethostname);
SYSCALL_DEF(sys_getuid);                /* users and groups (cred.h) */
SYSCALL_DEF(sys_geteuid);
SYSCALL_DEF(sys_getgid);
SYSCALL_DEF(sys_getegid);
SYSCALL_DEF(sys_setuid);
SYSCALL_DEF(sys_setgid);
SYSCALL_DEF(sys_setreuid);
SYSCALL_DEF(sys_setregid);
SYSCALL_DEF(sys_setresuid);
SYSCALL_DEF(sys_setresgid);
SYSCALL_DEF(sys_getresuid);
SYSCALL_DEF(sys_getresgid);
SYSCALL_DEF(sys_setfsuid);
SYSCALL_DEF(sys_setfsgid);
SYSCALL_DEF(sys_getgroups);
SYSCALL_DEF(sys_setgroups);
SYSCALL_DEF(sys_getpriority);
SYSCALL_DEF(sys_setpriority);
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

/* kernel/syscall.c: helpers for the files below */
struct inode;
int get_path(char *dst, uint32_t user_path);    /* copy a path in: 0 or -errno */
int dir_of(uint32_t dirfd, struct inode **dir); /* *at() dirfd -> NULL (cwd) or dir */
int32_t do_open(struct inode *dir, uint32_t user_path, uint32_t flags, uint32_t mode);

/* kernel/sys_fs.c */
SYSCALL_DEF(sys_mkdir);
SYSCALL_DEF(sys_mkdirat);
SYSCALL_DEF(sys_mknod);
SYSCALL_DEF(sys_mknodat);
SYSCALL_DEF(sys_symlink);
SYSCALL_DEF(sys_symlinkat);
SYSCALL_DEF(sys_link);
SYSCALL_DEF(sys_unlink);
SYSCALL_DEF(sys_rmdir);
SYSCALL_DEF(sys_unlinkat);
SYSCALL_DEF(sys_rename);
SYSCALL_DEF(sys_renameat);
SYSCALL_DEF(sys_renameat2);
SYSCALL_DEF(sys_chmod);
SYSCALL_DEF(sys_fchmodat);
SYSCALL_DEF(sys_fchmod);
SYSCALL_DEF(sys_chown32);
SYSCALL_DEF(sys_lchown32);
SYSCALL_DEF(sys_fchownat);
SYSCALL_DEF(sys_fchown32);
SYSCALL_DEF(sys_utimensat);
SYSCALL_DEF(sys_utimensat64);
SYSCALL_DEF(sys_truncate);
SYSCALL_DEF(sys_ftruncate);
SYSCALL_DEF(sys_truncate64);
SYSCALL_DEF(sys_ftruncate64);
SYSCALL_DEF(sys_statfs64);
SYSCALL_DEF(sys_fstatfs64);
SYSCALL_DEF(sys_mount);
SYSCALL_DEF(sys_umount2);

/* fs/flock.c */
SYSCALL_DEF(sys_flock);

/* fs/poll.c */
SYSCALL_DEF(sys_poll);
/* fs/epoll.c */
SYSCALL_DEF(sys_epoll_create);
SYSCALL_DEF(sys_epoll_create1);
SYSCALL_DEF(sys_epoll_ctl);
SYSCALL_DEF(sys_epoll_wait);
SYSCALL_DEF(sys_epoll_pwait);
SYSCALL_DEF(sys_ppoll64);
SYSCALL_DEF(sys_ppoll32);
SYSCALL_DEF(sys_pselect32);
SYSCALL_DEF(sys_select);
SYSCALL_DEF(sys_pselect64);

#endif
