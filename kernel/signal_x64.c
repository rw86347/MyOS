/*
 * signal_x64.c — minimal signal stubs until x86_64 sigreturn frames land.
 * BusyBox shell only needs rt_sigaction/procmask to succeed with defaults.
 */
#ifdef ARCH_X86_64

#include <errno.h>
#include <interrupts.h>
#include <proc.h>
#include <signal.h>
#include <thread.h>
#include <uaccess.h>

void signal_check(struct regs *r) { (void)r; }

int signal_send(struct process *p, int sig)
{
    (void)p;
    (void)sig;
    return 0;
}

void signal_send_group(int pgid, int sig) { (void)pgid; (void)sig; }

int signal_send_thread(struct thread *t, int sig) { (void)t; (void)sig; return 0; }

int signal_pending(void) { return 0; }

void signal_fork(struct process *child, struct process *parent)
{
    (void)child;
    (void)parent;
}

void signal_exec(struct process *p) { (void)p; }

int32_t sys_rt_sigaction_impl(int sig, uint32_t act, uint32_t oact, uint32_t size)
{
    (void)sig;
    (void)act;
    (void)size;
    if (oact) {
        struct k_sigaction z = { 0 };
        if (copy_to_user((void *)oact, &z, sizeof z) < 0)
            return -EFAULT;
    }
    return 0;
}

int32_t sys_rt_sigprocmask_impl(int how, uint32_t set, uint32_t oset, uint32_t size)
{
    (void)how;
    (void)set;
    (void)size;
    if (oset) {
        uint64_t zero = 0;
        if (copy_to_user((void *)oset, &zero, 8) < 0)
            return -EFAULT;
    }
    return 0;
}

int32_t sys_sigreturn_impl(int rt) { (void)rt; return -ENOSYS; }

int32_t sys_kill_impl(int pid, int sig) { (void)pid; (void)sig; return -ENOSYS; }

int32_t sys_rt_sigsuspend_impl(uint32_t set, uint32_t size)
{
    (void)set;
    (void)size;
    return -ENOSYS;
}

int32_t sys_rt_sigpending_impl(uint32_t set, uint32_t size)
{
    (void)size;
    if (set) {
        uint64_t zero = 0;
        if (copy_to_user((void *)set, &zero, 8) < 0)
            return -EFAULT;
    }
    return 0;
}

int32_t sys_rt_sigtimedwait_impl(uint32_t uset, uint32_t uinfo, uint32_t utimeout,
                                 uint32_t size, int time64)
{
    (void)uset;
    (void)uinfo;
    (void)utimeout;
    (void)size;
    (void)time64;
    return -ENOSYS;
}

void signal_alarm_tick(uint32_t now) { (void)now; }
void signal_alarm_cancel(struct process *p) { (void)p; }
int32_t sys_alarm_impl(uint32_t seconds) { (void)seconds; return 0; }
int32_t sys_getitimer_impl(int which, uint32_t cur) { (void)which; (void)cur; return 0; }
int32_t sys_setitimer_impl(int which, uint32_t new, uint32_t old)
{
    (void)which;
    (void)new;
    (void)old;
    return 0;
}

int proc_pgid_in_session(int pgid, int sid) { (void)pgid; (void)sid; return 0; }

#endif
