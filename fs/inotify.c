/*
 * inotify.c — tell programs when files change: inotify_init1,
 * inotify_add_watch, inotify_rm_watch, and the events read() returns
 *
 * A program asks to watch a file or directory for some kinds of event
 * (opened, modified, created inside, deleted, renamed...). The kernel puts
 * an event in the program's inotify file each time one happens, and read()
 * returns them as Linux's struct inotify_event: wd, mask, cookie, len,
 * then the name (for something inside a watched directory). File managers
 * refresh this way instead of looking again every second.
 *
 * Cost when nobody watches: an inode with a watch has I_WATCHED, and the
 * VFS's hooks (vfs.h: notify) test that flag on the file and its directory
 * — one or two loads per open, read, write, close. Only a watched inode
 * walks the watch list, O(watches in the system), which is small.
 *
 * A watch holds a reference to its inode: a watched file that is deleted
 * stays until the watch goes (IN_DELETE_SELF, then IN_IGNORED, then the
 * watch is removed by itself, as on Linux).
 */
#include <errno.h>
#include <interrupts.h>
#include <kheap.h>

#include <proc.h>
#include <signal.h>
#include <string.h>
#include <syscalls.h>
#include <thread.h>
#include <uaccess.h>
#include <vfs.h>

#define IN_ALL_EVENTS   0x00000FFF
#define IN_Q_OVERFLOW   0x00004000
#define IN_IGNORED      0x00008000
#define IN_ONLYDIR      0x01000000
#define IN_DONT_FOLLOW  0x02000000
#define IN_EXCL_UNLINK  0x04000000
#define IN_MASK_CREATE  0x10000000
#define IN_MASK_ADD     0x20000000
#define IN_ONESHOT      0x80000000
#define IN_CLOEXEC      02000000        /* = O_CLOEXEC */
#define IN_NONBLOCK     04000           /* = O_NONBLOCK */

#define WATCHES_MAX     4096            /* in the whole system */
#define QUEUE_MAX       1024            /* events waiting in one inotify file */
#define FIONREAD        0x541B
#define POLLIN          0x001

struct event {
    struct event *next;
    uint32_t size;                      /* of what read() returns for it */
    struct {                            /* struct inotify_event (both CPUs) */
        int32_t wd;
        uint32_t mask, cookie, len;     /* len: of the name, NUL and padding */
        char name[];
    } e;
};

struct inotify {
    struct watch *watches;              /* this file's */
    int32_t next_wd;
    struct event *head, *tail;
    uint32_t queued, bytes;
    struct wait_queue wait;             /* read() and poll() */
};

struct watch {
    struct watch *next, *next_mine;     /* all watches; this inotify's */
    struct inotify *in;
    struct inode *inode;
    int32_t wd;
    uint32_t mask;
};

static struct watch *watches;           /* every watch, for the event hooks */
uint32_t inotify_watches;               /* (vfs.h's notify tests it first) */

/* --- events --------------------------------------------------------------- */

static void queue_event(struct inotify *in, int32_t wd, uint32_t mask, uint32_t cookie,
                        const char *name, uint32_t len)
{
    /* The name with a NUL, rounded up to whole 16-byte structs, as Linux does. */
    uint32_t nlen = len ? (len + 1 + 15) & ~15u : 0;
    struct event *t = in->tail;
    if (t && t->e.wd == wd && t->e.mask == mask && t->e.cookie == cookie &&
        t->e.len == nlen && (!len || memcmp(t->e.name, name, len) == 0))
        return;                         /* the same as the last: merged, as on Linux */
    if (in->queued >= QUEUE_MAX) {
        if (in->queued > QUEUE_MAX)
            return;                     /* the overflow event is already there */
        wd = -1, mask = IN_Q_OVERFLOW, cookie = 0, len = nlen = 0;
    }
    struct event *ev = kmalloc(sizeof *ev + nlen);
    if (!ev)
        return;                         /* out of memory: the event is lost */
    ev->next = NULL;
    ev->size = sizeof ev->e + nlen;
    ev->e.wd = wd;
    ev->e.mask = mask;
    ev->e.cookie = cookie;
    ev->e.len = nlen;
    memset(ev->e.name, 0, nlen);
    memcpy(ev->e.name, name, len);
    if (in->tail)
        in->tail->next = ev;
    else
        in->head = ev;
    in->tail = ev;
    in->queued++;
    in->bytes += ev->size;
    wq_wake_all(&in->wait);
}

static void remove_watch(struct watch *w)
{
    for (struct watch **pp = &watches; *pp; pp = &(*pp)->next)
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    for (struct watch **pp = &w->in->watches; *pp; pp = &(*pp)->next_mine)
        if (*pp == w) {
            *pp = w->next_mine;
            break;
        }
    inotify_watches--;
    int still = 0;
    for (struct watch *o = watches; o; o = o->next)
        still |= o->inode == w->inode;
    if (!still)
        w->inode->flags &= ~I_WATCHED;
    struct inode *i = w->inode;
    kfree(w);
    vfs_put(i);
}

/* Event `mask` on inode i: to i's own watches (no name), or — when `dir` is
   given — to dir's watches, with the name of the thing inside it. */
static void deliver(struct inode *i, struct inode *dir, uint32_t mask, uint32_t cookie,
                    const char *name, uint32_t len)
{
    struct inode *target = dir ? dir : i;
    if (!(target->flags & I_WATCHED))
        return;
    uint32_t flags = irq_save();
    struct watch *w = watches;
    while (w) {
        struct watch *next = w->next;
        if (w->inode == target && (w->mask & mask & IN_ALL_EVENTS)) {
            queue_event(w->in, w->wd, mask, cookie, dir ? name : NULL, dir ? len : 0);
            if (w->mask & IN_ONESHOT) {
                queue_event(w->in, w->wd, IN_IGNORED, 0, NULL, 0);
                remove_watch(w);
            }
        }
        w = next;
    }
    irq_restore(flags);
}

void fsnotify(struct inode *i, uint32_t mask)
{
    uint32_t isdir = S_ISDIR(i->mode) ? IN_ISDIR : 0;
    deliver(i, NULL, mask | isdir, 0, NULL, 0);
    /* ...and to its directory, named — except for a directory's own events
       when it is itself the one inside (Linux: events on a watched
       directory's children carry the child's name). */
    if (i->parent && i->parent != i && i->name)
        deliver(i, i->parent, mask | isdir, 0, i->name, i->name_len);
}

void fsnotify_entry(struct inode *dir, struct inode *child, uint32_t mask, uint32_t cookie,
                    const char *name, uint32_t len)
{
    uint32_t isdir = child && S_ISDIR(child->mode) ? IN_ISDIR : 0;
    deliver(child, dir, mask | isdir, cookie, name, len);
}

/* A watched inode was deleted: IN_DELETE_SELF, then IN_IGNORED, and the
   watch goes by itself (which may let the inode go too). */
void fsnotify_deleted(struct inode *i)
{
    if (!(i->flags & I_WATCHED))
        return;
    uint32_t flags = irq_save();
    struct watch *w = watches;
    while (w) {
        struct watch *next = w->next;
        if (w->inode == i) {
            if (w->mask & IN_DELETE_SELF)
                queue_event(w->in, w->wd, IN_DELETE_SELF, 0, NULL, 0);
            queue_event(w->in, w->wd, IN_IGNORED, 0, NULL, 0);
            remove_watch(w);
        }
        w = next;
    }
    irq_restore(flags);
}

uint32_t fsnotify_cookie(void)
{
    static uint32_t cookie;
    return ++cookie ? cookie : ++cookie;    /* never 0: 0 means "no pair" */
}

/* --- the inotify file ---------------------------------------------------- */

static int32_t in_read(struct file *f, void *user_buf, uint32_t n)
{
    struct inotify *in = f->priv;
    while (!in->head) {
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (signal_pending())
            return -EINTR;
        wq_sleep(&in->wait);
    }
    if (n < in->head->size)
        return -EINVAL;                 /* too small for even one event */
    uint32_t done = 0;
    while (in->head && done + in->head->size <= n) {
        struct event *ev = in->head;    /* take it, then copy (may sleep) */
        in->head = ev->next;
        if (!in->head)
            in->tail = NULL;
        in->queued--;
        in->bytes -= ev->size;
        int err = copy_to_user((uint8_t *)user_buf + done, &ev->e, ev->size);
        uint32_t size = ev->size;
        kfree(ev);
        if (err < 0)
            return done ? (int32_t)done : -EFAULT;
        done += size;
    }
    return (int32_t)done;
}

static uint32_t in_poll(struct file *f)
{
    struct inotify *in = f->priv;
    return in->head ? POLLIN : 0;
}

static int32_t in_ioctl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    struct inotify *in = f->priv;
    if (cmd != FIONREAD)
        return -EINVAL;
    int32_t n = (int32_t)in->bytes;
    return copy_to_user((void *)arg, &n, sizeof n);
}

static void in_release(struct file *f)
{
    struct inotify *in = f->priv;
    uint32_t flags = irq_save();
    while (in->watches)
        remove_watch(in->watches);
    irq_restore(flags);
    while (in->head) {
        struct event *ev = in->head;
        in->head = ev->next;
        kfree(ev);
    }
    kfree(in);
}

static const struct file_ops inotify_ops = {
    .read = in_read, .poll = in_poll, .ioctl = in_ioctl, .release = in_release,
};

SYSCALL_DEF(sys_inotify_init1)
{
    if (a1 & ~(uintptr_t)(IN_CLOEXEC | IN_NONBLOCK))
        return -EINVAL;
    struct inotify *in = kmalloc(sizeof *in);
    if (!in)
        return -ENOMEM;
    memset(in, 0, sizeof *in);
    in->next_wd = 1;
    struct file *f = file_new(&inotify_ops, NULL, O_RDONLY | (a1 & IN_NONBLOCK));
    if (!f) {
        kfree(in);
        return -ENOMEM;
    }
    f->priv = in;
    int fd = fd_install(f);
    if (fd < 0) {
        file_put(f);
        return fd;
    }
    if (a1 & IN_CLOEXEC)
        fd_set_cloexec(proc_current(), fd, 1);
    return fd;
}

SYSCALL_DEF(sys_inotify_init) { return sys_inotify_init1(0, 0, 0, 0, 0, 0); }

static struct inotify *inotify_of(uint32_t fd, int *err)
{
    struct file *f = fd_get(fd);
    *err = !f ? -EBADF : f->ops != &inotify_ops ? -EINVAL : 0;
    return *err ? NULL : f->priv;
}

/* inotify_add_watch(fd, path, mask): returns the watch's number (wd). The
   same file again changes that watch's mask (or adds to it, IN_MASK_ADD). */
SYSCALL_DEF(sys_inotify_add_watch)
{
    int err;
    struct inotify *in = inotify_of(a1, &err);
    if (!in)
        return err;
    uint32_t mask = (uint32_t)a3;
    if (!(mask & IN_ALL_EVENTS) || ((mask & IN_MASK_ADD) && (mask & IN_MASK_CREATE)))
        return -EINVAL;
    char path[PATH_MAX];
    struct inode *i;
    err = get_path(path, a2);
    if (!err)
        err = mask & IN_DONT_FOLLOW ? vfs_lookup_nofollow(path, &i) : vfs_lookup(path, &i);
    if (err)
        return err;
    if ((mask & IN_ONLYDIR) && !S_ISDIR(i->mode))
        return -ENOTDIR;
    uint32_t keep = mask & (IN_ALL_EVENTS | IN_ONESHOT | IN_EXCL_UNLINK);

    uint32_t flags = irq_save();
    for (struct watch *w = in->watches; w; w = w->next_mine) {
        if (w->inode != i)
            continue;
        if (mask & IN_MASK_CREATE) {
            irq_restore(flags);
            return -EEXIST;
        }
        w->mask = (mask & IN_MASK_ADD) ? w->mask | keep : keep;
        irq_restore(flags);
        return w->wd;
    }
    struct watch *w = inotify_watches < WATCHES_MAX ? kmalloc(sizeof *w) : NULL;
    if (!w) {
        irq_restore(flags);
        return -ENOSPC;
    }
    *w = (struct watch){ .next = watches, .next_mine = in->watches, .in = in, .inode = i,
                         .wd = in->next_wd++, .mask = keep };
    vfs_get(i);
    i->flags |= I_WATCHED;
    watches = w;
    in->watches = w;
    inotify_watches++;
    irq_restore(flags);
    return w->wd;
}

SYSCALL_DEF(sys_inotify_rm_watch)
{
    int err;
    struct inotify *in = inotify_of(a1, &err);
    if (!in)
        return err;
    uint32_t flags = irq_save();
    for (struct watch *w = in->watches; w; w = w->next_mine)
        if (w->wd == (int32_t)a2) {
            queue_event(in, w->wd, IN_IGNORED, 0, NULL, 0);
            remove_watch(w);
            irq_restore(flags);
            return 0;
        }
    irq_restore(flags);
    return -EINVAL;
}
