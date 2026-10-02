/*
 * pipe.c — pipes: `ls | wc`
 *
 * A pipe is a small buffer with two ends: one process writes into it, the
 * other reads out of it, in order. Here: one 4 KiB page used as a ring —
 * `head` counts all bytes ever written, `tail` all bytes ever read; their
 * difference is what's waiting, and (count % 4096) is the position.
 *
 *   reading an empty pipe   sleeps until there's data — or returns 0
 *                           ("end of file") once every writer has closed
 *   writing a full pipe     sleeps until a reader makes room
 *   writing, no readers     SIGPIPE (ends the writer, by default) + -EPIPE
 *
 * Nobody polls: a sleeping reader is woken by the write that gives it
 * data, a sleeping writer by the read that makes room.
 *
 * Writes of up to PIPE_ATOMIC (512) bytes are never mixed with another
 * writer's bytes: such a write waits until it fits completely (POSIX's
 * PIPE_BUF guarantee, which shells rely on).
 */
#include <errno.h>
#include <kheap.h>
#include <memlayout.h>
#include <pmm.h>
#include <signal.h>
#include <uaccess.h>
#include <vfs.h>

#define PIPE_SIZE   PAGE_SIZE       /* power of two: % is a mask */
#define PIPE_ATOMIC 512

struct pipe {
    uint8_t *buf;                   /* one page, through the direct map */
    uint32_t head, tail;            /* bytes ever written / read */
    uint32_t readers, writers;      /* open ends */
    struct wait_queue readq, writeq;
};

static struct inode pipe_inode = {
    .ino = 0, .mode = S_IFIFO | 0600, .nlink = 1, .name = "pipe", .name_len = 4,
};

static int32_t pipe_read(struct file *f, void *user_buf, uint32_t n)
{
    struct pipe *p = f->priv;
    if (n == 0)
        return 0;
    while (p->head == p->tail) {            /* empty */
        if (p->writers == 0)
            return 0;                       /* end of file */
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (signal_pending())
            return -EINTR;
        wq_sleep(&p->readq);
    }
    uint32_t avail = p->head - p->tail;
    if (n > avail)
        n = avail;
    /* The bytes may wrap around the end of the ring: up to two copies. */
    uint32_t at = p->tail % PIPE_SIZE;
    uint32_t first = n < PIPE_SIZE - at ? n : PIPE_SIZE - at;
    if (copy_to_user(user_buf, p->buf + at, first) < 0 ||
        copy_to_user((uint8_t *)user_buf + first, p->buf, n - first) < 0)
        return -EFAULT;
    p->tail += n;
    wq_wake_all(&p->writeq);                /* there's room now */
    wq_wake_all(&poll_wait);
    return n;
}

static int32_t pipe_write(struct file *f, const void *user_buf, uint32_t n)
{
    struct pipe *p = f->priv;
    uint32_t done = 0;
    while (done < n) {
        if (p->readers == 0) {
            signal_send(proc_current(), SIGPIPE);   /* usually ends the writer */
            return done ? (int32_t)done : -EPIPE;
        }
        uint32_t room = PIPE_SIZE - (p->head - p->tail);
        if (room == 0 || (n <= PIPE_ATOMIC && room < n)) {
            if (f->flags & O_NONBLOCK)
                return done ? (int32_t)done : -EAGAIN;
            if (signal_pending())
                return done ? (int32_t)done : -EINTR;
            wq_sleep(&p->writeq);           /* a reader will make room */
            continue;
        }
        uint32_t k = n - done < room ? n - done : room;
        uint32_t at = p->head % PIPE_SIZE;
        uint32_t first = k < PIPE_SIZE - at ? k : PIPE_SIZE - at;
        const uint8_t *src = (const uint8_t *)user_buf + done;
        if (copy_from_user(p->buf + at, src, first) < 0 ||
            copy_from_user(p->buf, src + first, k - first) < 0)
            return done ? (int32_t)done : -EFAULT;
        p->head += k;
        done += k;
        wq_wake_all(&p->readq);             /* data for a sleeping reader */
        wq_wake_all(&poll_wait);
    }
    return done;
}

static void pipe_release(struct file *f)
{
    struct pipe *p = f->priv;
    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        p->writers--;
        wq_wake_all(&p->readq);             /* readers may now see EOF */
    } else {
        p->readers--;
        wq_wake_all(&p->writeq);            /* writers may now get EPIPE */
    }
    wq_wake_all(&poll_wait);
    if (p->readers == 0 && p->writers == 0) {
        pmm_free(V2P(p->buf));
        kfree(p);
    }
}

static uint32_t pipe_poll(struct file *f)
{
    struct pipe *p = f->priv;
    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        if (p->readers == 0)
            return 0x008;                   /* POLLERR: nobody reads */
        return p->head - p->tail < PIPE_SIZE ? 0x004 : 0;   /* POLLOUT */
    }
    uint32_t r = p->head != p->tail ? 0x001 : 0;            /* POLLIN */
    if (p->writers == 0)
        r |= 0x010;                         /* POLLHUP: end of file */
    return r;
}

static const struct file_ops pipe_ops = {
    .read = pipe_read, .write = pipe_write, .release = pipe_release, .poll = pipe_poll,
};

int pipe_create(struct file **rd, struct file **wr)
{
    struct pipe *p = kmalloc(sizeof *p);
    uint32_t page = pmm_alloc();
    *rd = file_new(&pipe_ops, &pipe_inode, O_RDONLY);
    *wr = file_new(&pipe_ops, &pipe_inode, O_WRONLY);
    if (!p || !page || !*rd || !*wr) {
        if (p) kfree(p);
        if (page) pmm_free(page);
        if (*rd) kfree(*rd);
        if (*wr) kfree(*wr);
        return -ENOMEM;
    }
    p->buf = P2V(page);
    p->head = p->tail = 0;
    p->readers = p->writers = 1;
    p->readq.first = p->writeq.first = NULL;
    (*rd)->priv = (*wr)->priv = p;
    return 0;
}
