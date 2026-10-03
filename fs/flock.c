/*
 * flock.c — flock(fd, op): a whole-file advisory lock.
 *
 * "Advisory": only programs that ask for the lock are held back; read and
 * write work regardless. runit's runsv takes one on supervise/lock so two
 * supervisors never run the same service; package managers and databases
 * use it the same way.
 *
 * The lock belongs to the open file (struct file), not to the process: a
 * dup'd fd or a forked child shares it, and it goes away when the last fd
 * closes (file_put calls flock_release). Many may hold it shared (LOCK_SH)
 * or one exclusive (LOCK_EX). The state is one number in the inode:
 * >0 shared holders, -1 exclusive.
 *
 * Contention is rare, so every waiter sleeps on one queue and each unlock
 * wakes them all to look again. Changing a lock (shared <-> exclusive)
 * drops the old one first, as Linux does: it's not atomic.
 */
#include <errno.h>
#include <proc.h>
#include <signal.h>
#include <syscalls.h>
#include <thread.h>
#include <vfs.h>

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

static struct wait_queue flock_wait;

void flock_release(struct file *f)
{
    struct inode *i = f->inode;
    if (f->flock == LOCK_EX)
        i->flock = 0;
    else
        i->flock--;
    f->flock = 0;
    wq_wake_all(&flock_wait);
}

/* flock(fd, op) */
SYSCALL_DEF(sys_flock)
{
    struct file *f = fd_get(a1);
    if (!f)
        return -EBADF;
    if (!f->inode)
        return -EINVAL;
    uint32_t want = a2 & ~LOCK_NB;
    if (want != LOCK_SH && want != LOCK_EX && want != LOCK_UN)
        return -EINVAL;
    if (f->flock == want)
        return 0;                       /* already held this way */
    if (f->flock)
        flock_release(f);
    if (want == LOCK_UN)
        return 0;

    struct inode *i = f->inode;
    for (;;) {
        if (want == LOCK_SH ? i->flock >= 0 : i->flock == 0)
            break;
        if (a2 & LOCK_NB)
            return -EAGAIN;             /* EWOULDBLOCK */
        if (signal_pending())
            return -EINTR;
        wq_sleep(&flock_wait);
    }
    i->flock = want == LOCK_SH ? i->flock + 1 : -1;
    f->flock = want;
    return 0;
}
