#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>

/*
 * System call handlers that live outside syscall.c. Every handler gets the
 * six argument registers (i386: EBX, ECX, EDX, ESI, EDI, EBP; x86_64: RDI,
 * RSI, RDX, R10, R8, R9) and returns the value for EAX / RAX: a result, or
 * -errno. They are register-sized — uintptr_t / intptr_t, which on i386 is
 * exactly uint32_t / int32_t — so a 64-bit pointer arrives whole.
 */
#define SYSCALL_ARG __attribute__((unused)) uintptr_t
#define SYSCALL_DEF(name) \
    intptr_t name(SYSCALL_ARG a1, SYSCALL_ARG a2, SYSCALL_ARG a3, \
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
SYSCALL_DEF(sys_clock_settime64);
SYSCALL_DEF(sys_adjtimex);
SYSCALL_DEF(sys_clock_adjtime);
SYSCALL_DEF(sys_clock_adjtime64);
SYSCALL_DEF(sys_clock_getres);
SYSCALL_DEF(sys_clock_getres64);
SYSCALL_DEF(sys_nanosleep);
SYSCALL_DEF(sys_clock_nanosleep);
SYSCALL_DEF(sys_clock_nanosleep64);
SYSCALL_DEF(sys_getrlimit);
SYSCALL_DEF(sys_setrlimit);
SYSCALL_DEF(sys_prlimit64);
SYSCALL_DEF(sys_sysinfo);
SYSCALL_DEF(sys_personality);
SYSCALL_DEF(sys_capget);
SYSCALL_DEF(sys_capset);
SYSCALL_DEF(sys_unshare);
SYSCALL_DEF(sys_setns);
SYSCALL_DEF(sys_swapon);
SYSCALL_DEF(sys_syslog);                 /* kprintf.c: dmesg */
SYSCALL_DEF(sys_times);
SYSCALL_DEF(sys_getrusage);
SYSCALL_DEF(sys_getrandom);
SYSCALL_DEF(sys_set_thread_area);
SYSCALL_DEF(sys_get_thread_area);
SYSCALL_DEF(sys_set_tid_address);
int64_t clock_ns(uint32_t clk);         /* ns on a clock: realtime or since boot */

/* kernel/posix_timer.c */
SYSCALL_DEF(sys_timer_create);
SYSCALL_DEF(sys_timer_settime);
SYSCALL_DEF(sys_timer_settime64);
SYSCALL_DEF(sys_timer_gettime);
SYSCALL_DEF(sys_timer_gettime64);
SYSCALL_DEF(sys_timer_getoverrun);
SYSCALL_DEF(sys_timer_delete);

/* kernel/shm.c: System V shared memory, through ipc() */
SYSCALL_DEF(sys_ipc);
SYSCALL_DEF(sys_shmget);
SYSCALL_DEF(sys_shmat);
SYSCALL_DEF(sys_shmdt);
SYSCALL_DEF(sys_shmctl);
SYSCALL_DEF(sys_semget);
SYSCALL_DEF(sys_semop);
SYSCALL_DEF(sys_semtimedop);
SYSCALL_DEF(sys_semctl);
SYSCALL_DEF(sys_msgget);
SYSCALL_DEF(sys_msgsnd);
SYSCALL_DEF(sys_msgrcv);
SYSCALL_DEF(sys_msgctl);

/* kernel/syscall.c: helpers for the files below */
struct inode;
int get_path(char *dst, uintptr_t user_path);   /* copy a path in: 0 or -errno */
int dir_of(uint32_t dirfd, struct inode **dir); /* *at() dirfd -> NULL (cwd) or dir */
int32_t do_open(struct inode *dir, uintptr_t user_path, uint32_t flags, uint32_t mode);

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
SYSCALL_DEF(sys_fallocate);
SYSCALL_DEF(sys_fallocate64);
SYSCALL_DEF(sys_fadvise64);
SYSCALL_DEF(sys_fadvise64_64);
SYSCALL_DEF(sys_statfs);
SYSCALL_DEF(sys_fstatfs);
SYSCALL_DEF(sys_memfd_create);
SYSCALL_DEF(sys_statfs64);
SYSCALL_DEF(sys_fstatfs64);
SYSCALL_DEF(sys_mount);
SYSCALL_DEF(sys_umount2);
SYSCALL_DEF(sys_pivot_root);
SYSCALL_DEF(sys_setxattr);              /* extended attributes: none here */
SYSCALL_DEF(sys_lsetxattr);
SYSCALL_DEF(sys_fsetxattr);
SYSCALL_DEF(sys_getxattr);
SYSCALL_DEF(sys_lgetxattr);
SYSCALL_DEF(sys_fgetxattr);
SYSCALL_DEF(sys_listxattr);
SYSCALL_DEF(sys_llistxattr);
SYSCALL_DEF(sys_flistxattr);
SYSCALL_DEF(sys_removexattr);
SYSCALL_DEF(sys_lremovexattr);
SYSCALL_DEF(sys_fremovexattr);

/* fs/inotify.c */
SYSCALL_DEF(sys_inotify_init);
SYSCALL_DEF(sys_inotify_init1);
SYSCALL_DEF(sys_inotify_add_watch);
SYSCALL_DEF(sys_inotify_rm_watch);

/* fs/flock.c */
SYSCALL_DEF(sys_flock);

/* fs/poll.c */
SYSCALL_DEF(sys_poll);
/* fs/epoll.c */
SYSCALL_DEF(sys_eventfd2);
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
