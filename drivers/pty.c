/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * pty.c — pseudo-terminals: /dev/ptmx and /dev/pts/N
 *
 * A terminal window (xterm) is a program, not a screen, but the shell
 * inside it wants a terminal: line editing, Ctrl-C, a window size. A pty
 * is a terminal with a program on both sides:
 *
 *   master (the fd /dev/ptmx gave xterm)      slave (/dev/pts/N, the shell)
 *     write: typing  ------------------->  line discipline (tty.c) -> read
 *     read:  what to draw  <-------------  write, and the echo of typing
 *
 * Opening /dev/ptmx makes a new pair. The C library asks which N it got
 * (TIOCGPTN), unlocks it (TIOCSPTLCK, unlockpt), and the child opens
 * /dev/pts/N. The slave side is the same line discipline as the console:
 * only where the echo goes differs.
 *
 * Toward the master is one page used as a ring (as in pipe.c); toward the
 * slave is the tty's own input ring. A full ring makes the writer sleep;
 * echo never sleeps (it runs as part of the master's write), so echo into
 * a full ring is dropped, as on a real terminal that's not being read.
 *
 * Closing the master hangs the slave up: its foreground group gets SIGHUP
 * and reads see end of file. When the last slave closes, the master's
 * reads fail with EIO — that's how xterm sees its shell has gone.
 *
 * The slave a session leader opens first becomes its controlling terminal:
 * that's what /dev/tty means inside the window (doas asks for a password
 * there, and shells take job control from it).
 */
#include <errno.h>
#include <kheap.h>
#include <memlayout.h>
#include <pmm.h>
#include <proc.h>
#include <pty.h>
#include <string.h>
#include <thread.h>
#include <tty.h>
#include <uaccess.h>
#include <vfs.h>

#define PTY_MAX   16
#define OUT_SIZE  PAGE_SIZE         /* power of two: % is a mask */

#define O_NOCTTY  0400
#define POLLIN    0x001
#define POLLOUT   0x004
#define POLLHUP   0x010

#define TIOCSCTTY  0x540E
#define TIOCNOTTY  0x5422
#define TIOCGSID   0x5429
#define TIOCPKT    0x5420
#define FIONREAD   0x541B
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431
#define TIOCGPTLCK 0x80045439

struct pty {
    int index;
    uint32_t refs;                  /* open files, and writers asleep */
    uint32_t slaves;                /* open slave files */
    uint8_t master_open, locked, slave_opened;
    int sid;                        /* the session it controls (0: none) */
    struct tty *tty;
    uint8_t *out;                   /* slave -> master, one page */
    uint32_t out_head, out_tail;    /* bytes ever put in / taken out */
    struct wait_queue mread;        /* the master's readers */
    struct wait_queue outroom;      /* slave writers waiting for room */
    struct inode *node;             /* /dev/pts/N */
};

static struct pty *ptys[PTY_MAX];   /* by N: opening /dev/pts/N is O(1) */
static struct inode *pts_dir;
static char pts_names[PTY_MAX][3];

static void pget(struct pty *p) { p->refs++; }

static void pput(struct pty *p)
{
    if (--p->refs)
        return;
    ptys[p->index] = NULL;
    tty_free(p->tty);
    pmm_free(V2P(p->out));
    kfree(p);
}

static void wake_master(struct pty *p)
{
    wq_wake_all(&p->mread);
    wq_wake_all(&poll_wait);
}

static uint32_t out_room(struct pty *p)
{
    return OUT_SIZE - (p->out_head - p->out_tail);
}

/* Toward the master, with output processing ('\n' as "\r\n"). The caller
   has checked there's room for 2n, or accepts that the rest is dropped. */
static void put_out(struct pty *p, const char *s, uint32_t n)
{
    int crlf = tty_onlcr(p->tty);
    for (uint32_t k = 0; k < n; k++) {
        if (crlf && s[k] == '\n') {
            if (out_room(p) < 2)
                break;
            p->out[p->out_head++ % OUT_SIZE] = '\r';
        } else if (!out_room(p)) {
            break;
        }
        p->out[p->out_head++ % OUT_SIZE] = (uint8_t)s[k];
    }
    wake_master(p);
}

static void echo_out(void *owner, const char *s, uint32_t n)
{
    put_out(owner, s, n);
}

/* --- the master ------------------------------------------------------------- */

static int32_t master_read(struct file *f, void *user_buf, uint32_t n)
{
    struct pty *p = f->priv;
    while (p->out_head == p->out_tail) {
        if (p->slave_opened && !p->slaves)
            return -EIO;            /* the shell has gone */
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (signal_pending())
            return -ERESTARTSYS;
        wq_sleep(&p->mread);
    }
    /* Taken out of the ring before the copy (which may sleep while a slave
       writes into the freed space): through a snapshot, like tty.c. */
    char snap[512];
    uint32_t avail = p->out_head - p->out_tail;
    if (n > avail)
        n = avail;
    if (n > sizeof snap)
        n = sizeof snap;
    uint32_t at = p->out_tail % OUT_SIZE;
    uint32_t first = n < OUT_SIZE - at ? n : OUT_SIZE - at;
    memcpy(snap, p->out + at, first);
    memcpy(snap + first, p->out, n - first);
    p->out_tail += n;
    wq_wake_all(&p->outroom);
    wq_wake_all(&poll_wait);
    if (copy_to_user(user_buf, snap, n) < 0)
        return -EFAULT;
    return n;
}

/* Typing. Copied in small pieces through the stack; each piece waits
   until the slave's input has room for all of it. */
static int32_t master_write(struct file *f, const void *user_buf, uint32_t n)
{
    struct pty *p = f->priv;
    char chunk[128];
    uint32_t done = 0;
    int32_t err = 0;
    if (n > RW_MAX)
        n = RW_MAX;
    pget(p);
    while (done < n) {
        uint32_t k = n - done < sizeof chunk ? n - done : sizeof chunk;
        if (file_copy_in(f, chunk, (const char *)user_buf + done, k) < 0) {
            err = -EFAULT;
            break;
        }
        while (tty_space(p->tty) < k) {
            if (f->flags & O_NONBLOCK)
                err = -EAGAIN;
            else if (signal_pending())
                err = -ERESTARTSYS;
            if (err)
                break;
            tty_wait_space(p->tty);
        }
        if (err)
            break;
        tty_feed(p->tty, chunk, k);
        done += k;
    }
    pput(p);
    return done ? (int32_t)done : err;
}

static uint32_t master_poll(struct file *f)
{
    struct pty *p = f->priv;
    uint32_t ev = tty_space(p->tty) ? POLLOUT : 0;
    if (p->out_head != p->out_tail)
        ev |= POLLIN;
    else if (p->slave_opened && !p->slaves)
        ev |= POLLHUP;
    return ev;
}

static int32_t master_ioctl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    struct pty *p = f->priv;
    switch (cmd) {
    case TIOCGPTN:
        return copy_to_user((void *)arg, &p->index, sizeof p->index);
    case TIOCSPTLCK: {
        int lock;
        if (copy_from_user(&lock, (void *)arg, sizeof lock) < 0)
            return -EFAULT;
        p->locked = lock != 0;
        return 0;
    }
    case TIOCGPTLCK: {
        int lock = p->locked;
        return copy_to_user((void *)arg, &lock, sizeof lock);
    }
    case TIOCPKT: {                 /* packet mode: nobody needs it yet */
        int on;
        if (copy_from_user(&on, (void *)arg, sizeof on) < 0)
            return -EFAULT;
        return on ? -EINVAL : 0;
    }
    case FIONREAD: {
        int count = (int)(p->out_head - p->out_tail);
        return copy_to_user((void *)arg, &count, sizeof count);
    }
    default:                        /* termios, window size: the slave's */
        return tty_ioctl_on(p->tty, cmd, arg);
    }
}

static void master_release(struct file *f)
{
    struct pty *p = f->priv;
    p->master_open = 0;
    if (p->node)
        vfs_unlink(p->node);        /* /dev/pts/N goes away */
    p->node = NULL;
    tty_set_hung(p->tty);
    wq_wake_all(&p->outroom);
    pput(p);
}

static int master_open(struct file *f);

static const struct file_ops master_ops = {
    .read = master_read, .write = master_write, .ioctl = master_ioctl, .kbuf_ok = 1,
    .poll = master_poll, .release = master_release, .open = master_open,
};

/* --- the slave -------------------------------------------------------------- */

static int32_t slave_read(struct file *f, void *user_buf, uint32_t n)
{
    struct pty *p = f->priv;
    pget(p);
    int32_t r = tty_read_from(p->tty, f, user_buf, n);
    pput(p);
    return r;
}

static int32_t slave_write(struct file *f, const void *user_buf, uint32_t n)
{
    struct pty *p = f->priv;
    char chunk[128];
    uint32_t done = 0;
    int32_t err = tty_write_check(p->tty);  /* a background job, `stty tostop` */
    if (err)
        return err;
    if (n > RW_MAX)
        n = RW_MAX;
    pget(p);
    while (done < n) {
        uint32_t k = n - done < sizeof chunk ? n - done : sizeof chunk;
        if (file_copy_in(f, chunk, (const char *)user_buf + done, k) < 0) {
            err = -EFAULT;
            break;
        }
        while (p->master_open && out_room(p) < 2 * k) {    /* worst case: all '\n' */
            if (f->flags & O_NONBLOCK)
                err = -EAGAIN;
            else if (signal_pending())
                err = -ERESTARTSYS;
            if (err)
                break;
            wq_sleep(&p->outroom);
        }
        if (!err && !p->master_open)
            err = -EIO;             /* hung up */
        if (err)
            break;
        put_out(p, chunk, k);
        done += k;
    }
    pput(p);
    return done ? (int32_t)done : err;
}

static uint32_t slave_poll(struct file *f)
{
    struct pty *p = f->priv;
    if (!p->master_open)
        return POLLIN | POLLHUP;
    return (tty_readable(p->tty) ? POLLIN : 0) | (out_room(p) >= 2 ? POLLOUT : 0);
}

/* The session that already has this pty as its terminal, or the pty the
   caller's session has. A short scan: PTY_MAX entries, only from open()
   and job-control ioctls. */
static struct pty *pty_of_session(int sid)
{
    for (int n = 0; n < PTY_MAX; n++)
        if (ptys[n] && ptys[n]->sid == sid && ptys[n]->master_open)
            return ptys[n];
    return NULL;
}

/* Become the controlling terminal of the caller's session, if it's a
   session leader that has none. */
static int make_ctty(struct pty *p)
{
    struct process *me = proc_current();
    if (me->sid != me->pid || pty_of_session(me->sid))
        return -EPERM;
    if (p->sid && p->sid != me->sid)
        return -EPERM;              /* another session's terminal */
    p->sid = me->sid;
    tty_set_fg(p->tty, me->pgid);
    tty_set_session(p->tty, me->sid);
    return 0;
}

static int32_t slave_ioctl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    struct pty *p = f->priv;
    switch (cmd) {
    case TIOCSCTTY:
        return p->sid == proc_current()->sid ? 0 : make_ctty(p);
    case TIOCNOTTY:
        if (p->sid == proc_current()->sid) {
            p->sid = 0;
            tty_set_session(p->tty, 0);
        }
        return 0;
    case TIOCGSID:
        if (!p->sid)
            return -ENOTTY;
        return copy_to_user((void *)arg, &p->sid, sizeof p->sid);
    default: {
        int32_t r = tty_ioctl_check(p->tty, cmd);   /* a background job: SIGTTOU */
        return r ? r : tty_ioctl_on(p->tty, cmd, arg);
    }
    }
}

static void slave_release(struct file *f)
{
    struct pty *p = f->priv;
    if (--p->slaves == 0)
        wake_master(p);
    pput(p);
}

static void attach_slave(struct file *f, struct pty *p)
{
    f->priv = p;
    pget(p);
    p->slaves++;
    p->slave_opened = 1;
}

static int slave_open(struct file *f)
{
    uint32_t n = f->inode->rdev & 0xFF;
    struct pty *p = n < PTY_MAX ? ptys[n] : NULL;
    if (!p || !p->master_open || p->locked)
        return -EIO;
    attach_slave(f, p);
    if (!(f->flags & O_NOCTTY))
        make_ctty(p);               /* if it may: not an error otherwise */
    return 0;
}

static const struct file_ops slave_ops = {
    .read = slave_read, .write = slave_write, .ioctl = slave_ioctl, .kbuf_ok = 1,
    .poll = slave_poll, .release = slave_release, .open = slave_open,
};

/* --- making pairs ------------------------------------------------------------- */

static int master_open(struct file *f)
{
    int n = 0;
    while (n < PTY_MAX && ptys[n])
        n++;
    if (n == PTY_MAX || !pts_dir)
        return -ENOSPC;             /* Linux says this when it's out of ptys */
    struct pty *p = kmalloc(sizeof *p);
    uint32_t page = pmm_alloc();
    if (!p || !page)
        goto nomem;
    memset(p, 0, sizeof *p);
    p->tty = tty_new(echo_out, p);
    if (!p->tty)
        goto nomem;
    p->node = vfs_add(pts_dir, pts_names[n], n < 10 ? 1 : 2, S_IFCHR | 0620);
    if (!p->node) {
        tty_free(p->tty);
        goto nomem;
    }
    p->node->rdev = (PTY_SLAVE_MAJOR << 8) | (uint32_t)n;
    p->node->ops = &slave_ops;
    p->node->uid = proc_current()->cred.euid;   /* grantpt(): the opener's, */
    p->node->gid = 5;                           /* group tty, mode 0620 (Linux) */
    p->index = n;
    p->refs = 1;
    p->master_open = 1;
    p->locked = 1;                  /* until unlockpt() */
    p->out = P2V(page);
    ptys[n] = p;
    f->priv = p;
    return 0;
nomem:
    if (page)
        pmm_free(page);
    kfree(p);
    return -ENOMEM;
}

const struct file_ops *pty_master_ops(void) { return &master_ops; }
const struct file_ops *pty_slave_ops(void)  { return &slave_ops; }

int pty_open_ctty(struct file *f)
{
    struct pty *p = pty_of_session(proc_current()->sid);
    if (!p)
        return 0;
    f->ops = &slave_ops;
    attach_slave(f, p);
    return 1;
}

void pty_init(struct inode *dev)
{
    for (int n = 0; n < PTY_MAX; n++) {
        char *s = pts_names[n];
        if (n >= 10)
            *s++ = '0' + n / 10;
        *s = '0' + n % 10;
    }
    pts_dir = vfs_child(dev, "pts", 3);
    if (!pts_dir)
        pts_dir = vfs_add(dev, "pts", 3, S_IFDIR | 0755);
}
