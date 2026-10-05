/*
 * lock.c — POSIX record locks: fcntl(F_GETLK / F_SETLK / F_SETLKW), and
 * the open-file ones (F_OFD_GETLK / F_OFD_SETLK / F_OFD_SETLKW)
 *
 * A record lock covers a byte range of a file — read (shared) or write
 * (exclusive) — and, like flock(), is advisory: it only holds back other
 * programs that ask for locks too. Databases (SQLite), mail spools and
 * editors' lock files use them.
 *
 * Who owns a lock is the difference between the two kinds:
 *   POSIX  the process. Any close() of the file by that process drops all
 *          its locks on it (a famous POSIX wart, kept for compatibility),
 *          and so does exit. A forked child doesn't inherit them.
 *   OFD    the open file (struct file): shared through dup() and fork(),
 *          gone when its last fd closes. The newer, saner kind.
 * A lock never conflicts with its own owner's: setting one over a range the
 * owner already holds replaces that part (splitting a lock in two if need
 * be), and F_UNLCK cuts a hole.
 *
 * All locks live in one small table, scanned: locking is rare and the
 * table short, and when there are no locks at all (the normal case) the
 * close() path costs one test of `nlocks`. F_SETLKW sleeps on one queue
 * that every unlock wakes; before it sleeps it follows the chain of "who
 * waits for whom" and refuses (EDEADLK) to close a circle, as Linux does.
 */
#include <errno.h>
#include <proc.h>
#include <signal.h>
#include <string.h>
#include <syscalls.h>
#include <thread.h>
#include <uaccess.h>
#include <vfs.h>

#define F_RDLCK      0
#define F_WRLCK      1
#define F_UNLCK      2
#ifdef ARCH_X86_64
#define F_GETLK      5                  /* (x86_64's struct flock is the 64-bit one) */
#define F_SETLK      6
#define F_SETLKW     7
#else
#define F_GETLK      12                 /* i386 F_GETLK64 etc. with struct flock64 */
#define F_SETLK      13                 /* (musl's only ones; the 32-bit 5-7 are legacy) */
#define F_SETLKW     14
#endif
#define F_OFD_GETLK  36
#define F_OFD_SETLK  37
#define F_OFD_SETLKW 38
#define LOCKS_MAX    256
#define END          INT64_MAX           /* "to the end of the file, however long" */

/* struct flock as programs pass it, one definition for both CPUs: an int64
   is 4-aligned on i386 and 8-aligned on x86_64, which gives exactly Linux's
   flock64 (24 bytes) and flock (32 bytes). */
struct flock_user {
    int16_t type, whence;
    int64_t start, len;
    int32_t pid;
};
#ifdef ARCH_X86_64
_Static_assert(sizeof(struct flock_user) == 32, "Linux x86_64 struct flock");
#else
_Static_assert(sizeof(struct flock_user) == 24, "Linux i386 struct flock64");
#endif

struct lock {
    struct inode *inode;                /* NULL: a free slot */
    const void *owner;                  /* the process (POSIX) or the struct file (OFD) */
    int32_t pid;                        /* for F_GETLK: the holder; -1 for OFD */
    uint8_t type;                       /* F_RDLCK / F_WRLCK */
    int64_t start, end;                 /* [start, end], end may be END */
};

static struct lock locks[LOCKS_MAX];
uint32_t nlocks;                        /* (vfs.h's inline checks test it first) */
static struct wait_queue lock_wait;

static int overlaps(const struct lock *l, int64_t start, int64_t end)
{
    return l->start <= end && start <= l->end;
}

/* The first lock that keeps `owner` from taking [start, end] as `type`. */
static struct lock *conflict(const struct inode *i, const void *owner, int type,
                             int64_t start, int64_t end)
{
    for (int k = 0; k < LOCKS_MAX; k++) {
        struct lock *l = &locks[k];
        if (l->inode == i && l->owner != owner && overlaps(l, start, end) &&
            (type == F_WRLCK || l->type == F_WRLCK))
            return l;
    }
    return NULL;
}

static struct lock *free_slot(void)
{
    for (int k = 0; k < LOCKS_MAX; k++)
        if (!locks[k].inode)
            return &locks[k];
    return NULL;
}

/* Slots an unlock or a set of [start, end] may need: at most one split. */
static int room_for(int n)
{
    return LOCKS_MAX - nlocks >= (uint32_t)n;
}

/* Take [start, end] away from owner's locks on i (cutting, or splitting
   one in two). The caller made sure a split has a free slot. */
static void cut(struct inode *i, const void *owner, int64_t start, int64_t end)
{
    for (int k = 0; k < LOCKS_MAX; k++) {
        struct lock *l = &locks[k];
        if (l->inode != i || l->owner != owner || !overlaps(l, start, end))
            continue;
        if (l->start < start && l->end > end) {         /* a hole in the middle */
            struct lock *r = free_slot();
            *r = *l;
            r->start = end + 1;
            l->end = start - 1;
            nlocks++;
        } else if (l->start < start) {
            l->end = start - 1;
        } else if (l->end > end) {
            l->start = end + 1;
        } else {
            l->inode = NULL;                            /* all of it */
            nlocks--;
        }
    }
}

/* flock's range in absolute bytes. */
static int range_of(struct file *f, const struct flock_user *fl, int64_t *start, int64_t *end)
{
    int64_t base;
    switch (fl->whence) {
    case 0: base = 0; break;                            /* SEEK_SET */
    case 1: base = f->pos; break;                       /* SEEK_CUR */
    case 2: base = f->inode->size; break;               /* SEEK_END */
    default: return -EINVAL;
    }
    int64_t s = base + fl->start, e;
    if (fl->len > 0) {
        e = s + fl->len - 1;
        if (e < s)
            return -EOVERFLOW;
    } else if (fl->len < 0) {
        e = s - 1;
        s += fl->len;
    } else {
        e = END;
    }
    if (s < 0)
        return -EINVAL;
    *start = s, *end = e;
    return 0;
}

/* Would waiting for `blocker` close a circle of POSIX waiters? Each
   process's lock_wait says whose lock it sleeps for. */
static int deadlock(const struct process *me, const struct lock *blocker)
{
    const void *o = blocker->owner;
    for (int hops = 0; hops < PROC_MAX; hops++) {
        if (o == me)
            return 1;
        const struct process *p = NULL;
        for (int k = 0; k < PROC_MAX && !p; k++)
            if (proc_at(k) == o && proc_at(k)->state == PROC_ALIVE)
                p = proc_at(k);
        if (!p || !p->lock_wait)
            return 0;                   /* (OFD owners, or not waiting) */
        o = p->lock_wait;
    }
    return 0;
}

int lock_fcntl_cmd(uint32_t cmd)
{
    return (cmd >= F_GETLK && cmd <= F_SETLKW) || (cmd >= F_OFD_GETLK && cmd <= F_OFD_SETLKW);
}

int32_t lock_fcntl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    int ofd = cmd >= F_OFD_GETLK;
    struct flock_user fl;
    if (copy_from_user(&fl, (const void *)arg, sizeof fl) < 0)
        return -EFAULT;
    if (ofd && fl.pid != 0)
        return -EINVAL;                 /* (Linux: must be 0 for OFD locks) */
    int64_t start, end;
    int err = range_of(f, &fl, &start, &end);
    if (err)
        return err;
    struct process *me = proc_current();
    const void *owner = ofd ? (const void *)f : (const void *)me;
    struct inode *i = f->inode;

    if (cmd == F_GETLK || cmd == F_OFD_GETLK) {
        if (fl.type != F_RDLCK && fl.type != F_WRLCK)
            return -EINVAL;
        struct lock *l = conflict(i, owner, fl.type, start, end);
        if (!l) {
            fl.type = F_UNLCK;
        } else {
            fl.type = l->type;
            fl.whence = 0;
            fl.start = l->start;
            fl.len = l->end == END ? 0 : l->end - l->start + 1;
            fl.pid = l->pid;
        }
        return copy_to_user((void *)arg, &fl, sizeof fl);
    }

    if (fl.type == F_UNLCK) {
        if (!room_for(1))
            return -ENOLCK;
        cut(i, owner, start, end);
        wq_wake_all(&lock_wait);
        return 0;
    }
    if (fl.type != F_RDLCK && fl.type != F_WRLCK)
        return -EINVAL;
    uint32_t acc = f->flags & O_ACCMODE;
    if ((fl.type == F_RDLCK && acc == O_WRONLY) || (fl.type == F_WRLCK && acc == O_RDONLY))
        return -EBADF;                  /* a lock needs the access it guards */
    int wait = cmd == F_SETLKW || cmd == F_OFD_SETLKW;
    for (;;) {
        struct lock *l = conflict(i, owner, fl.type, start, end);
        if (!l)
            break;
        if (!wait)
            return -EAGAIN;
        if (!ofd && deadlock(me, l))
            return -EDEADLK;
        if (!ofd)
            me->lock_wait = l->owner;
        wq_sleep(&lock_wait);
        me->lock_wait = NULL;
        if (signal_pending())
            return -EINTR;
    }
    if (!room_for(2))
        return -ENOLCK;
    cut(i, owner, start, end);          /* what it held there is replaced */
    struct lock *n = free_slot();
    *n = (struct lock){ i, owner, ofd ? -1 : me->pid, (uint8_t)fl.type, start, end };
    nlocks++;
    wq_wake_all(&lock_wait);            /* (a write lock that became a read lock) */
    return 0;
}

/* Drop every lock `owner` holds, on inode i (NULL: on any file): a POSIX
   owner's close() or exit, an OFD owner's last close (vfs.h). */
void lock_release(const void *owner, const struct inode *i)
{
    int any = 0;
    for (int k = 0; k < LOCKS_MAX && nlocks; k++) {
        struct lock *l = &locks[k];
        if (l->inode && l->owner == owner && (!i || l->inode == i)) {
            l->inode = NULL;
            nlocks--;
            any = 1;
        }
    }
    if (any)
        wq_wake_all(&lock_wait);
}

void lock_exit(struct process *p)       /* (its files are closed after this) */
{
    if (nlocks)
        lock_release(p, NULL);
}
