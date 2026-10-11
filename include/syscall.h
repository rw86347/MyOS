/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
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
#define NR_alarm        27
#define NR_pause        29
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
#define NR_setitimer   104
#define NR_getitimer   105
#define NR_sigreturn   119
#define NR_getpgid     132
#define NR_fchdir      133
#define NR__llseek     140
#define NR_readv       145
#define NR_writev      146
#define NR_getsid      147
#define NR_sched_yield 158
#define NR_msync       144
#define NR_mlock       150
#define NR_munlock     151
#define NR_mlockall    152
#define NR_munlockall  153
#define NR_sched_setparam     154
#define NR_sched_getparam     155
#define NR_sched_setscheduler 156
#define NR_sched_getscheduler 157
#define NR_sched_get_priority_max 159
#define NR_sched_get_priority_min 160
#define NR_sched_rr_get_interval  161
#define NR_sched_setaffinity  241
#define NR_waitid      284
#define NR_getcpu      318
#define NR_preadv      333
#define NR_pwritev     334
#define NR_mlock2      376
#define NR_preadv2     378
#define NR_pwritev2    379
#define NR_close_range 436
#define NR_rt_sigreturn   173
#define NR_sigaltstack    186
#define NR_rt_sigaction   174
#define NR_rt_sigprocmask 175
#define NR_rt_sigpending  176
#define NR_rt_sigtimedwait 177
#define NR_rt_sigsuspend  179
#define NR_pread64     180
#define NR_pwrite64    181
#define NR_getcwd      183
#define NR_times       43
#define NR_getrusage   77
/* (more, 2026-10-05: time setting, capabilities, namespaces, swap) */
#define NR_chroot          61
#define NR_setxattr        226
#define NR_lsetxattr       227
#define NR_fsetxattr       228
#define NR_getxattr        229
#define NR_lgetxattr       230
#define NR_fgetxattr       231
#define NR_listxattr       232
#define NR_llistxattr      233
#define NR_flistxattr      234
#define NR_removexattr     235
#define NR_lremovexattr    236
#define NR_fremovexattr    237
#define NR_inotify_init    291
#define NR_inotify_add_watch 292
#define NR_inotify_rm_watch 293
#define NR_inotify_init1   332
#define NR_semget          393
#define NR_semctl          394
#define NR_shmget          395
#define NR_shmctl          396
#define NR_shmat           397
#define NR_shmdt           398
#define NR_msgget          399
#define NR_msgsnd          400
#define NR_msgrcv          401
#define NR_msgctl          402
#define NR_semtimedop_time64 420
#define NR_fadvise64_64    272
#define NR_pivot_root      217
#define NR_syslog          103
#define NR_swapon          87
#define NR_adjtimex        124
#define NR_personality     136
#define NR_capget          184
#define NR_capset          185
#define NR_swapoff         115
#define NR_unshare         310
#define NR_clock_adjtime   343
#define NR_syncfs          344
#define NR_setns           346
#define NR_clock_settime64 404
#define NR_clock_adjtime64 405
#define NR_vfork       190
#define NR_sendfile64  239
#define NR_copy_file_range 377
#define NR_mmap2       192
#define NR_getdents64  220
#define NR_fcntl64     221
#define NR_gettid      224
#define NR_tkill       238
#define NR_futex       240
#define NR_exit_group  252
#define NR_tgkill      270
#define NR_rt_sigqueueinfo   178
#define NR_rt_tgsigqueueinfo 335
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
#define NR_mount        21
#define NR_umount2      52
#define NR_sethostname  74
#define NR_reboot       88
#define NR_vhangup     111
#define NR_flock       143
#define NR_getpriority  96
#define NR_setpriority  97
#define NR_epoll_create 254
#define NR_epoll_ctl   255
#define NR_epoll_wait  256
#define NR_epoll_pwait 319
#define NR_eventfd2    328
#define NR_epoll_create1 329
#define NR_membarrier  375
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
#define NR_ipc         117
#define NR_fsync       118
#define NR_socketcall  102
#define NR_clone       120
#define NR_uname       122
#define NR_mprotect    125
#define NR__newselect  142
#define NR_fdatasync   148
#define NR_nanosleep   162
#define NR_mremap      163
#define NR_poll        168
#define NR_prctl       172
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
#define NR_truncate64  193
#define NR_ftruncate64 194
#define NR_madvise     219
#define NR_sched_getaffinity 242
#define NR_set_thread_area 243
#define NR_get_thread_area 244
#define NR_set_tid_address 258
#define NR_timer_create    259
#define NR_timer_settime   260
#define NR_timer_gettime   261
#define NR_timer_getoverrun 262
#define NR_timer_delete    263
#define NR_statfs64    268
#define NR_fstatfs64   269
#define NR_clock_gettime   265
#define NR_clock_getres    266
#define NR_clock_nanosleep 267
#define NR_mkdirat     296
#define NR_mknodat     297
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
#define NR_renameat2   353
#define NR_mincore     218
#define NR_readahead   225
#define NR_fallocate   324
#define NR_getrandom   355
#define NR_memfd_create 356
#define NR_socket      359
#define NR_socketpair  360
#define NR_bind        361
#define NR_connect     362
#define NR_listen      363
#define NR_accept4     364
#define NR_getsockopt  365
#define NR_setsockopt  366
#define NR_getsockname 367
#define NR_getpeername 368
#define NR_sendto      369
#define NR_sendmsg     370
#define NR_recvfrom    371
#define NR_recvmsg     372
#define NR_shutdown    373
#define NR_clock_gettime64 403
#define NR_clock_getres_time64 406
#define NR_clock_nanosleep_time64 407
#define NR_timer_gettime64 408
#define NR_timer_settime64 409
#define NR_utimensat_time64 412
#define NR_pselect6_time64 413
#define NR_ppoll_time64 414
#define NR_rt_sigtimedwait_time64 421
#define NR_futex_time64 422

/* Not a Linux syscall and not in a normal build: `make test` only, so that a
   user-space test can arm the deliberate sleep inside copy_to_user (see
   include/uaccess_test.h). The number is past everything we implement. */
#define NR_uaccess_test 423

/* Arguments with Linux values */
#define AT_FDCWD      (-100)        /* "relative to the current directory" */
#define AT_EMPTY_PATH 0x1000        /* statx: the dirfd itself (fstat) */
#define AT_SYMLINK_NOFOLLOW 0x100   /* statx: the link itself (lstat) */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Run system call `nr` (the architecture's Linux number) with up to 6
   register-sized arguments; returns the result for EAX / RAX. Called by
   the int 0x80, sysenter and syscall entry code. */
intptr_t syscall_dispatch(uint32_t nr, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                          uintptr_t a4, uintptr_t a5, uintptr_t a6);

#endif
