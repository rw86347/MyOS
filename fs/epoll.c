/*
 * epoll.c — epoll: poll() with the list kept in the kernel
 *
 * poll() takes the whole list of files on every call. epoll keeps it:
 * epoll_create makes an "interest list" (itself an fd), epoll_ctl adds,
 * changes and removes files, epoll_wait waits for any of them. The X
 * server's main loop and most event libraries are built on it.
 *
 * Here the list is a small array, and waiting asks each file whether it's
 * ready (file_ops.poll), exactly as poll() does, sleeping on the same
 * queue in between. Linux instead has each file report to the epoll when
 * it becomes ready, so a wait costs nothing per idle file; with the few
 * dozen files a desktop has, the scan is cheap (docs/gaps.md).
 *
 * Edge-triggered entries (EPOLLET) are reported like level-triggered ones:
 * a program that asked for edges reads until EAGAIN anyway, so it sees no
 * difference except an extra wake-up. EPOLLONESHOT turns an entry off
 * after one report, until EPOLL_CTL_MOD.
 */
#include <errno.h>
#include <kheap.h>
#include <proc.h>
#include <signal.h>
#include <string.h>
#include <syscalls.h>
#include <thread.h>
#include <timer.h>
#include <uaccess.h>
#include <vfs.h>

#define EPOLLIN      0x001
#define EPOLLOUT     0x004
#define EPOLLERR     0x008
#define EPOLLHUP     0x010
#define EPOLLRDHUP   0x2000
#define EPOLLONESHOT (1u << 30)
#define EPOLLET      (1u << 31)
#define EPOLL_CLOEXEC 02000000
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLL_MAX 64                /* files per epoll */
#define WAIT_MAX  64                /* events returned per call */

/* struct epoll_event on i386: packed, 12 bytes. */
struct epoll_event { uint32_t events; uint32_t data[2]; } __attribute__((packed));

struct entry {
    int fd;
    struct file *f;                 /* compared, never followed unless it still matches */
    uint32_t serial;                /* ...and "matches" means this too, see below */
    uint32_t events;                /* wanted, plus EPOLLET / EPOLLONESHOT */
    uint32_t data[2];
};

struct epoll {
    struct entry e[EPOLL_MAX];
    int n;
};

static struct inode epoll_inode = {
    .ino = 0, .mode = 0600, .nlink = 1, .name = "[eventpoll]", .name_len = 11,
};

static const struct file_ops epoll_ops;

/* What entry k reports now (0: nothing). An entry whose file was closed
   (or whose fd now names another file) is dropped, as Linux does when the
   file goes away. */
static uint32_t entry_ready(struct epoll *ep, int k)
{
    struct entry *e = &ep->e[k];
    struct file *f = fd_get(e->fd);
    /* The serial and not just the address. No reference is held here on
       purpose -- holding one would keep the file alive past its last close,
       and a pipe registered in an epoll would never reach pipe_release -- so
       the file can be freed and another opened onto the same heap block at
       the same fd number. The addresses then match and this would poll a
       file the program never registered. The serial is per-open and never
       repeats (struct file in vfs.h). */
    if (f != e->f || !f || f->serial != e->serial) {
        ep->e[k] = ep->e[--ep->n];
        return ~0u;                 /* removed: the caller looks at k again */
    }
    if (!(e->events & ~(EPOLLET | EPOLLONESHOT)))
        return 0;                   /* a one-shot that fired, not re-armed */
    if (f->ops == &epoll_ops)
        return 0;                   /* nested epolls: not supported */
    uint32_t r = f->ops->poll ? f->ops->poll(f) : (EPOLLIN | EPOLLOUT);
    return r & (e->events | EPOLLERR | EPOLLHUP);
}

/* Collect up to max ready events into out[]. */
static int scan(struct epoll *ep, struct epoll_event *out, int max)
{
    int got = 0;
    for (int k = 0; k < ep->n && got < max; ) {
        uint32_t r = entry_ready(ep, k);
        if (r == ~0u)
            continue;
        if (r) {
            out[got].events = r;
            out[got].data[0] = ep->e[k].data[0];
            out[got].data[1] = ep->e[k].data[1];
            got++;
            if (ep->e[k].events & EPOLLONESHOT)
                ep->e[k].events &= EPOLLET | EPOLLONESHOT;
        }
        k++;
    }
    return got;
}

static int32_t epoll_read(struct file *f, void *b, uint32_t n)
{
    (void)f, (void)b, (void)n;
    return -EINVAL;
}

static int32_t epoll_write(struct file *f, const void *b, uint32_t n)
{
    (void)f, (void)b, (void)n;
    return -EINVAL;
}

static void epoll_release(struct file *f)
{
    kfree(f->priv);
}

static const struct file_ops epoll_ops = {
    .read = epoll_read, .write = epoll_write, .release = epoll_release,
};

static struct epoll *epoll_of(uint32_t fd, int *err)
{
    struct file *f = fd_get((int)fd);
    if (!f) {
        *err = -EBADF;
        return NULL;
    }
    if (f->ops != &epoll_ops) {
        *err = -EINVAL;
        return NULL;
    }
    return f->priv;
}

static int32_t create(uint32_t flags)
{
    if (flags & ~EPOLL_CLOEXEC)
        return -EINVAL;
    struct epoll *ep = kmalloc(sizeof *ep);
    if (!ep)
        return -ENOMEM;
    ep->n = 0;
    struct file *f = file_new(&epoll_ops, &epoll_inode, O_RDWR);
    if (!f) {
        kfree(ep);
        return -ENOMEM;
    }
    f->priv = ep;
    int fd = fd_install(f);
    if (fd < 0) {
        file_put(f);
        return fd;
    }
    if (flags & EPOLL_CLOEXEC)
        proc_current()->cloexec |= 1u << fd;
    return fd;
}

SYSCALL_DEF(sys_epoll_create)  { return (int32_t)a1 <= 0 ? -EINVAL : create(0); }
SYSCALL_DEF(sys_epoll_create1) { return create(a1); }

/* epoll_ctl(epfd, op, fd, struct epoll_event *) */
SYSCALL_DEF(sys_epoll_ctl)
{
    /* Both fds are checked here and the pointers thrown away, because the
       copy below can sleep: a fault on a page of an mmap'd file waits for the
       disk and a sibling thread runs. If that sibling closes epfd, epoll_release
       does kfree(f->priv) and every line after the copy writes into a freed
       heap block -- and in a `make test` build the block has been handed to
       the next kmalloc and poisoned, so ep->n reads as 0xA5A5A5A5 and the
       search below runs off the end of the heap.
       Checked before the copy and fetched again after it, rather than held
       with file_get, so that the errors still come out in Linux's order:
       EBADF for a bad fd even when the event pointer is bad too. */
    int err = 0;
    if (!epoll_of(a1, &err))
        return err;
    if (!fd_get((int)a3))
        return -EBADF;
    if (a3 == a1)
        return -EINVAL;
    struct epoll_event ev = { 0, { 0, 0 } };
    if (a2 != EPOLL_CTL_DEL && copy_from_user(&ev, (const void *)a4, sizeof ev) < 0)
        return -EFAULT;
    struct epoll *ep = epoll_of(a1, &err);
    if (!ep)
        return err;                 /* closed while we were in the copy */
    struct file *f = fd_get((int)a3);
    if (!f)
        return -EBADF;
    int k = 0;
    while (k < ep->n && !(ep->e[k].fd == (int)a3 && ep->e[k].f == f &&
                          ep->e[k].serial == f->serial))
        k++;
    switch (a2) {
    case EPOLL_CTL_ADD:
        if (k < ep->n)
            return -EEXIST;
        if (ep->n == EPOLL_MAX)
            return -ENOSPC;
        ep->n++;
        ep->e[k].fd = (int)a3;
        ep->e[k].f = f;
        ep->e[k].serial = f->serial;
        /* fall through */
    case EPOLL_CTL_MOD:
        if (k == ep->n)
            return -ENOENT;
        ep->e[k].events = ev.events;
        ep->e[k].data[0] = ev.data[0];
        ep->e[k].data[1] = ev.data[1];
        break;
    case EPOLL_CTL_DEL:
        if (k == ep->n)
            return -ENOENT;
        ep->e[k] = ep->e[--ep->n];
        break;
    default:
        return -EINVAL;
    }
    wq_wake_all(&poll_wait);        /* a waiter in another thread looks again */
    return 0;
}

static int32_t do_wait(uint32_t epfd, uint32_t uevents, int32_t max, int32_t timeout_ms)
{
    int err = 0;
    struct epoll *ep = epoll_of(epfd, &err);
    if (!ep)
        return err;
    if (max <= 0)
        return -EINVAL;
    if (max > WAIT_MAX)
        max = WAIT_MAX;
    struct epoll_event out[WAIT_MAX];
    uint32_t deadline = timeout_ms > 0 ? timer_ms() + (uint32_t)timeout_ms + 1 : 0;
    int got;
    for (;;) {
        got = scan(ep, out, max);
        if (got || timeout_ms == 0)
            break;
        if (signal_pending())
            return -EINTR;
        int timed_out = wq_sleep_until(&poll_wait, deadline);
        /* Fetched again before either path touches `ep`, not just before the
           next round: another thread may have closed epfd while we slept, and
           scan() does not only read through the pointer -- it drops entries
           whose file went away and clears EPOLLONESHOT, so a stale one is a
           write into freed memory. */
        ep = epoll_of(epfd, &err);
        if (!ep)
            return err;
        if (timed_out) {
            got = scan(ep, out, max);
            break;
        }
    }
    if (got && copy_to_user((void *)uevents, out, got * sizeof out[0]) < 0)
        return -EFAULT;
    return got;
}

SYSCALL_DEF(sys_epoll_wait)  { return do_wait(a1, a2, (int32_t)a3, (int32_t)a4); }
SYSCALL_DEF(sys_epoll_pwait) { return do_wait(a1, a2, (int32_t)a3, (int32_t)a4); }
