/*
 * eventfd.c — eventfd2(): an 8-byte counter you can poll
 *
 * Firefox (and glib) use this as a wakeup: one thread writes 1, another
 * is blocked in poll/read until the counter is not zero. Same idea as a
 * pipe with an 8-byte message, without the buffer.
 */
#include <errno.h>
#include <kheap.h>
#include <proc.h>
#include <thread.h>
#include <signal.h>
#include <string.h>
#include <syscalls.h>
#include <uaccess.h>
#include <vfs.h>

#define EFD_SEMAPHORE 1
#define EFD_NONBLOCK  O_NONBLOCK
#define EFD_CLOEXEC   O_CLOEXEC

struct eventfd {
    uint64_t count;
    uint32_t sem;                       /* EFD_SEMAPHORE: read returns 1 */
    struct wait_queue wait;
};

static struct inode efd_inode = {
    .ino = 0, .mode = S_IFCHR | 0600, .nlink = 1, .name = "eventfd", .name_len = 7,
};

static int32_t efd_read(struct file *f, void *user_buf, uint32_t n)
{
    struct eventfd *e = f->priv;
    if (n < 8)
        return -EINVAL;
    file_get(f);
    while (e->count == 0) {
        if (f->flags & O_NONBLOCK) {
            file_put(f);
            return -EAGAIN;
        }
        if (signal_pending()) {
            file_put(f);
            return -EINTR;
        }
        wq_sleep(&e->wait);
    }
    uint64_t v;
    if (e->sem) {
        v = 1;
        e->count--;
    } else {
        v = e->count;
        e->count = 0;
    }
    file_put(f);
    if (copy_to_user(user_buf, &v, 8) < 0)
        return -EFAULT;
    wq_wake_all(&e->wait);
    wq_wake_all(&poll_wait);
    return 8;
}

static int32_t efd_write(struct file *f, const void *user_buf, uint32_t n)
{
    struct eventfd *e = f->priv;
    uint64_t add;
    if (n < 8)
        return -EINVAL;
    if (copy_from_user(&add, user_buf, 8) < 0)
        return -EFAULT;
    if (add == ~0ull)
        return -EINVAL;
    file_get(f);
    for (;;) {
        if (add <= (~0ull - 1) - e->count) {    /* Linux: counter max is 2^64-2 */
            e->count += add;
            break;
        }
        if (f->flags & O_NONBLOCK) {
            file_put(f);
            return -EAGAIN;
        }
        if (signal_pending()) {
            file_put(f);
            return -EINTR;
        }
        wq_sleep(&e->wait);
    }
    file_put(f);
    wq_wake_all(&e->wait);
    wq_wake_all(&poll_wait);
    return 8;
}

static uint32_t efd_poll(struct file *f)
{
    struct eventfd *e = f->priv;
    uint32_t r = 0;
    if (e->count)
        r |= 0x001;                     /* POLLIN */
    if (e->count != ~0ull)
        r |= 0x004;                     /* POLLOUT */
    return r;
}

static void efd_release(struct file *f)
{
    kfree(f->priv);
}

static const struct file_ops efd_ops = {
    .read = efd_read, .write = efd_write, .poll = efd_poll, .release = efd_release,
};

SYSCALL_DEF(sys_eventfd2)
{
    if (a2 & ~(EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC))
        return -EINVAL;
    struct eventfd *e = kmalloc(sizeof *e);
    if (!e)
        return -ENOMEM;
    memset(e, 0, sizeof *e);
    e->count = a1;
    e->sem = a2 & EFD_SEMAPHORE;
    struct file *f = file_new(&efd_ops, &efd_inode, O_RDWR | (a2 & O_NONBLOCK));
    if (!f) {
        kfree(e);
        return -ENOMEM;
    }
    f->priv = e;
    int fd = fd_install(f);
    if (fd < 0) {
        file_put(f);
        return fd;
    }
    if (a2 & EFD_CLOEXEC) {
        struct process *p = proc_current();
        if (p)
            p->cloexec |= 1ull << fd;
    }
    return fd;
}
