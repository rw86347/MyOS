/*
 * socket.c — the socket syscalls (step 10)
 *
 * A socket is an open file whose ops are sock_ops: read/write/poll/close
 * work on it like on a pipe, and dup/fork share it. Datagrams that arrive
 * wait in a ring of 8 per socket; a ninth is dropped, so a flood costs a
 * bounded amount of memory. Every user pointer goes through copy_from_user
 * / copy_to_user, and every length is checked before it is used.
 *
 * Waiting: syscalls run with interrupts off. A blocked call runs
 * net_rx_action itself (the packet may already be in the card's ring),
 * then sleeps on net_wait, which the e1000 interrupt wakes.
 *
 * Supported: AF_INET with SOCK_STREAM (TCP, client side: tcp.c), SOCK_DGRAM
 * (UDP) and SOCK_RAW + IPPROTO_ICMP (ping). AF_INET6 says EAFNOSUPPORT, which musl's resolver expects on a
 * machine without IPv6.
 */
#include <errno.h>
#include <interrupts.h>
#include <ipv4.h>
#include <kheap.h>
#include <netdevice.h>
#include <proc.h>
#include <signal.h>
#include <socket.h>
#include <string.h>
#include <thread.h>
#include <uaccess.h>
#include <tcp.h>
#include <udp.h>
#include <vfs.h>

#define AF_UNSPEC 0
#define AF_INET   2

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define SOCK_TYPE_MASK 0xf
#define SOCK_NONBLOCK  04000
#define SOCK_CLOEXEC   02000000

#define MSG_PEEK     0x02
#define MSG_TRUNC    0x20
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000

#define SOL_SOCKET 1
#define SO_TYPE    3
#define SO_ERROR   4
#define SO_SNDBUF  7
#define SO_RCVBUF  8

#define FIONREAD 0x541B
#define FIONBIO  0x5421

#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010

#define RXQ      8
#define RAW_MAX  1480               /* ICMP message, without the IP header */
#define UDP_MAX  1472

struct sockaddr_in {
    uint16_t family;
    uint16_t port;                  /* network order */
    uint32_t addr;                  /* network order */
    uint8_t zero[8];
};

struct iovec32 { uint32_t base, len; };

struct msghdr32 {
    uint32_t name, namelen;
    uint32_t iov, iovlen;
    uint32_t control, controllen;
    int32_t flags;
};

struct dgram {
    uint32_t src;                   /* host order */
    uint16_t sport;
    uint16_t len;
    uint8_t data[];
};

struct sock {
    int type;
    uint16_t lport;                 /* UDP: bound port, 0 if none */
    int connected;
    uint32_t raddr;                 /* host order */
    uint16_t rport;
    struct dgram *q[RXQ];
    unsigned head, count;
    struct sock *raw_next;
    struct tcp_conn *tcp;           /* SOCK_STREAM */
};

static struct sock *raw_list;

static struct inode sock_inode = {
    .ino = 0, .mode = S_IFSOCK | 0777, .nlink = 1, .name = "socket", .name_len = 6,
};

static const struct file_ops sock_ops;

/* --- receive queue --------------------------------------------------- */

static void wake(void)
{
    uint32_t f = irq_save();
    wq_wake_all(&net_wait);
    wq_wake_all(&poll_wait);
    irq_restore(f);
}

static void enqueue(struct sock *s, uint32_t src, uint16_t sport,
                    const uint8_t *data, uint32_t len)
{
    if (s->count == RXQ || len > 0xFFFF)
        return;                     /* full: drop, like a busy card would */
    struct dgram *d = kmalloc(sizeof *d + (len ? len : 1));
    if (!d)
        return;
    d->src = src;
    d->sport = sport;
    d->len = (uint16_t)len;
    memcpy(d->data, data, len);
    s->q[(s->head + s->count) % RXQ] = d;
    s->count++;
    wake();
}

/* Put a peeked datagram back at the front of the queue. If it filled up
   while we were copying, the datagram is dropped: losing one is allowed for
   UDP, and it beats leaving a pointer that the next reader would free. */
static void requeue(struct sock *s, struct dgram *d)
{
    uint32_t at = (s->head + RXQ - 1) % RXQ;
    if (s->count < RXQ && !s->q[at]) {
        s->q[at] = d;
        s->head = at;
        s->count++;
    } else {
        kfree(d);
    }
}

static void dequeue(struct sock *s)
{
    kfree(s->q[s->head]);
    s->q[s->head] = 0;
    s->head = (s->head + 1) % RXQ;
    s->count--;
}

static void udp_deliver(void *ctx, uint32_t src, uint16_t sport,
                        const uint8_t *data, uint32_t len)
{
    struct sock *s = ctx;
    if (s->connected && (src != s->raddr || sport != s->rport))
        return;
    enqueue(s, src, sport, data, len);
}

void raw_icmp_input(const uint8_t *ip, uint32_t len)
{
    if (len < 20)
        return;
    uint32_t src = ((uint32_t)ip[12] << 24) | ((uint32_t)ip[13] << 16) |
                   ((uint32_t)ip[14] << 8) | ip[15];
    for (struct sock *s = raw_list; s; s = s->raw_next)
        if (!s->connected || s->raddr == src)
            enqueue(s, src, 0, ip, len);
}

/* Wait until a datagram is queued. 0, -EAGAIN, or -EINTR. */
static int wait_rx(struct sock *s, int nonblock)
{
    for (;;) {
        net_rx_action();
        if (s->count)
            return 0;
        if (nonblock)
            return -EAGAIN;
        if (signal_pending())
            return -EINTR;
        net_sleep(50);
    }
}

/* --- helpers ----------------------------------------------------------- */

static struct sock *sock_of(uint32_t fd, struct file **fp)
{
    struct file *f = fd_get((int)fd);
    if (fp)
        *fp = f;
    if (!f)
        return 0;
    return f->ops == &sock_ops ? f->priv : 0;
}

/* The error for an fd that is not a socket we can use. */
static int32_t bad_fd(uint32_t fd)
{
    return fd_get((int)fd) ? -ENOTSOCK : -EBADF;
}

static int read_addr(uint32_t uaddr, uint32_t alen, uint32_t *ip, uint16_t *port, int *unspec)
{
    struct sockaddr_in sa;
    if (alen < 2 || !uaddr)
        return -EINVAL;
    memset(&sa, 0, sizeof sa);
    if (copy_from_user(&sa, (const void *)uaddr, alen < sizeof sa ? alen : sizeof sa) < 0)
        return -EFAULT;
    if (unspec)
        *unspec = sa.family == AF_UNSPEC;
    if (sa.family == AF_UNSPEC && unspec)
        return 0;
    if (sa.family != AF_INET)
        return -EAFNOSUPPORT;
    if (alen < 8)
        return -EINVAL;
    *ip = ntohl(sa.addr);
    *port = ntohs(sa.port);
    return 0;
}

/* Write an address back: copy min(*len, 16) bytes, then *len = 16. */
static int write_addr(uint32_t uaddr, uint32_t ulenp, uint32_t ip, uint16_t port)
{
    if (!uaddr || !ulenp)
        return 0;
    uint32_t alen;
    if (get_user32(&alen, ulenp) < 0)
        return -EFAULT;
    if ((int32_t)alen < 0)
        return -EINVAL;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.family = AF_INET;
    sa.port = htons(port);
    sa.addr = htonl(ip);
    uint32_t n = alen < sizeof sa ? alen : sizeof sa;
    uint32_t full = sizeof sa;
    if (copy_to_user((void *)uaddr, &sa, n) < 0 ||
        copy_to_user((void *)ulenp, &full, 4) < 0)
        return -EFAULT;
    return 0;
}

static int autobind(struct sock *s)
{
    if (s->type != SOCK_DGRAM || s->lport)
        return 0;
    uint16_t p = udp_free_port();
    if (!p)
        return -EADDRINUSE;
    int rc = udp_bind(p, udp_deliver, s);
    if (rc < 0)
        return rc;
    s->lport = p;
    return 0;
}

/*
 * What a raw socket may put on the wire.
 *
 * A raw ICMP socket that can send any message type is a weapon aimed at the
 * rest of the network rather than at this machine. An ICMP redirect reroutes a
 * neighbour's traffic through whoever sent it. A forged "fragmentation
 * needed" or "destination unreachable" tears down connections between two
 * other hosts — the ICMP blind-reset attack. Linux keeps all of that behind
 * CAP_NET_RAW, and MyOS has no users to withhold a capability from.
 *
 * But only one program ever wants a raw socket, and ping sends exactly one
 * thing: an echo request. Allowing exactly that removes the attack instead of
 * gating it, and costs nothing we have a use for. The length check is here
 * because an ICMP message shorter than its own header is not one.
 *
 * When users exist, this is the place to ask for CAP_NET_RAW and let a
 * privileged program send the rest. docs/security.md has the entry.
 */
static int raw_send_ok(const uint8_t *buf, uint32_t len)
{
    return len >= 8 && buf[0] == 8 /* echo request */ && buf[1] == 0;
}

/* Send len bytes already in kernel memory. Returns len or -errno. */
static int32_t send_k(struct sock *s, const uint8_t *buf, uint32_t len,
                      uint32_t ip, uint16_t port)
{
    if ((ip >> 24) == 127 || ip == 0)
        return -ENETUNREACH;        /* no loopback IP yet */
    int rc;
    if (s->type == SOCK_DGRAM) {
        if (!port)
            return -EINVAL;
        rc = autobind(s);
        if (rc == 0)
            rc = udp_sendto(ip, s->lport, port, buf, len);
    } else {
        if (!raw_send_ok(buf, len))
            return -EPERM;          /* what Linux returns without the capability */
        rc = ip_send(ip, IPPROTO_ICMP, buf, len);
    }
    if (rc == -EIO)
        rc = -ENETUNREACH;          /* ARP got no answer */
    return rc < 0 ? rc : (int32_t)len;
}

static uint32_t max_len(struct sock *s)
{
    return s->type == SOCK_DGRAM ? UDP_MAX : RAW_MAX;
}

/* The most one send or receive on this socket can move, which is also the
   most of a user buffer worth faulting in: a datagram is bounded by its
   protocol's maximum, a stream by the ring in tcp.c. Probing further would
   allocate pages the call cannot reach. */
static uint32_t move_max(struct sock *s)
{
    return s->type == SOCK_STREAM ? tcp_buf_max() : max_len(s);
}

/*
 * Fault in the buffers a send or receive is about to use, before the socket's
 * own state has been looked at. The copies below must not be able to sleep:
 * by the time they run, a datagram has been taken off the queue or a window
 * has been consumed, and a second process entering here would see state that
 * is mid-update. fault_in_user() in uaccess.h has the long version.
 *
 * `write` is the kernel's direction: 1 for a receive.
 */
static int fault_in_iov(const struct iovec32 *iov, uint32_t n, uint32_t cap, int write)
{
    for (uint32_t i = 0; i < n && cap; i++) {
        uint32_t len = iov[i].len < cap ? iov[i].len : cap;
        if (len && fault_in_user((const void *)iov[i].base, len, write) < 0)
            return -EFAULT;
        cap -= len;
    }
    return 0;
}

/* A stream write to a closed connection also raises SIGPIPE (Linux). */
static int32_t stream_send(struct sock *s, uint32_t ubuf, uint32_t len,
                           int nonblock, uint32_t flags)
{
    int rc = tcp_write(s->tcp, (const void *)ubuf, len, 1, nonblock);
    if (rc == -EPIPE && !(flags & MSG_NOSIGNAL))
        signal_send(proc_current(), SIGPIPE);
    return rc;
}

static int32_t do_send(struct sock *s, uint32_t ubuf, uint32_t len,
                       uint32_t uaddr, uint32_t alen, int nonblock, uint32_t flags)
{
    if (s->type == SOCK_STREAM)
        return stream_send(s, ubuf, len, nonblock, flags);
    uint32_t ip = s->raddr;
    uint16_t port = s->rport;
    if (uaddr) {
        int rc = read_addr(uaddr, alen, &ip, &port, 0);
        if (rc < 0)
            return rc;
    } else if (!s->connected) {
        return -EDESTADDRREQ;
    }
    if (len > max_len(s))
        return -EMSGSIZE;
    uint8_t *k = kmalloc(len ? len : 1);
    if (!k)
        return -ENOMEM;
    int32_t rc = copy_from_user(k, (const void *)ubuf, len) < 0 ? -EFAULT
                                                                : send_k(s, k, len, ip, port);
    kfree(k);
    return rc;
}

/* Copy the next datagram out. iov: n user buffers. Returns bytes copied
   (or the full length with MSG_TRUNC), or -errno. */
static int32_t do_recv(struct sock *s, struct file *f, const struct iovec32 *iov,
                       uint32_t niov, uint32_t flags, uint32_t uaddr, uint32_t ulenp,
                       int32_t *msg_flags)
{
    int nonblock = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    if (s->type == SOCK_STREAM) {
        /* Block for the first buffer only, then take what is there. */
        int32_t done = 0;
        for (uint32_t i = 0; i < niov; i++) {
            if (!iov[i].len)
                continue;
            int r = tcp_read(s->tcp, (void *)iov[i].base, iov[i].len, 1,
                             nonblock || done, flags & MSG_PEEK, 0);
            if (r < 0)
                return done ? done : r;
            done += r;
            if ((uint32_t)r < iov[i].len || (flags & MSG_PEEK))
                break;
        }
        if (msg_flags)
            *msg_flags = 0;
        return done;
    }
    int rc = wait_rx(s, nonblock);
    if (rc < 0)
        return rc;
    /* Take the datagram out of the queue before copying out of it. Copying
       can sleep -- copy_to_user faults on a page of an mmap'd file that is
       not resident, and mm_fault reads it from the disk and waits -- so a
       second reader on the same socket (a fork shares it) runs here in the
       middle. Were the queue advanced afterwards, both would take this same
       pointer: the first to finish frees the datagram, the other keeps
       reading it and copying freed memory out to user space, then frees it
       again and takes s->count below zero, which makes wait_rx report data
       on an empty queue and the next read dereference NULL in ring 0.
       Holding it out of the queue for the duration makes this one the owner. */
    struct dgram *d = s->q[s->head];
    s->q[s->head] = 0;
    s->head = (s->head + 1) % RXQ;
    s->count--;

    uint32_t done = 0, full = d->len;
    int32_t rc2 = -EFAULT;
    for (uint32_t i = 0; i < niov && done < full; i++) {
        uint32_t n = full - done < iov[i].len ? full - done : iov[i].len;
        if (copy_to_user((void *)iov[i].base, d->data + done, n) < 0)
            goto out;
        done += n;
    }
    if (write_addr(uaddr, ulenp, d->src, d->sport) < 0)
        goto out;
    if (msg_flags)
        *msg_flags = done < full ? MSG_TRUNC : 0;
    rc2 = (int32_t)((flags & MSG_TRUNC) ? full : done);
out:
    if (flags & MSG_PEEK)
        requeue(s, d);              /* a peek does not consume it */
    else
        kfree(d);
    return rc2;
}

/* --- file ops ------------------------------------------------------------ */

static int32_t sock_read(struct file *f, void *ubuf, uint32_t n)
{
    struct iovec32 v = { (uint32_t)ubuf, n };
    return do_recv(f->priv, f, &v, 1, 0, 0, 0, 0);
}

static int32_t sock_write(struct file *f, const void *ubuf, uint32_t n)
{
    return do_send(f->priv, (uint32_t)ubuf, n, 0, 0, f->flags & O_NONBLOCK, 0);
}

static uint32_t sock_poll(struct file *f)
{
    struct sock *s = f->priv;
    net_rx_action();
    if (s->type == SOCK_STREAM) {
        unsigned t = tcp_poll(s->tcp);
        uint32_t r = (t & TCP_READABLE ? POLLIN : 0) | (t & TCP_WRITABLE ? POLLOUT : 0);
        if (tcp_error(s->tcp, 0))
            r |= POLLERR;
        if ((t & TCP_READABLE) && (t & TCP_WRITABLE) && !tcp_connected(s->tcp))
            r |= POLLHUP;
        return r;
    }
    return (s->count ? POLLIN : 0) | POLLOUT;
}

static int32_t sock_ioctl(struct file *f, uint32_t cmd, uint32_t arg)
{
    struct sock *s = f->priv;
    if (cmd == FIONREAD) {
        net_rx_action();
        uint32_t n = s->type == SOCK_STREAM ? tcp_rx_avail(s->tcp)
                   : s->count ? s->q[s->head]->len : 0;
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

static void sock_release(struct file *f)
{
    struct sock *s = f->priv;
    if (s->lport)
        udp_unbind(s->lport);
    for (struct sock **pp = &raw_list; *pp; pp = &(*pp)->raw_next) {
        if (*pp == s) {
            *pp = s->raw_next;
            break;
        }
    }
    while (s->count)
        dequeue(s);
    if (s->tcp)
        tcp_release(s->tcp);
    kfree(s);
}

static const struct file_ops sock_ops = {
    .read = sock_read,
    .write = sock_write,
    .release = sock_release,
    .ioctl = sock_ioctl,
    .poll = sock_poll,
};

/* --- the syscalls -------------------------------------------------------- */

/* int socket(int domain, int type, int protocol) */
SYSCALL_DEF(sys_socket)
{
    if (a1 != AF_INET)
        return -EAFNOSUPPORT;
    uint32_t type = a2 & SOCK_TYPE_MASK;
    if (a2 & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;
    if (type == SOCK_DGRAM) {
        if (a3 != 0 && a3 != IPPROTO_UDP)
            return -EPROTONOSUPPORT;
    } else if (type == SOCK_RAW) {
        if (a3 != IPPROTO_ICMP)
            return -EPROTONOSUPPORT;
    } else if (type == SOCK_STREAM) {
        if (a3 != 0 && a3 != IPPROTO_TCP)
            return -EPROTONOSUPPORT;
    } else {
        return -EINVAL;
    }
    struct sock *s = kmalloc(sizeof *s);
    if (!s)
        return -ENOMEM;
    memset(s, 0, sizeof *s);
    s->type = (int)type;
    if (type == SOCK_STREAM && !(s->tcp = tcp_open())) {
        kfree(s);
        return -ENOMEM;             /* all TCP_MAX connections in use */
    }
    struct file *f = file_new(&sock_ops, &sock_inode, O_RDWR | (a2 & SOCK_NONBLOCK));
    if (!f) {
        kfree(s);
        return -ENOMEM;
    }
    f->priv = s;
    int fd = fd_install(f);
    if (fd < 0) {
        file_put(f);                /* release frees s */
        return -EMFILE;
    }
    if (type == SOCK_RAW) {
        s->raw_next = raw_list;
        raw_list = s;
    }
    if (a2 & SOCK_CLOEXEC)
        proc_current()->cloexec |= 1u << fd;
    return fd;
}

/* int bind(int fd, const struct sockaddr *addr, socklen_t len) */
SYSCALL_DEF(sys_bind)
{
    struct sock *s = sock_of(a1, 0);
    if (!s)
        return bad_fd(a1);
    uint32_t ip;
    uint16_t port;
    int rc = read_addr(a2, a3, &ip, &port, 0);
    if (rc < 0)
        return rc;
    if (ip != 0 && ip != ipv4_addr)
        return -EADDRNOTAVAIL;
    if (s->type != SOCK_DGRAM)
        return 0;
    if (s->lport)
        return -EINVAL;
    if (!port)
        return autobind(s);
    rc = udp_bind(port, udp_deliver, s);
    if (rc < 0)
        return rc == -ENOMEM ? -EADDRINUSE : rc;
    s->lport = port;
    return 0;
}

/* int connect(int fd, const struct sockaddr *addr, socklen_t len)
   UDP and raw: remember the peer. AF_UNSPEC forgets it. */
SYSCALL_DEF(sys_connect)
{
    struct file *f;
    struct sock *s = sock_of(a1, &f);
    if (!s)
        return bad_fd(a1);
    uint32_t ip = 0;
    uint16_t port = 0;
    int unspec = 0;
    int rc = read_addr(a2, a3, &ip, &port, &unspec);
    if (rc < 0)
        return rc;
    if (unspec && s->type == SOCK_STREAM)
        return -EAFNOSUPPORT;
    if (unspec) {
        s->connected = 0;
        return 0;
    }
    if (s->type == SOCK_STREAM) {
        if ((ip >> 24) == 127 || ip == 0 || !port)
            return (ip >> 24) == 127 || ip == 0 ? -ENETUNREACH : -EINVAL;
        return tcp_connect(s->tcp, ip, port, f->flags & O_NONBLOCK);
    }
    if (s->type == SOCK_DGRAM && !port)
        return -EINVAL;
    if ((ip >> 24) == 127 || ip == 0)
        return -ENETUNREACH;
    rc = autobind(s);
    if (rc < 0)
        return rc;
    s->raddr = ip;
    s->rport = port;
    s->connected = 1;
    return 0;
}

/* int getsockname(int fd, struct sockaddr *addr, socklen_t *len) */
SYSCALL_DEF(sys_getsockname)
{
    struct sock *s = sock_of(a1, 0);
    if (!s)
        return bad_fd(a1);
    if (s->type == SOCK_STREAM) {
        uint16_t lp, rp;
        uint32_t ra;
        tcp_names(s->tcp, &lp, &ra, &rp);
        return write_addr(a2, a3, lp ? ipv4_addr : 0, lp);
    }
    return write_addr(a2, a3, s->connected ? ipv4_addr : 0, s->lport);
}

SYSCALL_DEF(sys_getpeername)
{
    struct sock *s = sock_of(a1, 0);
    if (!s)
        return bad_fd(a1);
    if (s->type == SOCK_STREAM) {
        uint16_t lp, rp;
        uint32_t ra;
        if (!tcp_connected(s->tcp))
            return -ENOTCONN;
        tcp_names(s->tcp, &lp, &ra, &rp);
        return write_addr(a2, a3, ra, rp);
    }
    if (!s->connected)
        return -ENOTCONN;
    return write_addr(a2, a3, s->raddr, s->rport);
}

/* ssize_t sendto(int fd, const void *buf, size_t len, int flags,
                  const struct sockaddr *to, socklen_t tolen) */
SYSCALL_DEF(sys_sendto)
{
    struct file *f;
    struct sock *s = sock_of(a1, &f);
    if (!s)
        return bad_fd(a1);
    struct iovec32 v = { a2, a3 };
    if (fault_in_iov(&v, 1, move_max(s), 0) < 0)
        return -EFAULT;
    return do_send(s, a2, a3, a5, a6, (f->flags & O_NONBLOCK) || (a4 & MSG_DONTWAIT), a4);
}

/* ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                    struct sockaddr *from, socklen_t *fromlen) */
SYSCALL_DEF(sys_recvfrom)
{
    struct file *f;
    struct sock *s = sock_of(a1, &f);
    if (!s)
        return bad_fd(a1);
    struct iovec32 v = { a2, a3 };
    if (fault_in_iov(&v, 1, move_max(s), 1) < 0)
        return -EFAULT;
    return do_recv(s, f, &v, 1, a4, a5, a6, 0);
}

#define IOV_MAX_K 16

static int read_msg(uint32_t umsg, struct msghdr32 *m, struct iovec32 *iov)
{
    if (copy_from_user(m, (const void *)umsg, sizeof *m) < 0)
        return -EFAULT;
    if (m->iovlen > IOV_MAX_K)
        return -EMSGSIZE;
    if (m->iovlen && copy_from_user(iov, (const void *)m->iov, m->iovlen * sizeof *iov) < 0)
        return -EFAULT;
    return 0;
}

/* ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) */
SYSCALL_DEF(sys_sendmsg)
{
    struct file *f;
    struct sock *s = sock_of(a1, &f);
    if (!s)
        return bad_fd(a1);
    struct msghdr32 m;
    struct iovec32 iov[IOV_MAX_K];
    int rc = read_msg(a2, &m, iov);
    if (rc < 0)
        return rc;
    if (fault_in_iov(iov, m.iovlen, move_max(s), 0) < 0)
        return -EFAULT;
    if (s->type == SOCK_STREAM) {
        int nonblock = (f->flags & O_NONBLOCK) || (a3 & MSG_DONTWAIT);
        int32_t done = 0;
        for (uint32_t i = 0; i < m.iovlen; i++) {
            if (!iov[i].len)
                continue;
            int32_t r = stream_send(s, iov[i].base, iov[i].len, nonblock || done, a3);
            if (r < 0)
                return done ? done : r;
            done += r;
            if ((uint32_t)r < iov[i].len)
                break;
        }
        return done;
    }
    uint32_t ip = s->raddr;
    uint16_t port = s->rport;
    if (m.name) {
        rc = read_addr(m.name, m.namelen, &ip, &port, 0);
        if (rc < 0)
            return rc;
    } else if (!s->connected) {
        return -EDESTADDRREQ;
    }
    uint32_t total = 0;
    for (uint32_t i = 0; i < m.iovlen; i++) {
        if (iov[i].len > max_len(s) || total + iov[i].len > max_len(s))
            return -EMSGSIZE;
        total += iov[i].len;
    }
    uint8_t *k = kmalloc(total ? total : 1);
    if (!k)
        return -ENOMEM;
    uint32_t at = 0;
    for (uint32_t i = 0; i < m.iovlen; i++) {
        if (copy_from_user(k + at, (const void *)iov[i].base, iov[i].len) < 0) {
            kfree(k);
            return -EFAULT;
        }
        at += iov[i].len;
    }
    int32_t r = send_k(s, k, total, ip, port);
    kfree(k);
    return r;
}

/* ssize_t recvmsg(int fd, struct msghdr *msg, int flags) */
SYSCALL_DEF(sys_recvmsg)
{
    struct file *f;
    struct sock *s = sock_of(a1, &f);
    if (!s)
        return bad_fd(a1);
    struct msghdr32 m;
    struct iovec32 iov[IOV_MAX_K];
    int rc = read_msg(a2, &m, iov);
    if (rc < 0)
        return rc;
    if (fault_in_iov(iov, m.iovlen, move_max(s), 1) < 0)
        return -EFAULT;
    int32_t mflags = 0;
    uint32_t namelen_at = a2 + 4;   /* &msg->msg_namelen */
    int32_t r = do_recv(s, f, iov, m.iovlen, a3, m.name, m.name ? namelen_at : 0, &mflags);
    if (r < 0)
        return r;
    uint32_t zero = 0;
    if (copy_to_user((void *)(a2 + 20), &zero, 4) < 0 ||        /* msg_controllen */
        copy_to_user((void *)(a2 + 24), &mflags, 4) < 0)        /* msg_flags */
        return -EFAULT;
    return r;
}

/* Options we accept and ignore: buffer sizes, broadcast, reuseaddr, TTL.
   The values change nothing here yet. */
SYSCALL_DEF(sys_setsockopt)
{
    if (!sock_of(a1, 0))
        return bad_fd(a1);
    return 0;
}

SYSCALL_DEF(sys_getsockopt)
{
    struct sock *s = sock_of(a1, 0);
    if (!s)
        return bad_fd(a1);
    uint32_t val;
    if (a2 != SOL_SOCKET)
        return -ENOPROTOOPT;
    switch (a3) {
    case SO_TYPE:   val = (uint32_t)s->type; break;
    case SO_ERROR:  val = s->type == SOCK_STREAM ? (uint32_t)-tcp_error(s->tcp, 1) : 0; break;
    case SO_SNDBUF: val = 8192; break;
    case SO_RCVBUF: val = RXQ * 1500; break;
    default:        return -ENOPROTOOPT;
    }
    uint32_t len;
    if (get_user32(&len, a5) < 0)
        return -EFAULT;
    if (len < 4)
        return -EINVAL;
    uint32_t four = 4;
    if (copy_to_user((void *)a4, &val, 4) < 0 || copy_to_user((void *)a5, &four, 4) < 0)
        return -EFAULT;
    return 0;
}

/* int shutdown(int fd, int how): 1 or 2 sends our FIN after the data. */
SYSCALL_DEF(sys_shutdown)
{
    struct sock *s = sock_of(a1, 0);
    if (!s)
        return bad_fd(a1);
    if (a2 > 2)
        return -EINVAL;
    if (s->type == SOCK_STREAM) {
        if (!tcp_connected(s->tcp))
            return -ENOTCONN;
        if (a2 != 0)
            tcp_shutdown_write(s->tcp);
    }
    return 0;
}

SYSCALL_DEF(sys_listen)
{
    return sock_of(a1, 0) ? -EOPNOTSUPP : bad_fd(a1);
}

SYSCALL_DEF(sys_accept4)
{
    return sock_of(a1, 0) ? -EOPNOTSUPP : bad_fd(a1);
}

SYSCALL_DEF(sys_socketpair)
{
    return a1 == AF_INET ? -EOPNOTSUPP : -EAFNOSUPPORT;
}

/* socketcall(call, args): the old one-syscall-for-all. args points at an
   array of the real arguments; how many depends on the call. */
SYSCALL_DEF(sys_socketcall)
{
    static const uint8_t nargs[19] = { 0, 3, 3, 3, 2, 3, 3, 3, 4, 4, 4, 6, 6, 2, 5, 5, 3, 3, 4 };
    if (a1 < 1 || a1 > 18)
        return -EINVAL;
    uint32_t v[6] = { 0 };
    if (copy_from_user(v, (const void *)a2, nargs[a1] * 4u) < 0)
        return -EFAULT;
    switch (a1) {
    case 1:  return sys_socket(v[0], v[1], v[2], 0, 0, 0);
    case 2:  return sys_bind(v[0], v[1], v[2], 0, 0, 0);
    case 3:  return sys_connect(v[0], v[1], v[2], 0, 0, 0);
    case 4:  return sys_listen(v[0], v[1], 0, 0, 0, 0);
    case 5:  return sys_accept4(v[0], v[1], v[2], 0, 0, 0);
    case 6:  return sys_getsockname(v[0], v[1], v[2], 0, 0, 0);
    case 7:  return sys_getpeername(v[0], v[1], v[2], 0, 0, 0);
    case 8:  return sys_socketpair(v[0], v[1], v[2], v[3], 0, 0);
    case 9:  return sys_sendto(v[0], v[1], v[2], v[3], 0, 0);       /* send */
    case 10: return sys_recvfrom(v[0], v[1], v[2], v[3], 0, 0);     /* recv */
    case 11: return sys_sendto(v[0], v[1], v[2], v[3], v[4], v[5]);
    case 12: return sys_recvfrom(v[0], v[1], v[2], v[3], v[4], v[5]);
    case 13: return sys_shutdown(v[0], v[1], 0, 0, 0, 0);
    case 14: return sys_setsockopt(v[0], v[1], v[2], v[3], v[4], 0);
    case 15: return sys_getsockopt(v[0], v[1], v[2], v[3], v[4], 0);
    case 16: return sys_sendmsg(v[0], v[1], v[2], 0, 0, 0);
    case 17: return sys_recvmsg(v[0], v[1], v[2], 0, 0, 0);
    default: return sys_accept4(v[0], v[1], v[2], v[3], 0, 0);
    }
}
