/*
 * packet.c — AF_PACKET: sockets that send and receive link-layer frames
 * (Linux's packet(7)). wpa_supplicant carries its EAPOL key handshake over
 * one; DHCP clients such as udhcpc use them too.
 *
 *   socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_PAE))   payloads: the kernel
 *       adds and removes the Ethernet header; addresses in sockaddr_ll
 *   socket(AF_PACKET, SOCK_RAW, ...)                  whole frames
 *   bind(fd, {AF_PACKET, protocol, ifindex})          one device, one type
 *
 * Receiving: netif_receive_skb shows every frame to packet_rcv first, which
 * copies it to each socket that wants it (its type, or ETH_P_ALL, and its
 * device, or any). O(packet sockets) per frame — there are one or two.
 * Each socket queues at most QUEUE_MAX bytes; past that a frame is dropped
 * for it, as Linux drops when the receive buffer is full.
 *
 * Socket filters (SO_ATTACH_FILTER, BPF) are accepted and not run: a
 * program sees frames a filter would have dropped (docs/gaps.md).
 */
#include <errno.h>
#include <kheap.h>
#include <netdevice.h>
#include <proc.h>
#include <socket.h>
#include <string.h>
#include <thread.h>
#include <uaccess.h>
#include <user.h>
#include <vfs.h>

#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define SOCK_TYPE_MASK  0xf
#define SOCK_NONBLOCK   04000
#define SOCK_CLOEXEC    02000000
#define MSG_PEEK        0x02
#define MSG_TRUNC       0x20
#define MSG_DONTWAIT    0x40
#define SOL_SOCKET      1
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_RCVBUF       8
#define FIONREAD        0x541B
#define FIONBIO         0x5421
#define POLLIN          0x001
#define POLLOUT         0x004

#define ETH_P_ALL       0x0003
#define ARPHRD_ETHER    1
#define PACKET_HOST      0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3

#define QUEUE_MAX       (64 * 1024)     /* bytes waiting to be read, per socket */
#define FRAME_MAX       2048            /* one frame (Ethernet: 1514) */
#define IOV_MAX_K       16

struct sockaddr_ll {                    /* (linux/if_packet.h) */
    uint16_t family;
    uint16_t protocol;                  /* network order */
    int32_t ifindex;
    uint16_t hatype;
    uint8_t pkttype, halen;
    uint8_t addr[8];
};

struct pframe {                         /* one received frame, as the program will see it */
    struct pframe *next;
    struct sockaddr_ll from;
    uint32_t len;
    uint8_t data[];
};

struct psock {
    int type;                           /* SOCK_DGRAM or SOCK_RAW */
    uint16_t protocol;                  /* host order; 0: receive nothing yet */
    int ifindex;                        /* 0: every device */
    struct pframe *head, *tail;
    uint32_t queued;
    struct wait_queue wq;
    struct psock *next;
};

static struct psock *socks;
static const struct file_ops packet_ops;
static struct inode p_inode = {
    .ino = 0, .mode = S_IFSOCK | 0777, .nlink = 1, .name = "socket", .name_len = 6,
};

static struct psock *ps_of(uint32_t fd, struct file **fp)
{
    struct file *f = fd_get((int)fd);
    if (fp)
        *fp = f;
    return f && f->ops == &packet_ops ? f->priv : NULL;
}

int packet_fd(uint32_t fd)
{
    return ps_of(fd, NULL) != NULL;
}

int32_t packet_socket(uint32_t type, uint32_t protocol)
{
    uint32_t t = type & SOCK_TYPE_MASK;
    if (type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC) || (t != SOCK_RAW && t != SOCK_DGRAM))
        return -ESOCKTNOSUPPORT;
    struct psock *s = kmalloc(sizeof *s);
    if (!s)
        return -ENOMEM;
    memset(s, 0, sizeof *s);
    s->type = (int)t;
    s->protocol = ntohs((uint16_t)protocol);
    struct file *f = file_new(&packet_ops, &p_inode, O_RDWR | (type & SOCK_NONBLOCK));
    if (!f) {
        kfree(s);
        return -ENOMEM;
    }
    f->priv = s;
    s->next = socks;
    socks = s;
    int fd = fd_install(f);
    if (fd < 0) {
        file_put(f);                    /* release unlinks and frees s */
        return -EMFILE;
    }
    if (type & SOCK_CLOEXEC)
        proc_current()->cloexec |= 1ull << fd;
    return fd;
}

static void p_release(struct file *f)
{
    struct psock *s = f->priv;
    for (struct psock **pp = &socks; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    while (s->head) {
        struct pframe *b = s->head;
        s->head = b->next;
        kfree(b);
    }
    kfree(s);
}

/* --- receiving -------------------------------------------------------------------- */

/* netif_receive_skb: a frame arrived (its Ethernet header already pulled;
   skb->mac_header still says where it was). Copies for the sockets that
   want it; the frame itself goes on to its protocol. */
void packet_rcv(const struct sk_buff *skb)
{
    if (!socks || skb->mac_header == 0xFFFF || !skb->dev)
        return;
    const uint8_t *mac = skb->head + skb->mac_header;
    uint16_t proto = ntohs(skb->protocol);
    uint32_t paylen = skb->len;
    for (struct psock *s = socks; s; s = s->next) {
        if (!s->protocol || (s->protocol != ETH_P_ALL && s->protocol != proto) ||
            (s->ifindex && s->ifindex != skb->dev->ifindex))
            continue;
        uint32_t len = s->type == SOCK_RAW ? paylen + ETH_HLEN : paylen;
        if (len > FRAME_MAX || s->queued + len > QUEUE_MAX)
            continue;                   /* too big, or the program isn't reading */
        struct pframe *b = kmalloc(sizeof *b + len);
        if (!b)
            continue;
        memset(&b->from, 0, sizeof b->from);
        b->from.family = AF_PACKET;
        b->from.protocol = skb->protocol;
        b->from.ifindex = skb->dev->ifindex;
        b->from.hatype = ARPHRD_ETHER;
        b->from.pkttype = (mac[0] & 1) ? (mac[0] == 0xff ? PACKET_BROADCAST : PACKET_MULTICAST)
                        : memcmp(mac, skb->dev->dev_addr, ETH_ALEN) ? PACKET_OTHERHOST : PACKET_HOST;
        b->from.halen = ETH_ALEN;
        memcpy(b->from.addr, mac + ETH_ALEN, ETH_ALEN);     /* the sender */
        if (s->type == SOCK_RAW) {
            memcpy(b->data, mac, ETH_HLEN);
            memcpy(b->data + ETH_HLEN, skb->data, paylen);
        } else {
            memcpy(b->data, skb->data, paylen);
        }
        b->len = len;
        b->next = NULL;
        if (s->tail)
            s->tail->next = b;
        else
            s->head = b;
        s->tail = b;
        s->queued += len;
        wq_wake_all(&s->wq);
        wq_wake_all(&poll_wait);
    }
}

static int32_t take(struct psock *s, int nonblock, int peek, struct pframe **out)
{
    while (!s->head) {
        if (nonblock)
            return -EAGAIN;
        if (signal_pending())
            return -EINTR;
        wq_sleep(&s->wq);
    }
    struct pframe *b = s->head;
    if (peek) {
        struct pframe *c = kmalloc(sizeof *c + b->len);
        if (!c)
            return -ENOMEM;
        memcpy(c, b, sizeof *c + b->len);
        *out = c;
        return 0;
    }
    s->head = b->next;
    if (!s->head)
        s->tail = NULL;
    s->queued -= b->len;
    *out = b;
    return 0;
}

static int32_t copy_frame(const struct pframe *b, const struct user_iovec *iov, uint32_t niov)
{
    uint32_t done = 0;
    for (uint32_t k = 0; k < niov && done < b->len; k++) {
        uint32_t n = b->len - done < iov[k].len ? b->len - done : (uint32_t)iov[k].len;
        if (n && copy_to_user((void *)(uintptr_t)iov[k].base, b->data + done, n) < 0)
            return -EFAULT;
        done += n;
    }
    return (int32_t)done;
}

/* The sender's address for recvfrom/recvmsg, cut to the program's buffer. */
static int write_from(const struct pframe *b, uintptr_t uaddr, uint32_t have, uint32_t *written)
{
    uint32_t n = sizeof b->from;
    *written = n;
    if (!uaddr)
        return 0;
    return copy_to_user((void *)uaddr, &b->from, have < n ? have : n) < 0 ? -EFAULT : 0;
}

/* --- sending ------------------------------------------------------------------------ */

/* Send bytes from the iovecs as one frame, to `to` (or the bound device). */
static int32_t p_send(struct psock *s, struct file *f, const struct user_iovec *iov, uint32_t niov,
                      uintptr_t uaddr, uint32_t alen)
{
    struct sockaddr_ll to;
    memset(&to, 0, sizeof to);
    if (uaddr) {
        if (alen < offsetof(struct sockaddr_ll, addr) + ETH_ALEN)
            return -EINVAL;
        if (copy_from_user(&to, (const void *)uaddr, alen < sizeof to ? alen : sizeof to) < 0)
            return -EFAULT;
        if (to.family != AF_PACKET)
            return -EINVAL;
    }
    int ifindex = to.ifindex ? to.ifindex : s->ifindex;
    struct net_device *dev = dev_get_by_index(ifindex);
    if (!dev)
        return -ENXIO;
    if (!(dev->flags & IFF_UP))
        return -ENETDOWN;
    uint32_t total = 0;
    for (uint32_t k = 0; k < niov; k++)
        total += (uint32_t)iov[k].len;
    uint32_t hdr = s->type == SOCK_DGRAM ? ETH_HLEN : 0;
    if (total + hdr > dev->mtu + ETH_HLEN || (s->type == SOCK_RAW && total < ETH_HLEN))
        return -EMSGSIZE;
    if (s->type == SOCK_DGRAM && !uaddr)
        return -EDESTADDRREQ;           /* (a datagram needs a destination) */

    struct sk_buff *skb = alloc_skb(total + hdr + 64, 0);
    if (!skb)
        return -ENOBUFS;
    skb_reserve(skb, 64);               /* (room for the Wi-Fi header in front) */
    uint8_t *p = skb_put(skb, total + hdr);
    if (hdr) {
        memcpy(p, to.addr, ETH_ALEN);
        memcpy(p + ETH_ALEN, dev->dev_addr, ETH_ALEN);
        uint16_t proto = to.protocol ? to.protocol : htons(s->protocol);
        memcpy(p + 2 * ETH_ALEN, &proto, 2);
    }
    uint32_t at = hdr;
    for (uint32_t k = 0; k < niov; k++) {
        if (file_copy_in(f, p + at, (const void *)(uintptr_t)iov[k].base, (uint32_t)iov[k].len) < 0) {
            kfree_skb(skb);
            return -EFAULT;
        }
        at += (uint32_t)iov[k].len;
    }
    skb->dev = dev;
    int err = dev_queue_xmit(skb);
    if (err == -EAGAIN) {               /* (its queue is stopped: still ours) */
        kfree_skb(skb);
        return -ENOBUFS;
    }
    return err < 0 ? err : (int32_t)total;
}

/* --- the syscalls --------------------------------------------------------------------- */

SYSCALL_DEF(packet_bind)
{
    struct psock *s = ps_of(a1, NULL);
    struct sockaddr_ll a;
    if (a3 < offsetof(struct sockaddr_ll, hatype))
        return -EINVAL;
    memset(&a, 0, sizeof a);
    if (copy_from_user(&a, (const void *)a2, a3 < sizeof a ? (uint32_t)a3 : sizeof a) < 0)
        return -EFAULT;
    if (a.family != AF_PACKET)
        return -EINVAL;
    if (a.ifindex && !dev_get_by_index(a.ifindex))
        return -ENODEV;
    s->ifindex = a.ifindex;
    if (a.protocol)
        s->protocol = ntohs(a.protocol);
    return 0;
}

SYSCALL_DEF(packet_getsockname)
{
    struct psock *s = ps_of(a1, NULL);
    struct sockaddr_ll a;
    memset(&a, 0, sizeof a);
    a.family = AF_PACKET;
    a.protocol = htons(s->protocol);
    a.ifindex = s->ifindex;
    a.hatype = ARPHRD_ETHER;
    struct net_device *dev = dev_get_by_index(s->ifindex);
    if (dev) {
        a.halen = ETH_ALEN;
        memcpy(a.addr, dev->dev_addr, ETH_ALEN);
    }
    uint32_t have, n = sizeof a;
    if (get_user32(&have, a3) < 0)
        return -EFAULT;
    if (copy_to_user((void *)a2, &a, have < n ? have : n) < 0 || copy_to_user((void *)a3, &n, 4) < 0)
        return -EFAULT;
    return 0;
}

/* sendto(fd, buf, len, flags, dest, destlen) */
SYSCALL_DEF(packet_sendto)
{
    struct file *f;
    struct psock *s = ps_of(a1, &f);
    struct user_iovec iov = { (user_addr_t)a2, (user_addr_t)a3 };
    return p_send(s, f, &iov, 1, a5, (uint32_t)a6);
}

SYSCALL_DEF(packet_sendmsg)
{
    struct file *f;
    struct psock *s = ps_of(a1, &f);
    struct user_msghdr m;
    struct user_iovec iov[IOV_MAX_K];
    if (copy_from_user(&m, (const void *)a2, sizeof m) < 0)
        return -EFAULT;
    if (m.iovlen > IOV_MAX_K)
        return -EMSGSIZE;
    if (m.iovlen && copy_from_user(iov, (const void *)(uintptr_t)m.iov, m.iovlen * sizeof iov[0]) < 0)
        return -EFAULT;
    return p_send(s, f, iov, (uint32_t)m.iovlen, m.name, m.namelen);
}

/* recvfrom(fd, buf, len, flags, src, srclen) */
SYSCALL_DEF(packet_recvfrom)
{
    struct file *f;
    struct psock *s = ps_of(a1, &f);
    struct pframe *b;
    int32_t r = take(s, (f->flags & O_NONBLOCK) || (a4 & MSG_DONTWAIT), a4 & MSG_PEEK, &b);
    if (r < 0)
        return r;
    struct user_iovec iov = { (user_addr_t)a2, (user_addr_t)a3 };
    r = copy_frame(b, &iov, 1);
    uint32_t full = b->len, have = 0, n;
    if (r >= 0 && a5 && (get_user32(&have, a6) < 0 || write_from(b, a5, have, &n) < 0 ||
                         copy_to_user((void *)a6, &n, 4) < 0))
        r = -EFAULT;
    kfree(b);
    return r < 0 ? r : (a4 & MSG_TRUNC) ? (int32_t)full : r;
}

SYSCALL_DEF(packet_recvmsg)
{
    struct file *f;
    struct psock *s = ps_of(a1, &f);
    struct user_msghdr m;
    struct user_iovec iov[IOV_MAX_K];
    if (copy_from_user(&m, (const void *)a2, sizeof m) < 0)
        return -EFAULT;
    if (m.iovlen > IOV_MAX_K)
        return -EMSGSIZE;
    if (m.iovlen && copy_from_user(iov, (const void *)(uintptr_t)m.iov, m.iovlen * sizeof iov[0]) < 0)
        return -EFAULT;
    struct pframe *b;
    int32_t r = take(s, (f->flags & O_NONBLOCK) || (a3 & MSG_DONTWAIT), a3 & MSG_PEEK, &b);
    if (r < 0)
        return r;
    r = copy_frame(b, iov, (uint32_t)m.iovlen);
    uint32_t full = b->len, namelen = 0;
    int32_t flags = r >= 0 && (uint32_t)r < full ? MSG_TRUNC : 0;
    user_addr_t clen = 0;
    if (r >= 0 && write_from(b, m.name, m.namelen, &namelen) < 0)
        r = -EFAULT;
    kfree(b);
    if (r < 0)
        return r;
    if (copy_to_user((void *)(a2 + offsetof(struct user_msghdr, namelen)), &namelen, 4) < 0 ||
        copy_to_user((void *)(a2 + offsetof(struct user_msghdr, controllen)), &clen, sizeof clen) < 0 ||
        copy_to_user((void *)(a2 + offsetof(struct user_msghdr, flags)), &flags, 4) < 0)
        return -EFAULT;
    return (a3 & MSG_TRUNC) ? (int32_t)full : r;
}

/* Socket options: buffer sizes, filters, membership: accepted. */
SYSCALL_DEF(packet_setsockopt)
{
    return 0;
}

SYSCALL_DEF(packet_getsockopt)
{
    struct psock *s = ps_of(a1, NULL);
    uint32_t val;
    if (a2 != SOL_SOCKET)
        return -ENOPROTOOPT;
    switch (a3) {
    case SO_TYPE:   val = (uint32_t)s->type; break;
    case SO_ERROR:  val = 0; break;
    case SO_RCVBUF: val = QUEUE_MAX; break;
    default:        return -ENOPROTOOPT;
    }
    uint32_t len;
    if (get_user32(&len, a5) < 0)
        return -EFAULT;
    if (len > 4)
        len = 4;
    if (copy_to_user((void *)a4, &val, len) < 0 || copy_to_user((void *)a5, &len, 4) < 0)
        return -EFAULT;
    return 0;
}

SYSCALL_DEF(packet_unsupported)       /* connect, listen, accept, getpeername, shutdown */
{
    return -EOPNOTSUPP;
}

/* --- as a file ------------------------------------------------------------------------ */

static int32_t p_read(struct file *f, void *ubuf, uint32_t n)
{
    struct psock *s = f->priv;
    struct pframe *b;
    int32_t r = take(s, f->flags & O_NONBLOCK, 0, &b);
    if (r < 0)
        return r;
    uint32_t c = b->len < n ? b->len : n;
    r = copy_to_user(ubuf, b->data, c) < 0 ? -EFAULT : (int32_t)c;
    kfree(b);
    return r;
}

static int32_t p_write(struct file *f, const void *ubuf, uint32_t n)
{
    struct user_iovec iov = { (user_addr_t)(uintptr_t)ubuf, n };
    return p_send(f->priv, f, &iov, 1, 0, 0);
}

static uint32_t p_poll(struct file *f)
{
    struct psock *s = f->priv;
    return (s->head ? POLLIN : 0) | POLLOUT;
}

static int32_t p_ioctl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    struct psock *s = f->priv;
    if ((cmd & 0xFF00) == 0x8900)
        return dev_ioctl(cmd, arg);     /* SIOCGIFINDEX, SIOCGIFHWADDR and friends */
    if (cmd == FIONREAD) {
        uint32_t n = s->head ? s->head->len : 0;
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

static const struct file_ops packet_ops = {
    .read = p_read, .write = p_write, .release = p_release,
    .ioctl = p_ioctl, .poll = p_poll,
};
