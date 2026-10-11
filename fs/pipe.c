/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
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
 *
 * A named pipe (mkfifo) is the same thing reached through a name in the
 * tree: every open of it joins one pipe, which lives while anyone has it
 * open. runit talks to its service supervisors through two of these.
 */
#include <errno.h>
#include <kheap.h>
#include <memlayout.h>
#include <pmm.h>
#include <signal.h>
#include <uaccess.h>
#include <string.h>
#include <vfs.h>

#define PIPE_SIZE   PAGE_SIZE       /* power of two: % is a mask */
#define PIPE_ATOMIC 512

struct pipe {
    uint8_t *buf;                   /* one page, through the direct map */
    uint32_t head, tail;            /* bytes ever written / read */
    uint32_t readers, writers;      /* open ends */
    uint32_t reader_opens, writer_opens;    /* named pipe: opens ever (see fifo_open) */
    struct inode *fifo;             /* named pipe: its name's inode (else NULL) */
    struct wait_queue readq, writeq;
};

/* Open named pipes: which pipe belongs to which inode. Searched only by
   open(), and few are open at once. */
#define FIFO_MAX 64
static struct pipe *fifos[FIFO_MAX];

static struct inode pipe_inode = {
    .ino = 0, .mode = S_IFIFO | 0600, .nlink = 1, .name = "pipe", .name_len = 4,
};

static int32_t pipe_read(struct file *f, void *user_buf, uint32_t n)
{
    struct pipe *p = f->priv;
    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
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
    /* Consume before copying, because copying can sleep. If the destination
       is a page of an mmap'd file that is not resident, copy_to_user faults,
       mm_fault reads it from the disk, and that waits -- so a second reader
       on the same pipe (a fork shares it) runs here meanwhile. Were tail
       advanced afterwards, both would read the same bytes and both would
       subtract, tail would pass head, and `head - tail` would underflow to
       about four billion -- which stops clamping n on the next read and
       copies the rest of the kernel's memory out to whoever asked.
       The cost of going first is that an EFAULT loses the data, which is
       what read() is allowed to do. */
    p->tail += n;
    if (copy_to_user(user_buf, p->buf + at, first) < 0 ||
        copy_to_user((uint8_t *)user_buf + first, p->buf, n - first) < 0)
        return -EFAULT;
    wq_wake_all(&p->writeq);                /* there's room now */
    wq_wake_all(&poll_wait);
    return n;
}

static int32_t pipe_write(struct file *f, const void *user_buf, uint32_t n)
{
    struct pipe *p = f->priv;
    uint32_t done = 0;
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
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
        /* Claim the space before copying, for the reason given in pipe_read:
           copy_from_user can sleep, and a second writer that saw the old head
           would hand out the same bytes of the ring and push head past
           tail + PIPE_SIZE. `room` would then underflow and the copy below
           would run off the end of a one-page buffer with as much
           attacker-chosen data as it liked. */
        p->head += k;
        uint32_t claimed = p->head;
        if (file_copy_in(f, p->buf + at, src, first) < 0 ||
            file_copy_in(f, p->buf, src + first, k - first) < 0) {
            if (p->head == claimed)
                p->head -= k;       /* nobody else claimed after us: take it back */
            else {
                /* Someone did, so the space has to stay. Leave zeros rather
                   than whatever the page held, so a failed write cannot show
                   the reader the contents of recycled kernel memory. */
                memset(p->buf + at, 0, first);
                memset(p->buf, 0, k - first);
            }
            return done ? (int32_t)done : -EFAULT;
        }
        done += k;
        wq_wake_all(&p->readq);             /* data for a sleeping reader */
        wq_wake_all(&poll_wait);
    }
    return done;
}

static void pipe_release(struct file *f)
{
    struct pipe *p = f->priv;
    uint32_t acc = f->flags & O_ACCMODE;    /* a named pipe opened O_RDWR is both */
    if (acc != O_RDONLY) {
        p->writers--;
        wq_wake_all(&p->readq);             /* readers may now see EOF */
    }
    if (acc != O_WRONLY) {
        p->readers--;
        wq_wake_all(&p->writeq);            /* writers may now get EPIPE */
    }
    wq_wake_all(&poll_wait);
    if (p->readers == 0 && p->writers == 0) {
        for (int k = 0; p->fifo && k < FIFO_MAX; k++)
            if (fifos[k] == p)
                fifos[k] = NULL;
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
    .kbuf_ok = 1,
};

/* An empty pipe with no ends open, or NULL. */
static struct pipe *pipe_new(void)
{
    struct pipe *p = kmalloc(sizeof *p);
    uint32_t page = pmm_alloc();
    if (!p || !page) {
        if (p) kfree(p);
        if (page) pmm_free(page);
        return NULL;
    }
    memset(p, 0, sizeof *p);
    p->buf = P2V(page);
    return p;
}

int pipe_create(struct file **rd, struct file **wr)
{
    struct pipe *p = pipe_new();
    *rd = file_new(&pipe_ops, &pipe_inode, O_RDONLY);
    *wr = file_new(&pipe_ops, &pipe_inode, O_WRONLY);
    if (!p || !*rd || !*wr) {
        if (p) {
            pmm_free(V2P(p->buf));
            kfree(p);
        }
        if (*rd) kfree(*rd);
        if (*wr) kfree(*wr);
        return -ENOMEM;
    }
    p->readers = p->writers = 1;
    (*rd)->priv = (*wr)->priv = p;
    return 0;
}

/*
 * open() of a named pipe. As in Linux:
 *   read-only    waits for a writer to open it (O_NONBLOCK: doesn't wait)
 *   write-only   waits for a reader (O_NONBLOCK: -ENXIO if there is none)
 *   read-write   both ends at once; never waits
 * "Waits for a writer to open" means one open, not one that's still open:
 * a writer that comes and goes at once still lets the reader through (it
 * then reads end-of-file). The open counters tell.
 */
int fifo_open(struct inode *i, uint32_t flags, struct file **out)
{
    struct pipe *p = NULL;
    int slot = -1;
    for (int k = 0; k < FIFO_MAX; k++) {
        if (fifos[k] && fifos[k]->fifo == i)
            p = fifos[k];
        else if (!fifos[k] && slot < 0)
            slot = k;
    }
    uint32_t acc = flags & O_ACCMODE;
    if (!p && acc == O_WRONLY && (flags & O_NONBLOCK))
        return -ENXIO;                  /* nobody has it open to read */
    if (!p) {
        if (slot < 0)
            return -ENFILE;
        p = pipe_new();
        if (!p)
            return -ENOMEM;
        p->fifo = i;
        fifos[slot] = p;
    } else if (acc == O_WRONLY && (flags & O_NONBLOCK) && p->readers == 0) {
        return -ENXIO;
    }
    struct file *f = file_new(&pipe_ops, i, flags);
    if (!f) {
        if (p->readers == 0 && p->writers == 0) {
            fifos[slot] = NULL;
            pmm_free(V2P(p->buf));
            kfree(p);
        }
        return -ENOMEM;
    }
    f->priv = p;
    if (acc != O_WRONLY) {
        p->readers++;
        p->reader_opens++;
        wq_wake_all(&p->writeq);        /* a writer waiting in open */
    }
    if (acc != O_RDONLY) {
        p->writers++;
        p->writer_opens++;
        wq_wake_all(&p->readq);         /* a reader waiting in open */
    }
    if (acc == O_RDWR || (flags & O_NONBLOCK)) {
        *out = f;
        return 0;
    }
    /* Wait for the other end. */
    uint32_t *other = acc == O_RDONLY ? &p->writer_opens : &p->reader_opens;
    uint32_t seen = *other;
    uint32_t *open_now = acc == O_RDONLY ? &p->writers : &p->readers;
    while (*open_now == 0 && *other == seen) {
        if (signal_pending()) {
            file_put(f);                /* undoes our end, frees the pipe if last */
            return -EINTR;
        }
        wq_sleep(acc == O_RDONLY ? &p->readq : &p->writeq);
    }
    *out = f;
    return 0;
}
