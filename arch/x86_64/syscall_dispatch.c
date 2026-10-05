/*
 * syscall_dispatch.c — Linux x86_64 syscall numbers -> portable handlers
 */
#include <syscall.h>
#include <syscall64.h>

extern int32_t syscall_dispatch(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6);

static uint32_t arg6(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                     uint64_t a6)
{
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    return (uint32_t)a6;
}

static int32_t forward(uint32_t inr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5, uint64_t a6)
{
    return syscall_dispatch(inr, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3, (uint32_t)a4,
                            (uint32_t)a5, arg6(a1, a2, a3, a4, a5, a6));
}

int32_t syscall_dispatch_x86_64(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    switch (nr) {
    case NR64_read:        return forward(NR_read, a1, a2, a3, a4, a5, a6);
    case NR64_write:       return forward(NR_write, a1, a2, a3, a4, a5, a6);
    case NR64_open:        return forward(NR_open, a1, a2, a3, a4, a5, a6);
    case NR64_close:       return forward(NR_close, a1, a2, a3, a4, a5, a6);
    case NR64_stat:        return forward(NR_statx, a1, a2, a3, a4, a5, a6); /* TODO stat */
    case NR64_fstat:       return forward(NR_fcntl, a1, a2, a3, a4, a5, a6);
    case NR64_lseek:       return forward(NR_lseek, a1, a2, a3, a4, a5, a6);
    case NR64_mmap:        return forward(NR_mmap2, a1, a2, a3, a4, a5, a6);
    case NR64_mprotect:    return forward(NR_mprotect, a1, a2, a3, a4, a5, a6);
    case NR64_munmap:      return forward(NR_munmap, a1, a2, a3, a4, a5, a6);
    case NR64_brk:         return forward(NR_brk, a1, a2, a3, a4, a5, a6);
    case NR64_rt_sigaction: return forward(NR_rt_sigaction, a1, a2, a3, a4, a5, a6);
    case NR64_rt_sigprocmask: return forward(NR_rt_sigprocmask, a1, a2, a3, a4, a5, a6);
    case NR64_rt_sigreturn: return forward(NR_rt_sigreturn, a1, a2, a3, a4, a5, a6);
    case NR64_ioctl:       return forward(NR_ioctl, a1, a2, a3, a4, a5, a6);
    case NR64_access:      return forward(NR_access, a1, a2, a3, a4, a5, a6);
    case NR64_pipe:        return forward(NR_pipe, a1, a2, a3, a4, a5, a6);
    case NR64_dup:         return forward(NR_dup, a1, a2, a3, a4, a5, a6);
    case NR64_dup2:        return forward(NR_dup2, a1, a2, a3, a4, a5, a6);
    case NR64_getpid:      return forward(NR_getpid, a1, a2, a3, a4, a5, a6);
    case NR64_exit:        return forward(NR_exit, a1, a2, a3, a4, a5, a6);
    case NR64_exit_group:  return forward(NR_exit_group, a1, a2, a3, a4, a5, a6);
    case NR64_wait4:       return forward(NR_wait4, a1, a2, a3, a4, a5, a6);
    case NR64_kill:        return forward(NR_kill, a1, a2, a3, a4, a5, a6);
    case NR64_uname:       return forward(NR_uname, a1, a2, a3, a4, a5, a6);
    case NR64_fcntl:       return forward(NR_fcntl, a1, a2, a3, a4, a5, a6);
    case NR64_getcwd:      return forward(NR_getcwd, a1, a2, a3, a4, a5, a6);
    case NR64_chdir:       return forward(NR_chdir, a1, a2, a3, a4, a5, a6);
    case NR64_mkdir:       return forward(NR_mkdir, a1, a2, a3, a4, a5, a6);
    case NR64_rmdir:       return forward(NR_rmdir, a1, a2, a3, a4, a5, a6);
    case NR64_link:        return forward(NR_link, a1, a2, a3, a4, a5, a6);
    case NR64_unlink:      return forward(NR_unlink, a1, a2, a3, a4, a5, a6);
    case NR64_readlink:    return forward(NR_readlink, a1, a2, a3, a4, a5, a6);
    case NR64_execve:      return forward(NR_execve, a1, a2, a3, a4, a5, a6);
    case NR64_getuid:      return forward(NR_getuid, a1, a2, a3, a4, a5, a6);
    case NR64_getgid:      return forward(NR_getgid, a1, a2, a3, a4, a5, a6);
    case NR64_geteuid:     return forward(NR_geteuid, a1, a2, a3, a4, a5, a6);
    case NR64_getegid:     return forward(NR_getegid, a1, a2, a3, a4, a5, a6);
    case NR64_getppid:     return forward(NR_getppid, a1, a2, a3, a4, a5, a6);
    case NR64_arch_prctl:  return 0; /* musl may probe; ignore for now */
    case NR64_set_tid_address: return forward(NR_set_tid_address, a1, a2, a3, a4, a5, a6);
    case NR64_set_robust_list: return forward(NR_set_robust_list, a1, a2, a3, a4, a5, a6);
    case NR64_futex:       return forward(NR_futex, a1, a2, a3, a4, a5, a6);
    case NR64_openat:      return forward(NR_openat, a1, a2, a3, a4, a5, a6);
    case NR64_newfstatat:  return forward(NR_faccessat, a1, a2, a3, a4, a5, a6);
    case NR64_getdents64:  return forward(NR_getdents64, a1, a2, a3, a4, a5, a6);
    case NR64_clock_gettime: return forward(NR_clock_gettime, a1, a2, a3, a4, a5, a6);
    case NR64_getrandom:   return forward(NR_getrandom, a1, a2, a3, a4, a5, a6);
    case NR64_clone:       return forward(NR_clone, a1, a2, a3, a4, a5, a6);
    case NR64_fork:        return forward(NR_fork, a1, a2, a3, a4, a5, a6);
    case NR64_vfork:       return forward(NR_vfork, a1, a2, a3, a4, a5, a6);
    default:
        return syscall_dispatch((uint32_t)nr, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3,
                                (uint32_t)a4, (uint32_t)a5, (uint32_t)a6);
    }
}

