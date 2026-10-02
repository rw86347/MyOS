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
#define NR_fork          2
#define NR_read          3
#define NR_write         4
#define NR_open          5
#define NR_close         6
#define NR_waitpid       7
#define NR_execve       11
#define NR_chdir        12
#define NR_lseek        19
#define NR_getpid       20
#define NR_access       33
#define NR_kill         37
#define NR_dup          41
#define NR_pipe         42
#define NR_brk          45
#define NR_ioctl        54
#define NR_fcntl        55
#define NR_setpgid      57
#define NR_umask        60
#define NR_dup2         63
#define NR_getppid      64
#define NR_getpgrp      65
#define NR_setsid       66
#define NR_readlink     85
#define NR_munmap       91
#define NR_wait4       114
#define NR_sigreturn   119
#define NR_getpgid     132
#define NR_fchdir      133
#define NR__llseek     140
#define NR_readv       145
#define NR_writev      146
#define NR_getsid      147
#define NR_sched_yield 158
#define NR_rt_sigreturn   173
#define NR_rt_sigaction   174
#define NR_rt_sigprocmask 175
#define NR_rt_sigpending  176
#define NR_rt_sigsuspend  179
#define NR_getcwd      183
#define NR_vfork       190
#define NR_mmap2       192
#define NR_getdents64  220
#define NR_fcntl64     221
#define NR_tkill       238
#define NR_exit_group  252
#define NR_tgkill      270
#define NR_openat      295
#define NR_readlinkat  305
#define NR_faccessat   307
#define NR_dup3        330
#define NR_pipe2       331
#define NR_statx       383
#define NR_faccessat2  439

/* More, used by C libraries and shells at startup (step 8g). */
#define NR_creat         8
#define NR_link          9
#define NR_unlink       10
#define NR_time         13
#define NR_mknod        14
#define NR_chmod        15
#define NR_getuid       24
#define NR_sync         36
#define NR_rename       38
#define NR_mkdir        39
#define NR_rmdir        40
#define NR_getgid       47
#define NR_geteuid      49
#define NR_getegid      50
#define NR_setrlimit    75
#define NR_getrlimit    76
#define NR_gettimeofday 78
#define NR_symlink      83
#define NR_truncate     92
#define NR_ftruncate    93
#define NR_fchmod       94
#define NR_sysinfo     116
#define NR_fsync       118
#define NR_socketcall  102
#define NR_clone       120
#define NR_uname       122
#define NR_mprotect    125
#define NR__newselect  142
#define NR_fdatasync   148
#define NR_nanosleep   162
#define NR_poll        168
#define NR_ugetrlimit  191
#define NR_lchown32    198
#define NR_getuid32    199
#define NR_getgid32    200
#define NR_geteuid32   201
#define NR_getegid32   202
#define NR_setreuid32  203
#define NR_setregid32  204
#define NR_getgroups32 205
#define NR_setgroups32 206
#define NR_fchown32    207
#define NR_setresuid32 208
#define NR_getresuid32 209
#define NR_setresgid32 210
#define NR_getresgid32 211
#define NR_chown32     212
#define NR_setuid32    213
#define NR_setgid32    214
#define NR_setfsuid32  215
#define NR_setfsgid32  216
#define NR_madvise     219
#define NR_sched_getaffinity 242
#define NR_set_thread_area 243
#define NR_get_thread_area 244
#define NR_set_tid_address 258
#define NR_clock_gettime   265
#define NR_clock_getres    266
#define NR_clock_nanosleep 267
#define NR_mkdirat     296
#define NR_fchownat    298
#define NR_unlinkat    301
#define NR_renameat    302
#define NR_linkat      303
#define NR_symlinkat   304
#define NR_fchmodat    306
#define NR_pselect6    308
#define NR_ppoll       309
#define NR_set_robust_list 311
#define NR_utimensat   320
#define NR_prlimit64   340
#define NR_getrandom   355
#define NR_getsockname 367
#define NR_getpeername 368
#define NR_clock_gettime64 403
#define NR_clock_getres_time64 406
#define NR_clock_nanosleep_time64 407
#define NR_pselect6_time64 413
#define NR_ppoll_time64 414

/* Arguments with Linux values */
#define AT_FDCWD      (-100)        /* "relative to the current directory" */
#define AT_EMPTY_PATH 0x1000        /* statx: the dirfd itself (fstat) */
#define AT_SYMLINK_NOFOLLOW 0x100   /* statx: the link itself (lstat) */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Run system call `nr` with up to 6 arguments; returns the result for EAX.
   Called by the int 0x80 handler (interrupts.c). */
int32_t syscall_dispatch(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3,
                         uint32_t a4, uint32_t a5, uint32_t a6);

#endif
