/*
 * unix.c — AF_UNIX sockets: connections between programs on this machine
 *
 * A UNIX socket is a pipe in both directions that a program finds by name.
 * The X server listens on /tmp/.X11-unix/X0; every X program connects to
 * it and they talk through the connection: requests one way, events and
 * replies the other. syslog, D-Bus and most daemons work the same way.
 *
 *   server: socket, bind(name), listen, accept   -> one fd per client
 *   client: socket, connect(name)                -> one fd
 *   or:     socketpair                           -> two connected fds
 *
 * Each end of a connection owns a one-page ring of the bytes waiting for
 * it to read; the other end writes into it. It works like a pipe (pipe.c),
 * including claiming ring space before a copy that may sleep.
 *
 * A name is either a path, which becomes a socket node in the file system
 * (S_IFSOCK), or "abstract" (sun_path starts with a 0 byte): just bytes,
 * not in any directory. Bound sockets sit in a small table that connect()
 * searches: a connect is rare next to the reads and writes that follow.
 *
 * Lifetimes: a socket is freed when its last reference goes. The open file
 * holds one, each end of a connection holds one on the other, and a write
 * holds one on the receiving end while it copies (the copy can sleep, and
 * the receiver may be closed meanwhile). A connection waiting in a
 * listener's queue is held by the queue until accept() takes it.
 *
 * Datagram sockets (SOCK_DGRAM) keep message boundaries and have no
 * connection: each datagram is sent to a name (sendto) or to the socket
 * connect() named, and the receiver learns the sender's name (recvfrom).
 * wpa_supplicant's control socket works this way (wpa_cli, the tray icon),
 * as does syslog. A datagram is one kmalloc'd record in the receiver's
 * queue, bounded in count and bytes: a sender waits (or gets EAGAIN) while
 * it is full.
 *
 * Not yet: passing file descriptors (SCM_RIGHTS), credentials messages.
 */
#include <errno.h>
#include <kheap.h>
#include <memlayout.h>
#include <netdevice.h>
#include <pmm.h>
#include <proc.h>
#include <signal.h>
#include <socket.h>
#include <string.h>
#include <thread.h>
#include <stddef.h>
#include <uaccess.h>
#include <user.h>
#include <vfs.h>

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_TYPE_MASK 0xf
#define SOCK_NONBLOCK  04000
#define SOCK_CLOEXEC   02000000

#define MSG_PEEK     0x02
#define MSG_TRUNC    0x20
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000

#define SOL_SOCKET    1
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_PEERCRED  17
#define SO_ACCEPTCONN 30
#define SCM_RIGHTS    1

#define FIONREAD 0x541B
#define FIONBIO  0x5421

#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLHUP 0x010

#define BUF_SIZE    PAGE_SIZE       /* power of two: % is a mask */
#define BACKLOG_MAX 16
#define PATH_UN     108             /* sizeof sun_path */
#define BOUND_MAX   32              /* bound sockets, machine-wide */
#define IOV_MAX_K   16
#define DGRAM_MAX   16384           /* the largest datagram */
#define DQ_MAX      16              /* datagrams waiting for one socket (Linux: 10) */
#define DQ_BYTES    65536           /* ...and their bytes */

enum { U_NEW, U_LISTEN, U_CONNECTED };

struct ucred { int32_t pid; uint32_t uid, gid; };

struct usock {
    int type, state;
    uint32_t refs;
    struct usock *peer;             /* connected: the other end (NULL once it's gone) */
    uint8_t *buf;                   /* the bytes waiting for US to read */
    uint32_t head, tail;            /* bytes ever written into buf / read out */
    int rd_shut;                    /* no more bytes will come */
    int wr_shut;                    /* we said we won't write more */
    struct usock *backlog[BACKLOG_MAX];   /* listening: connections to accept */
    int nback, maxback;
    uint8_t name[PATH_UN];          /* bound: sun_path, namelen bytes (abstract: */
    uint32_t namelen;               /*   starts with 0); 0 = no name */
    struct inode *node;             /* a path name's socket node */
    struct ucred cred, peercred;    /* ours, and the other end's (SO_PEERCRED) */
    struct wait_queue wq;           /* readers, writers waiting for room, accept */
    /* SOCK_DGRAM: peer is where send() goes (connect), held, one way. */
    struct udgram *dq, *dq_last;    /* datagrams waiting for us to read */
    uint32_t dq_count, dq_bytes;
    int closed;                     /* its file is gone: sends to it are refused */
};

/* A datagram waiting to be read, with its sender's name (for recvfrom). */
struct udgram {
    struct udgram *next;
    uint32_t len, fromlen;
    uint8_t from[PATH_UN];
    uint8_t data[];
};

static struct usock *bound[BOUND_MAX];

static const struct file_ops unix_ops;
static int read_name(uintptr_t uaddr, uint32_t alen, uint8_t *name, uint32_t *len);
static struct usock *find_bound(const uint8_t *name, uint32_t len, int *err);

static struct inode unix_inode = {
    .ino = 0, .mode = S_IFSOCK | 0777, .nlink = 1, .name = "socket", .name_len = 6,
};

/* --- sockets and references ------------------------------------------------ */

static void my_cred(struct ucred *c)
{
    struct process *p = proc_current();
    c->pid = p ? p->pid : 0;
    c->uid = p ? p->cred.euid : 0;
    c->gid = p ? p->cred.egid : 0;
}

static struct usock *usock_new(int type)
{
    struct usock *s = kmalloc(sizeof *s);
    uint32_t page = type == SOCK_STREAM ? pmm_alloc() : 0;  /* (datagrams: no ring) */
    if (!s || (type == SOCK_STREAM && !page)) {
        if (s) kfree(s);
        if (page) pmm_free(page);
        return NULL;
    }
    memset(s, 0, sizeof *s);
    s->type = type;
    s->refs = 1;
    s->buf = page ? P2V(page) : NULL;
    my_cred(&s->cred);
    return s;
}

static void uget(struct usock *s) { s->refs++; }

static void dq_free(struct usock *s)
{
    while (s->dq) {
        struct udgram *d = s->dq;
        s->dq = d->next;
        kfree(d);
    }
    s->dq_last = NULL;
    s->dq_count = s->dq_bytes = 0;
}

static void uput(struct usock *s)
{
    if (--s->refs == 0) {
        if (s->buf)
            pmm_free(V2P(s->buf));
        dq_free(s);
        kfree(s);
    }
}

static void wake(struct usock *s)
{
    wq_wake_all(&s->wq);
    wq_wake_all(&poll_wait);
}

static void pair(struct usock *a, struct usock *b)
{
    a->peer = b, b->peer = a;
    uget(a), uget(b);
    a->state = b->state = U_CONNECTED;
}

/* The open file's reference is gone (or a queued connection is dropped):
   end the connection, empty the queue, give up the name. */
static void usock_close(struct usock *s)
{
    struct usock *p = s->peer;
    if (s->type == SOCK_DGRAM) {
        s->peer = NULL;
        if (p)
            uput(p);                /* our hold on where we sent */
        s->closed = 1;              /* senders still holding us get ECONNREFUSED */
        dq_free(s);
    } else if (p) {
        p->peer = NULL;
        p->rd_shut = 1;             /* it reads what's left, then end of file */
        s->peer = NULL;
        wake(p);
        uput(s);                    /* p's hold on us */
        uput(p);                    /* ours on p */
    }
    while (s->nback)
        usock_close(s->backlog[--s->nback]);
    for (int k = 0; k < BOUND_MAX; k++)
        if (bound[k] == s)
            bound[k] = NULL;
    if (s->node)
        vfs_put(s->node);           /* the name stays: connect says ECONNREFUSED */
    s->node = NULL;
    wake(s);
    uput(s);
}

static struct usock *usock_of(uint32_t fd, struct file **fp)
{
    struct file *f = fd_get((int)fd);
    if (fp)
        *fp = f;
    return f && f->ops == &unix_ops ? f->priv : NULL;
}

int unix_fd(uint32_t fd)
{
    return usock_of(fd, NULL) != NULL;
}

static int install(struct usock *s, uint32_t flags)
{
    struct file *f = file_new(&unix_ops, &unix_inode, O_RDWR | (flags & SOCK_NONBLOCK));
    if (!f) {
        usock_close(s);
        return -ENOMEM;
    }
    f->priv = s;
    int fd = fd_install(f);
    if (fd < 0) {
        file_put(f);                /* release closes s */
        return -EMFILE;
    }
    if (flags & SOCK_CLOEXEC)
        proc_current()->cloexec |= 1ull << fd;
    return fd;
}

/* --- reading and writing --------------------------------------------------- */

static int32_t usock_write(struct usock *s, const uint8_t *ubuf, uint32_t n,
                           int nonblock, int nosignal)
{
    uint32_t done = 0;
    if (s->state != U_CONNECTED)
        return -ENOTCONN;
    while (done < n) {
        struct usock *p = s->peer;
        if (!p || s->wr_shut) {
            if (!nosignal)
                signal_send(proc_current(), SIGPIPE);
            return done ? (int32_t)done : -EPIPE;
        }
        uint32_t room = BUF_SIZE - (p->head - p->tail);
        if (room == 0) {
            if (nonblock)
                return done ? (int32_t)done : -EAGAIN;
            if (signal_pending())
                return done ? (int32_t)done : -EINTR;
            uget(p);                /* its queue must outlive our sleep on it */
            wq_sleep(&p->wq);       /* its reader will make room */
            uput(p);
            continue;
        }
        uint32_t k = n - done < room ? n - done : room;
        uint32_t at = p->head % BUF_SIZE;
        uint32_t first = k < BUF_SIZE - at ? k : BUF_SIZE - at;
        /* Claim the space, then copy (pipe.c says why). p is held during
           the copy: if it sleeps and p's owner closes, the page stays ours. */
        uget(p);
        p->head += k;
        uint32_t claimed = p->head;
        if (copy_from_user(p->buf + at, ubuf + done, first) < 0 ||
            copy_from_user(p->buf, ubuf + done + first, k - first) < 0) {
            if (p->head == claimed)
                p->head -= k;
            else {
                memset(p->buf + at, 0, first);
                memset(p->buf, 0, k - first);
            }
            uput(p);
            return done ? (int32_t)done : -EFAULT;
        }
        done += k;
        wake(p);
        uput(p);
    }
    return done;
}

static int32_t usock_read(struct usock *s, uint8_t *ubuf, uint32_t n, int nonblock, int peek)
{
    if (s->state != U_CONNECTED)
        return -ENOTCONN;
    if (n == 0)
        return 0;
    while (s->head == s->tail) {
        if (s->rd_shut)
            return 0;               /* end of file */
        if (nonblock)
            return -EAGAIN;
        if (signal_pending())
            return -EINTR;
        wq_sleep(&s->wq);
    }
    uint32_t avail = s->head - s->tail;
    if (n > avail)
        n = avail;
    uint32_t at = s->tail % BUF_SIZE;
    uint32_t first = n < BUF_SIZE - at ? n : BUF_SIZE - at;
    if (!peek)
        s->tail += n;               /* consume first: pipe.c says why */
    if (copy_to_user(ubuf, s->buf + at, first) < 0 ||
        copy_to_user(ubuf + first, s->buf, n - first) < 0)
        return -EFAULT;
    if (!peek)
        wake(s);                    /* room for the writer */
    return n;
}

/* --- datagrams -------------------------------------------------------------------- */

/* One datagram, gathered from iov, into to's queue (to is held by the
   caller). It is copied in before it is queued, so a fault leaves no half
   datagram behind. */
static int32_t dgram_send(struct usock *s, struct usock *to, const struct user_iovec *iov,
                          uint32_t niov, int nonblock)
{
    uint32_t len = 0;
    for (uint32_t i = 0; i < niov; i++)
        if (iov[i].len > DGRAM_MAX || (len += (uint32_t)iov[i].len) > DGRAM_MAX)
            return -EMSGSIZE;
    struct udgram *d = kmalloc(sizeof *d + len);
    if (!d)
        return -ENOBUFS;
    uint32_t at = 0;
    for (uint32_t i = 0; i < niov; i++) {
        if (copy_from_user(d->data + at, (const void *)(uintptr_t)iov[i].base,
                           (uint32_t)iov[i].len) < 0) {
            kfree(d);
            return -EFAULT;
        }
        at += (uint32_t)iov[i].len;
    }
    d->next = NULL;
    d->len = len;
    d->fromlen = s->namelen;
    memcpy(d->from, s->name, s->namelen);
    for (;;) {
        int err = 0;
        if (to->closed)
            err = -ECONNREFUSED;
        else if (to->dq_count < DQ_MAX && to->dq_bytes + len <= DQ_BYTES)
            break;
        else if (nonblock)
            err = -EAGAIN;
        else if (signal_pending())
            err = -EINTR;
        if (err) {
            kfree(d);
            return err;
        }
        wq_sleep(&to->wq);          /* its reader will make room */
    }
    if (to->dq_last)
        to->dq_last->next = d;
    else
        to->dq = d;
    to->dq_last = d;
    to->dq_count++;
    to->dq_bytes += len;
    wake(to);
    return (int32_t)len;
}

/* The next datagram, scattered into iov; what doesn't fit is dropped
   (*trunc). Its sender's name goes to from/fromlen. Returns the bytes
   copied, or with MSG_TRUNC the datagram's real length, as Linux does. */
static int32_t dgram_recv(struct usock *s, const struct user_iovec *iov, uint32_t niov,
                          uint32_t flags, int nonblock, uint8_t *from, uint32_t *fromlen,
                          int *trunc)
{
    while (!s->dq) {
        if (s->rd_shut)
            return 0;
        if (nonblock)
            return -EAGAIN;
        if (signal_pending())
            return -EINTR;
        wq_sleep(&s->wq);
    }
    struct udgram *d = s->dq;       /* out of the queue first: the copy may sleep */
    s->dq = d->next;
    if (!s->dq)
        s->dq_last = NULL;
    s->dq_count--;
    s->dq_bytes -= d->len;
    uint32_t at = 0;
    int err = 0;
    for (uint32_t i = 0; i < niov && at < d->len; i++) {
        uint32_t k = d->len - at < iov[i].len ? d->len - at : (uint32_t)iov[i].len;
        if (copy_to_user((void *)(uintptr_t)iov[i].base, d->data + at, k) < 0) {
            err = -EFAULT;
            break;
        }
        at += k;
    }
    uint32_t len = d->len;
    *trunc = at < len;
    *fromlen = d->fromlen;
    memcpy(from, d->from, d->fromlen);
    if (flags & MSG_PEEK) {         /* back in front, for the next read */
        d->next = s->dq;
        s->dq = d;
        if (!d->next)
            s->dq_last = d;
        s->dq_count++;
        s->dq_bytes += len;
    } else {
        kfree(d);
        wake(s);                    /* room for a sender */
    }
    if (err)
        return err;
    return (int32_t)(flags & MSG_TRUNC ? len : at);
}

/* Where a datagram goes: the name given, else the connected socket.
   Returned held. A socket connected elsewhere takes nothing from us
   (Linux's rule: EPERM). */
static struct usock *dgram_dest(struct usock *s, uintptr_t uaddr, uint32_t alen, int *err)
{
    struct usock *to = s->peer;
    if (uaddr && alen) {
        uint8_t name[PATH_UN];
        uint32_t len;
        *err = read_name(uaddr, alen, name, &len);
        if (*err)
            return NULL;
        to = find_bound(name, len, err);
        if (!to)
            return NULL;
        if (to->type != SOCK_DGRAM) {
            *err = -EPROTOTYPE;
            return NULL;
        }
    } else if (!to) {
        *err = -ENOTCONN;
        return NULL;
    }
    if (to->closed) {
        *err = -ECONNREFUSED;
        return NULL;
    }
    if (to->peer && to->peer != s) {
        *err = -EPERM;
        return NULL;
    }
    uget(to);
    return to;
}

static int32_t unix_read(struct file *f, void *ubuf, uint32_t n)
{
    struct usock *s = f->priv;
    if (s->type == SOCK_DGRAM) {
        struct user_iovec v = { (uintptr_t)ubuf, n };
        uint8_t from[PATH_UN];
        uint32_t fromlen;
        int trunc;
        return dgram_recv(s, &v, 1, 0, f->flags & O_NONBLOCK, from, &fromlen, &trunc);
    }
    return usock_read(s, ubuf, n, f->flags & O_NONBLOCK, 0);
}

static int32_t unix_write(struct file *f, const void *ubuf, uint32_t n)
{
    struct usock *s = f->priv;
    if (s->type == SOCK_DGRAM) {
        int err;
        struct usock *to = dgram_dest(s, 0, 0, &err);
        if (!to)
            return err;
        struct user_iovec v = { (uintptr_t)ubuf, n };
        int32_t r = dgram_send(s, to, &v, 1, f->flags & O_NONBLOCK);
        uput(to);
        return r;
    }
    return usock_write(s, ubuf, n, f->flags & O_NONBLOCK, 0);
}

static uint32_t unix_poll(struct file *f)
{
    struct usock *s = f->priv;
    if (s->type == SOCK_DGRAM) {
        struct usock *p = s->peer;
        uint32_t r = s->dq || s->rd_shut ? POLLIN : 0;
        if (!p || p->closed || (p->dq_count < DQ_MAX && p->dq_bytes < DQ_BYTES))
            r |= POLLOUT;
        return r;
    }
    if (s->state == U_LISTEN)
        return s->nback ? POLLIN : 0;
    if (s->state != U_CONNECTED)
        return 0;
    uint32_t r = 0;
    if (s->head != s->tail || s->rd_shut)
        r |= POLLIN;
    if (!s->peer)
        r |= POLLHUP;
    else if (s->peer->head - s->peer->tail < BUF_SIZE)
        r |= POLLOUT;
    return r;
}

static int32_t unix_ioctl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    struct usock *s = f->priv;
    if ((cmd & 0xFF00) == 0x8900)
        return dev_ioctl(cmd, arg);     /* musl's if_nametoindex asks here */
    if (cmd == FIONREAD) {             /* (a datagram socket: the next one's size) */
        uint32_t n = s->type == SOCK_DGRAM ? (s->dq ? s->dq->len : 0) : s->head - s->tail;
        return copy_to_user((void *)arg, &n, 4) < 0 ? -EFAULT : 0;
    }
    if (cmd == FIONBIO) {
        uint32_t on;
        if (get_user32(&on, arg) < 0)
            return -EFAULT;
        if (on)
            f->flags |= O_NONBLOCK;
        else
            f->flags &= ~O_NONBLOCK;
        return 0;
    }
    return -ENOTTY;
}

static void unix_release(struct file *f)
{
    usock_close(f->priv);
}

static const struct file_ops unix_ops = {
    .read = unix_read, .write = unix_write, .release = unix_release,
    .ioctl = unix_ioctl, .poll = unix_poll,
};

/* --- names --------------------------------------------------------------------- */

/* struct sockaddr_un { sa_family_t family; char sun_path[108]; } from user
   space: the name's bytes and length. A path stops at its NUL. */
static int read_name(uintptr_t uaddr, uint32_t alen, uint8_t *name, uint32_t *len)
{
    uint8_t sa[2 + PATH_UN];
    if (alen < 2 || alen > sizeof sa)
        return -EINVAL;
    if (copy_from_user(sa, (const void *)uaddr, alen) < 0)
        return -EFAULT;
    if ((sa[0] | (sa[1] << 8)) != AF_UNIX)
        return -EINVAL;
    uint32_t n = alen - 2;
    if (n && sa[2] != 0) {          /* a path */
        uint32_t k = 0;
        while (k < n && sa[2 + k])
            k++;
        n = k;
    }
    if (n == 0)
        return -EINVAL;
    memcpy(name, sa + 2, n);
    *len = n;
    return 0;
}

static int write_name(uintptr_t uaddr, uintptr_t ulenp, const uint8_t *name, uint32_t len)
{
    if (!uaddr || !ulenp)
        return 0;
    uint32_t have;
    if (get_user32(&have, ulenp) < 0)
        return -EFAULT;
    if ((int32_t)have < 0)
        return -EINVAL;
    uint8_t sa[2 + PATH_UN + 1] = { AF_UNIX, 0 };
    memcpy(sa + 2, name, len);
    uint32_t full = 2 + len + (len && name[0] ? 1 : 0);    /* a path counts its NUL */
    uint32_t n = have < full ? have : full;
    if (copy_to_user((void *)uaddr, sa, n) < 0 || copy_to_user((void *)ulenp, &full, 4) < 0)
        return -EFAULT;
    return 0;
}

/* Who is bound to this name? */
static struct usock *find_bound(const uint8_t *name, uint32_t len, int *err)
{
    struct inode *node = NULL;
    if (name[0]) {
        char path[PATH_UN + 1];
        memcpy(path, name, len);
        path[len] = '\0';
        *err = vfs_lookup(path, &node);
        if (*err)
            return NULL;
        if ((node->mode & S_IFMT) != S_IFSOCK) {
            *err = -ECONNREFUSED;
            return NULL;
        }
    }
    for (int k = 0; k < BOUND_MAX; k++) {
        struct usock *b = bound[k];
        if (b && (node ? b->node == node
                       : b->namelen == len && memcmp(b->name, name, len) == 0))
            return b;
    }
    *err = -ECONNREFUSED;           /* a socket file nobody listens on */
    return NULL;
}

/* --- the syscalls ---------------------------------------------------------------- */

int32_t unix_socket(uint32_t type, uint32_t protocol)
{
    uint32_t t = type & SOCK_TYPE_MASK;
    if (type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;
    if (t != SOCK_STREAM && t != SOCK_DGRAM)
        return -ESOCKTNOSUPPORT;
    if (protocol != 0)
        return -EPROTONOSUPPORT;
    struct usock *s = usock_new((int)t);
    if (!s)
        return -ENOMEM;
    return install(s, type);
}

SYSCALL_DEF(unix_bind)
{
    struct usock *s = usock_of(a1, NULL);
    uint8_t name[PATH_UN];
    uint32_t len;
    int err = read_name(a2, a3, name, &len);
    if (err)
        return err;
    if (s->namelen)
        return -EINVAL;             /* bound already */
    int slot = -1;
    for (int k = 0; k < BOUND_MAX; k++) {
        if (!bound[k] && slot < 0)
            slot = k;
        else if (bound[k] && !name[0] && bound[k]->namelen == len &&
                 memcmp(bound[k]->name, name, len) == 0)
            return -EADDRINUSE;
    }
    if (slot < 0)
        return -EADDRINUSE;
    if (name[0]) {                  /* a path: make the socket node */
        char path[PATH_UN + 1];
        memcpy(path, name, len);
        path[len] = '\0';
        struct inode *dir, *node;
        const char *last;
        uint32_t llen;
        err = vfs_lookup_parent(NULL, path, &dir, &last, &llen);
        if (!err)
            err = vfs_create(dir, last, llen,
                             S_IFSOCK | (0777 & ~proc_current()->umask), 0, &node);
        if (err)
            return err == -EEXIST ? -EADDRINUSE : err;
        vfs_get(node);
        s->node = node;
    }
    memcpy(s->name, name, len);
    s->namelen = len;
    bound[slot] = s;
    return 0;
}

SYSCALL_DEF(unix_listen)
{
    struct usock *s = usock_of(a1, NULL);
    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    if (s->state == U_CONNECTED)
        return -EINVAL;
    int32_t n = (int32_t)a2;
    s->maxback = n < 1 ? 1 : n > BACKLOG_MAX ? BACKLOG_MAX : n;
    s->state = U_LISTEN;
    my_cred(&s->cred);
    return 0;
}

SYSCALL_DEF(unix_connect)
{
    struct file *f;
    struct usock *s = usock_of(a1, &f);
    uint8_t name[PATH_UN];
    uint32_t len;
    int err = read_name(a2, a3, name, &len);
    if (err)
        return err;
    if (s->type == SOCK_DGRAM) {     /* just "send() goes there"; again: elsewhere */
        struct usock *to = find_bound(name, len, &err);
        if (!to)
            return err;
        if (to->type != SOCK_DGRAM)
            return -EPROTOTYPE;
        if (to->peer && to->peer != s)
            return -EPERM;
        uget(to);
        if (s->peer)
            uput(s->peer);
        s->peer = to;
        return 0;
    }
    if (s->state == U_CONNECTED)
        return -EISCONN;
    if (s->state == U_LISTEN)
        return -EINVAL;
    struct usock *l = find_bound(name, len, &err);
    if (!l)
        return err;
    if (l->type != s->type)
        return -EPROTOTYPE;
    if (s->type != SOCK_STREAM || l->state != U_LISTEN)
        return -ECONNREFUSED;
    uget(l);                        /* it may close while we wait */
    while (l->nback >= l->maxback && l->state == U_LISTEN) {
        if (f->flags & O_NONBLOCK)
            err = -EAGAIN;
        else if (signal_pending())
            err = -EINTR;
        if (err) {
            uput(l);
            return err;
        }
        wq_sleep(&l->wq);
    }
    if (l->state != U_LISTEN || !l->namelen) {
        uput(l);
        return -ECONNREFUSED;
    }
    struct usock *a = usock_new(s->type);   /* the end accept() will return */
    if (!a) {
        uput(l);
        return -ENOMEM;
    }
    pair(s, a);
    memcpy(a->name, l->name, l->namelen);   /* getsockname on it: the listener's */
    a->namelen = l->namelen;
    a->cred = l->cred;
    a->peercred = s->cred;
    s->peercred = l->cred;
    l->backlog[l->nback++] = a;     /* the queue holds a's first reference */
    wake(l);
    uput(l);
    return 0;
}

SYSCALL_DEF(unix_accept4)
{
    struct file *f;
    struct usock *s = usock_of(a1, &f);
    if (a4 & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;
    if (s->state != U_LISTEN)
        return -EINVAL;
    while (!s->nback) {
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (signal_pending())
            return -EINTR;
        wq_sleep(&s->wq);
        if (s->state != U_LISTEN)
            return -EINVAL;
    }
    struct usock *a = s->backlog[0];
    memmove(s->backlog, s->backlog + 1, --s->nback * sizeof s->backlog[0]);
    wake(s);                        /* a connect waiting for room in the queue */
    struct usock *p = a->peer;
    int err = write_name(a2, a3, p ? p->name : a->name, p ? p->namelen : 0);
    int fd = install(a, a4);
    if (fd >= 0 && err) {
        fd_close(fd);
        return err;
    }
    return fd;
}

SYSCALL_DEF(unix_getsockname)
{
    struct usock *s = usock_of(a1, NULL);
    return write_name(a2, a3, s->name, s->namelen);
}

SYSCALL_DEF(unix_getpeername)
{
    struct usock *s = usock_of(a1, NULL);
    if (!s->peer)
        return -ENOTCONN;
    return write_name(a2, a3, s->peer->name, s->peer->namelen);
}

SYSCALL_DEF(unix_sendto)
{
    struct file *f;
    struct usock *s = usock_of(a1, &f);
    if (s->type == SOCK_DGRAM) {
        int err;
        struct usock *to = dgram_dest(s, a5, a6, &err);
        if (!to)
            return err;
        struct user_iovec v = { a2, a3 };
        int32_t r = dgram_send(s, to, &v, 1, (f->flags & O_NONBLOCK) || (a4 & MSG_DONTWAIT));
        uput(to);
        return r;
    }
    return usock_write(s, (const uint8_t *)a2, a3,
                       (f->flags & O_NONBLOCK) || (a4 & MSG_DONTWAIT), a4 & MSG_NOSIGNAL);
}

SYSCALL_DEF(unix_recvfrom)
{
    struct file *f;
    struct usock *s = usock_of(a1, &f);
    if (s->type == SOCK_DGRAM) {
        struct user_iovec v = { a2, a3 };
        uint8_t from[PATH_UN];
        uint32_t fromlen;
        int trunc;
        int32_t r = dgram_recv(s, &v, 1, a4, (f->flags & O_NONBLOCK) || (a4 & MSG_DONTWAIT),
                               from, &fromlen, &trunc);
        if (r >= 0) {
            int err = write_name(a5, a6, from, fromlen);
            if (err)
                return err;
        }
        return r;
    }
    int32_t r = usock_read(s, (uint8_t *)a2, a3,
                           (f->flags & O_NONBLOCK) || (a4 & MSG_DONTWAIT), a4 & MSG_PEEK);
    if (r >= 0 && a5 && a6) {
        uint32_t two = 2;           /* the sender has no name */
        uint16_t fam = AF_UNIX;
        uint32_t have;
        if (get_user32(&have, a6) < 0 ||
            (have >= 2 && copy_to_user((void *)a5, &fam, 2) < 0) ||
            copy_to_user((void *)a6, &two, 4) < 0)
            return -EFAULT;
    }
    return r;
}

static int read_msg(uintptr_t umsg, struct user_msghdr *m, struct user_iovec *iov)
{
    if (copy_from_user(m, (const void *)umsg, sizeof *m) < 0)
        return -EFAULT;
    if (m->iovlen > IOV_MAX_K)
        return -EMSGSIZE;
    if (m->iovlen && copy_from_user(iov, (const void *)(uintptr_t)m->iov,
                                    m->iovlen * sizeof *iov) < 0)
        return -EFAULT;
    return 0;
}

SYSCALL_DEF(unix_sendmsg)
{
    struct file *f;
    struct usock *s = usock_of(a1, &f);
    struct user_msghdr m;
    struct user_iovec iov[IOV_MAX_K];
    int err = read_msg(a2, &m, iov);
    if (err)
        return err;
    if (m.controllen >= sizeof(struct user_cmsghdr)) {
        struct user_cmsghdr cm;
        if (copy_from_user(&cm, (const void *)(uintptr_t)m.control, sizeof cm) < 0)
            return -EFAULT;
        if (cm.level == SOL_SOCKET && cm.type == SCM_RIGHTS)
            return -EOPNOTSUPP;     /* passing fds: not yet */
    }
    int nonblock = (f->flags & O_NONBLOCK) || (a3 & MSG_DONTWAIT);
    if (s->type == SOCK_DGRAM) {     /* all of the iovecs: one datagram */
        struct usock *to = dgram_dest(s, m.name, m.namelen, &err);
        if (!to)
            return err;
        int32_t r = dgram_send(s, to, iov, (uint32_t)m.iovlen, nonblock);
        uput(to);
        return r;
    }
    int32_t done = 0;
    for (uint32_t i = 0; i < m.iovlen; i++) {
        if (!iov[i].len)
            continue;
        int32_t r = usock_write(s, (const uint8_t *)(uintptr_t)iov[i].base, iov[i].len,
                                nonblock || done, a3 & MSG_NOSIGNAL);
        if (r < 0)
            return done ? done : r;
        done += r;
        if ((uint32_t)r < iov[i].len)
            break;
    }
    return done;
}

SYSCALL_DEF(unix_recvmsg)
{
    struct file *f;
    struct usock *s = usock_of(a1, &f);
    struct user_msghdr m;
    struct user_iovec iov[IOV_MAX_K];
    int err = read_msg(a2, &m, iov);
    if (err)
        return err;
    int nonblock = (f->flags & O_NONBLOCK) || (a3 & MSG_DONTWAIT);
    if (s->type == SOCK_DGRAM) {
        uint8_t from[PATH_UN];
        uint32_t fromlen;
        int trunc;
        int32_t r = dgram_recv(s, iov, (uint32_t)m.iovlen, a3, nonblock, from, &fromlen, &trunc);
        if (r < 0)
            return r;
        uint32_t fl = trunc ? MSG_TRUNC : 0;
        user_addr_t none = 0;
        if ((m.name && (err = write_name(m.name, a2 + offsetof(struct user_msghdr, namelen),
                                         from, fromlen))) ||
            copy_to_user((void *)(a2 + offsetof(struct user_msghdr, controllen)),
                         &none, sizeof none) < 0 ||
            copy_to_user((void *)(a2 + offsetof(struct user_msghdr, flags)), &fl, 4) < 0)
            return err ? err : -EFAULT;
        return r;
    }
    int32_t done = 0;
    for (uint32_t i = 0; i < m.iovlen; i++) {
        if (!iov[i].len)
            continue;
        int32_t r = usock_read(s, (uint8_t *)(uintptr_t)iov[i].base, iov[i].len,
                               nonblock || done, a3 & MSG_PEEK);
        if (r < 0)
            return done ? done : r;
        done += r;
        if ((uint32_t)r < iov[i].len || (a3 & MSG_PEEK))
            break;
    }
    uint32_t zero = 0;
    user_addr_t none = 0;
    if ((m.name && copy_to_user((void *)(a2 + offsetof(struct user_msghdr, namelen)),
                                &zero, 4) < 0) ||
        copy_to_user((void *)(a2 + offsetof(struct user_msghdr, controllen)),
                     &none, sizeof none) < 0 ||
        copy_to_user((void *)(a2 + offsetof(struct user_msghdr, flags)), &zero, 4) < 0)
        return -EFAULT;
    return done;
}

SYSCALL_DEF(unix_setsockopt)
{
    (void)a1;
    return 0;                       /* SO_PASSCRED, buffer sizes: accepted, ignored */
}

SYSCALL_DEF(unix_getsockopt)
{
    struct usock *s = usock_of(a1, NULL);
    if (a2 != SOL_SOCKET)
        return -ENOPROTOOPT;
    uint32_t val, size = 4;
    const void *p = &val;
    switch (a3) {
    case SO_TYPE:       val = (uint32_t)s->type; break;
    case SO_ERROR:      val = 0; break;
    case SO_SNDBUF:
    case SO_RCVBUF:     val = s->type == SOCK_DGRAM ? DQ_BYTES : BUF_SIZE; break;
    case SO_ACCEPTCONN: val = s->state == U_LISTEN; break;
    case SO_PEERCRED:
        if (s->state != U_CONNECTED)
            return -ENOTCONN;
        p = &s->peercred, size = sizeof s->peercred;
        break;
    default:            return -ENOPROTOOPT;
    }
    uint32_t len;
    if (get_user32(&len, a5) < 0)
        return -EFAULT;
    if ((int32_t)len < 0)
        return -EINVAL;
    if (len > size)
        len = size;
    if (copy_to_user((void *)a4, p, len) < 0 || copy_to_user((void *)a5, &len, 4) < 0)
        return -EFAULT;
    return 0;
}

SYSCALL_DEF(unix_shutdown)
{
    struct usock *s = usock_of(a1, NULL);
    if (a2 > 2)
        return -EINVAL;
    if (s->state != U_CONNECTED)
        return -ENOTCONN;
    if (a2 != 1)                    /* SHUT_RD, SHUT_RDWR */
        s->rd_shut = 1;
    if (a2 != 0) {                  /* SHUT_WR, SHUT_RDWR: the peer sees EOF */
        s->wr_shut = 1;
        if (s->peer) {
            s->peer->rd_shut = 1;
            wake(s->peer);
        }
    }
    wake(s);
    return 0;
}

/* socketpair(AF_UNIX, type, 0, int sv[2]) */
int32_t unix_socketpair(uint32_t type, uint32_t protocol, uintptr_t usv)
{
    uint32_t t = type & SOCK_TYPE_MASK;
    if (type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;
    if (t != SOCK_STREAM && t != SOCK_DGRAM)
        return -EOPNOTSUPP;
    if (protocol != 0)
        return -EPROTONOSUPPORT;
    struct usock *a = usock_new((int)t), *b = usock_new((int)t);
    if (!a || !b) {
        if (a) uput(a);
        if (b) uput(b);
        return -ENOMEM;
    }
    if (t == SOCK_DGRAM) {          /* each sends to the other */
        a->peer = b, b->peer = a;
        uget(a), uget(b);
    } else {
        pair(a, b);
    }
    a->peercred = b->cred;
    b->peercred = a->cred;
    int fa = install(a, type);
    if (fa < 0) {
        usock_close(b);
        return fa;
    }
    int fb = install(b, type);
    if (fb < 0) {
        fd_close(fa);
        return fb;
    }
    int32_t sv[2] = { fa, fb };
    if (copy_to_user((void *)usv, sv, sizeof sv) < 0) {
        fd_close(fa);
        fd_close(fb);
        return -EFAULT;
    }
    return 0;
}
