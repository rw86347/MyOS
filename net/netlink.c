/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * netlink.c — AF_NETLINK: how programs talk to the kernel's network code
 *
 * Linux programs configure and watch the network through netlink, a socket
 * family whose other end is the kernel itself: `ip`, musl's getifaddrs(),
 * GLib's network monitor, and (for Wi-Fi) wpa_supplicant and iw. A request
 * is a message, sent with sendto/sendmsg:
 *
 *   struct nlmsghdr { u32 len; u16 type; u16 flags; u32 seq; u32 pid; }  + payload
 *
 * and the answers are messages too, read with recvmsg. Asking for one
 * object answers with that object; a dump (NLM_F_DUMP) answers with one
 * message per object, then NLMSG_DONE. An error — or an ack, when the
 * request asks for one (NLM_F_ACK) — is NLMSG_ERROR { int error; header }.
 * A payload is a fixed struct followed by "attributes": { u16 len; u16
 * type; value }, each padded to 4 bytes, so new fields never break old
 * readers.
 *
 * Two protocols so far:
 *   NETLINK_ROUTE ("rtnetlink"): the network links (RTM_GETLINK), their IPv4
 *     addresses (RTM_GETADDR) and the routes (RTM_GETROUTE). Read-only.
 *   NETLINK_GENERIC: a directory of named "families", found by asking its
 *     controller (family 0x10, "nlctrl"). Wi-Fi's nl80211 will be one
 *     (docs/WIFI-PLAN.md, W2).
 *
 * The kernel answers inside the sendmsg: a request is small and its answer
 * is built at once into the socket's queue, so a reader that doesn't wait
 * (musl reads with MSG_DONTWAIT) finds it there. The queue holds datagrams
 * of at most DGRAM_MAX bytes — a dump is packed into them as Linux packs
 * it — and QUEUE_MAX bytes in all; past that a request fails with ENOBUFS.
 *
 * Numbers and struct layouts are Linux's (uapi netlink.h, rtnetlink.h,
 * if_link.h, if_addr.h, genetlink.h), written out here.
 */
#include <errno.h>
#include <genetlink.h>
#include <ipv4.h>
#include <kheap.h>
#include <netdevice.h>
#include <proc.h>
#include <signal.h>
#include <socket.h>
#include <stddef.h>
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
#define MSG_CTRUNC      0x08
#define MSG_TRUNC       0x20
#define MSG_DONTWAIT    0x40

#define SOL_SOCKET      1
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_PASSCRED     16
#define SO_PROTOCOL     38
#define SO_DOMAIN       39
#define SCM_CREDENTIALS 2

#define FIONREAD        0x541B
#define FIONBIO         0x5421
#define POLLIN          0x001
#define POLLOUT         0x004

#define IOV_MAX_K       16
#define DGRAM_MAX       4096            /* one answer datagram (Linux: about a page) */
#define QUEUE_MAX       (64 * 1024)     /* waiting to be read, per socket */
#define REQUEST_MAX     (64 * 1024)     /* one sendmsg */
#define MSG_MAX         512             /* one answer message, built on the stack */

/* netlink.h */
#define NETLINK_ROUTE   0
#define NETLINK_GENERIC 16
#define SOL_NETLINK     270
#define NETLINK_ADD_MEMBERSHIP  1
#define NETLINK_DROP_MEMBERSHIP 2

#define NLMSG_ERROR     2
#define NLMSG_DONE      3
#define NLMSG_MIN_TYPE  0x10            /* below: control messages, not requests */
#define ALIGN4(n)       (((n) + 3u) & ~3u)

struct sockaddr_nl { uint16_t family, pad; uint32_t pid, groups; };
struct ucred { int32_t pid; uint32_t uid, gid; };

/* rtnetlink.h, if_link.h, if_addr.h */
#define RTM_NEWLINK     16
#define RTM_GETLINK     18
#define RTM_NEWADDR     20
#define RTM_GETADDR     22
#define RTM_NEWROUTE    24
#define RTM_GETROUTE    26

struct ifinfomsg { uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change; };
struct ifaddrmsg { uint8_t family, prefixlen, flags, scope; uint32_t index; };
struct rtmsg { uint8_t family, dst_len, src_len, tos, table, protocol, scope, type; uint32_t flags; };

#define IFLA_ADDRESS    1
#define IFLA_BROADCAST  2
#define IFLA_IFNAME     3
#define IFLA_MTU        4
#define IFLA_TXQLEN     13
#define IFLA_OPERSTATE  16
#define IFA_ADDRESS     1
#define IFA_LOCAL       2
#define IFA_LABEL       3
#define IFA_BROADCAST   4
#define RTA_DST         1
#define RTA_OIF         4
#define RTA_GATEWAY     5
#define RTA_PREFSRC     7
#define RTA_TABLE       15

#define AF_UNSPEC       0
#define AF_INET         2
#define ARPHRD_ETHER    1
#define ARPHRD_LOOPBACK 772
#define IF_OPER_DOWN    2
#define IF_OPER_UP      6
#define IFA_F_PERMANENT 0x80
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK   253
#define RT_SCOPE_HOST   254
#define RT_TABLE_MAIN   254
#define RTPROT_KERNEL   2
#define RTPROT_DHCP     16
#define RTN_UNICAST     1

/* genetlink.h */
#define GENL_ID_CTRL    0x10
#define CTRL_CMD_NEWFAMILY 1
#define CTRL_CMD_GETFAMILY 3
#define CTRL_ATTR_FAMILY_ID   1
#define CTRL_ATTR_FAMILY_NAME 2
#define CTRL_ATTR_VERSION     3
#define CTRL_ATTR_HDRSIZE     4
#define CTRL_ATTR_MAXATTR     5
#define CTRL_ATTR_MCAST_GROUPS 7
#define CTRL_ATTR_MCAST_GRP_NAME 1
#define CTRL_ATTR_MCAST_GRP_ID   2


/* The generic netlink families, found by name or id: the controller
   (here) and nl80211 (net/wireless/nl80211.c). */
static const char *const ctrl_groups[] = { "notify", NULL };
static const struct genl_family_k ctrl_family = {
    "nlctrl", GENL_ID_CTRL, 2, 0, 10, ctrl_groups, GENL_ID_CTRL, NULL,
};
extern const struct genl_family_k nl80211_family;
static const struct genl_family_k *const families[] = { &ctrl_family, &nl80211_family };
#define NFAMILIES (sizeof families / sizeof families[0])

/* --- sockets ------------------------------------------------------------------ */

struct nlbuf {                          /* one datagram waiting to be read */
    struct nlbuf *next;
    uint32_t len;
    uint8_t data[];
};

struct nlsock {
    int type, protocol;
    uint32_t portid;                    /* our address (nl_pid); 0 until bound */
    uint64_t groups;                    /* multicast groups joined, bit g-1 */
    int passcred;                       /* SO_PASSCRED: say the sender is the kernel */
    struct nlbuf *head, *tail;
    uint32_t queued;                    /* bytes in the queue */
    struct wait_queue wq;
    struct nlsock *next;                /* every netlink socket */
};

static struct nlsock *socks;
static const struct file_ops netlink_ops;
static struct inode nl_inode = {
    .ino = 0, .mode = S_IFSOCK | 0777, .nlink = 1, .name = "socket", .name_len = 6,
};

static struct nlsock *nl_of(uint32_t fd, struct file **fp)
{
    struct file *f = fd_get((int)fd);
    if (fp)
        *fp = f;
    return f && f->ops == &netlink_ops ? f->priv : NULL;
}

int netlink_fd(uint32_t fd)
{
    return nl_of(fd, NULL) != NULL;
}

static int portid_used(int protocol, uint32_t id)
{
    for (struct nlsock *s = socks; s; s = s->next)
        if (s->protocol == protocol && s->portid == id)
            return 1;
    return 0;
}

/* An address of our own, picked by the kernel as Linux does: the process
   id for its first socket, then numbers counting down from -4096. */
static void autobind(struct nlsock *s)
{
    static uint32_t next = 0xFFFFF000u;
    if (s->portid)
        return;
    uint32_t id = (uint32_t)proc_current()->pid;
    while (portid_used(s->protocol, id))
        id = next--;
    s->portid = id;
}

int32_t netlink_socket(uint32_t type, uint32_t protocol)
{
    uint32_t t = type & SOCK_TYPE_MASK;
    if (type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC) ||
        (t != SOCK_RAW && t != SOCK_DGRAM))
        return -ESOCKTNOSUPPORT;
    if (protocol != NETLINK_ROUTE && protocol != NETLINK_GENERIC)
        return -EPROTONOSUPPORT;
    struct nlsock *s = kmalloc(sizeof *s);
    if (!s)
        return -ENOMEM;
    memset(s, 0, sizeof *s);
    s->type = (int)t;
    s->protocol = (int)protocol;
    struct file *f = file_new(&netlink_ops, &nl_inode, O_RDWR | (type & SOCK_NONBLOCK));
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
        fd_set_cloexec(proc_current(), fd, 1);
    return fd;
}

static void nl_release(struct file *f)
{
    struct nlsock *s = f->priv;
    for (struct nlsock **pp = &socks; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    while (s->head) {
        struct nlbuf *b = s->head;
        s->head = b->next;
        kfree(b);
    }
    kfree(s);
}

/* --- building answers ---------------------------------------------------------- */

/* An answer message: a fixed struct, then the attributes, in a buffer the
   builder owns (include/genetlink.h). One that would not fit stops growing
   (`over`) and is not sent. */
#define MSG(name) uint8_t name##_buf[MSG_MAX]; NL_MSG(name, name##_buf)

void *nl_put(struct nl_msg *m, const void *p, uint32_t n)
{
    if (m->over || m->len + ALIGN4(n) > m->cap) {
        m->over = 1;
        return NULL;
    }
    void *at = m->b + m->len;
    if (n)
        memcpy(at, p, n);
    memset(m->b + m->len + n, 0, ALIGN4(n) - n);
    m->len += ALIGN4(n);
    return at;
}

void nla_put(struct nl_msg *m, uint16_t type, const void *p, uint32_t n)
{
    struct nlattr a = { (uint16_t)(sizeof a + n), type };
    if (m->over || n > 0xFFFF - sizeof a || m->len + sizeof a + ALIGN4(n) > m->cap) {
        m->over = 1;
        return;
    }
    nl_put(m, &a, sizeof a);
    nl_put(m, p, n);
}

static void attr_u32(struct nl_msg *m, uint16_t type, uint32_t v) { nla_put(m, type, &v, 4); }
static void attr_str(struct nl_msg *m, uint16_t type, const char *s) { nla_put_string(m, type, s); }

/* A nested attribute: its length is known only at the end. */
uint32_t nla_nest_start(struct nl_msg *m, uint16_t type)
{
    uint32_t at = m->len;
    struct nlattr a = { 0, (uint16_t)(type | NLA_F_NESTED) };
    nl_put(m, &a, sizeof a);
    return at;
}

void nla_nest_end(struct nl_msg *m, uint32_t at)
{
    if (!m->over)
        ((struct nlattr *)(m->b + at))->len = (uint16_t)(m->len - at);
}

int nla_parse(const struct nlattr **tb, int maxtype, const void *p, uint32_t len)
{
    memset(tb, 0, sizeof(*tb) * (size_t)(maxtype + 1));
    const uint8_t *b = p;
    for (uint32_t off = 0; off + sizeof(struct nlattr) <= len; ) {
        const struct nlattr *a = (const void *)(b + off);
        if (a->len < sizeof *a || a->len > len - off)
            return -EINVAL;
        uint16_t t = a->type & NLA_TYPE_MASK;
        if (t <= maxtype)
            tb[t] = a;
        off += ALIGN4(a->len);
    }
    return 0;
}

/* Where the answers to one request go: datagrams for the socket's queue. */
struct nl_reply {
    struct nlsock *s;
    const struct nlmsghdr *req;
    struct nlbuf *cur;                  /* the datagram being filled */
    int err;
};

static void reply_flush(struct nl_reply *r)
{
    struct nlbuf *b = r->cur;
    r->cur = NULL;
    if (!b)
        return;
    if (!b->len) {                      /* nothing in it (a dropped answer) */
        kfree(b);
        return;
    }
    struct nlsock *s = r->s;
    if (s->queued + b->len > QUEUE_MAX) {
        kfree(b);
        r->err = -ENOBUFS;
        return;
    }
    b->next = NULL;
    if (s->tail)
        s->tail->next = b;
    else
        s->head = b;
    s->tail = b;
    s->queued += b->len;
    wq_wake_all(&s->wq);
    wq_wake_all(&poll_wait);
}

/* Send message m (its payload) as `type`: a header in front, into the
   current datagram, or a new one when it would not fit. */
static void emit(struct nl_reply *r, uint16_t type, uint16_t flags, const struct nl_msg *m)
{
    uint32_t len = sizeof(struct nlmsghdr) + (m ? m->len : 0);
    if (m && m->over)
        r->err = -EMSGSIZE;             /* an answer outgrew MSG_MAX: a bug here */
    if (r->err)
        return;
    if (r->cur && r->cur->len + len > DGRAM_MAX)
        reply_flush(r);
    if (!r->cur) {
        r->cur = kmalloc(sizeof *r->cur + DGRAM_MAX);
        if (!r->cur) {
            r->err = -ENOBUFS;
            return;
        }
        r->cur->len = 0;
    }
    struct nlmsghdr h = { len, type, flags, r->req->seq, r->s->portid };
    memcpy(r->cur->data + r->cur->len, &h, sizeof h);
    if (m)
        memcpy(r->cur->data + r->cur->len + sizeof h, m->b, m->len);
    r->cur->len += len;
}

static void emit_done(struct nl_reply *r)
{
    MSG(m);
    int32_t zero = 0;
    nl_put(&m, &zero, 4);
    emit(r, NLMSG_DONE, NLM_F_MULTI, &m);
}

/* NLMSG_ERROR: the error (0 = an ack) and the request's header, without its
   payload (as Linux sends it with NETLINK_CAP_ACK). */
static void emit_error(struct nl_reply *r, int err)
{
    MSG(m);
    int32_t e = err;
    nl_put(&m, &e, 4);
    nl_put(&m, r->req, sizeof *r->req);
    emit(r, NLMSG_ERROR, 0, &m);
}

/* The attributes of a request, after its fixed header of `skip` bytes:
   the first of type `want`, or NULL. Its value length in *len. */
static const void *find_attr(const uint8_t *p, uint32_t plen, uint32_t skip, uint16_t want,
                             uint32_t *len)
{
    for (uint32_t off = ALIGN4(skip); off + sizeof(struct nlattr) <= plen; ) {
        const struct nlattr *a = (const void *)(p + off);
        if (a->len < sizeof *a || a->len > plen - off)
            break;
        if ((a->type & NLA_TYPE_MASK) == want) {
            *len = a->len - sizeof *a;
            return a + 1;
        }
        off += ALIGN4(a->len);
    }
    return NULL;
}

static int is_dump(const struct nlmsghdr *h)
{
    return (h->flags & NLM_F_DUMP) == NLM_F_DUMP;
}

/* --- rtnetlink: links, addresses, routes ----------------------------------------- */

static int prefix_len(uint32_t mask)
{
    int n = 0;
    while (mask & 0x80000000u) {
        n++;
        mask <<= 1;
    }
    return n;
}

static void put_link(struct nl_reply *r, const struct net_device *d, uint16_t flags)
{
    MSG(m);
    int lo = (d->flags & IFF_LOOPBACK) != 0;
    struct ifinfomsg ifi = {
        .family = AF_UNSPEC, .type = lo ? ARPHRD_LOOPBACK : ARPHRD_ETHER,
        .index = d->ifindex, .flags = dev_get_flags(d), .change = 0,
    };
    static const uint8_t bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t zero[ETH_ALEN];
    uint8_t oper = (d->flags & IFF_UP) ? IF_OPER_UP : IF_OPER_DOWN;
    nl_put(&m, &ifi, sizeof ifi);
    attr_str(&m, IFLA_IFNAME, d->name);
    attr_u32(&m, IFLA_MTU, d->mtu);
    attr_u32(&m, IFLA_TXQLEN, 1000);
    nla_put(&m, IFLA_OPERSTATE, &oper, 1);
    nla_put(&m, IFLA_ADDRESS, d->dev_addr, ETH_ALEN);
    nla_put(&m, IFLA_BROADCAST, lo ? zero : bcast, ETH_ALEN);
    emit(r, RTM_NEWLINK, flags, &m);
}

static int get_link(struct nl_reply *r, const uint8_t *p, uint32_t plen)
{
    if (is_dump(r->req)) {
        struct net_device *d;
        for (int i = 1; i <= NETDEV_MAX; i++)
            if ((d = dev_get_by_index(i)) != NULL)
                put_link(r, d, NLM_F_MULTI);
        emit_done(r);
        return 0;
    }
    /* One link, by index or by name (IFLA_IFNAME). */
    if (plen < sizeof(struct ifinfomsg))
        return -EINVAL;
    const struct ifinfomsg *ifi = (const void *)p;
    struct net_device *d = NULL;
    uint32_t n;
    const char *name;
    if (ifi->index > 0)
        d = dev_get_by_index(ifi->index);
    else if ((name = find_attr(p, plen, sizeof *ifi, IFLA_IFNAME, &n)) && n && n <= IFNAMSIZ &&
             !name[n - 1])
        d = dev_get_by_name(name);
    else
        return -EINVAL;
    if (!d)
        return -ENODEV;
    put_link(r, d, 0);
    return 0;
}

static void put_addr(struct nl_reply *r, const struct net_device *d, uint32_t addr, int prefix,
                     uint8_t scope)
{
    MSG(m);
    struct ifaddrmsg ifa = {
        .family = AF_INET, .prefixlen = (uint8_t)prefix, .flags = IFA_F_PERMANENT,
        .scope = scope, .index = (uint32_t)d->ifindex,
    };
    uint32_t a = htonl(addr);
    nl_put(&m, &ifa, sizeof ifa);
    nla_put(&m, IFA_ADDRESS, &a, 4);
    nla_put(&m, IFA_LOCAL, &a, 4);
    if (prefix < 31) {
        uint32_t bc = htonl(addr | (prefix ? ~0u >> prefix : ~0u));
        if (!(d->flags & IFF_LOOPBACK))
            nla_put(&m, IFA_BROADCAST, &bc, 4);
    }
    attr_str(&m, IFA_LABEL, d->name);
    emit(r, RTM_NEWADDR, NLM_F_MULTI, &m);
}

/* The first byte of a dump request is the address family it wants
   (struct rtgenmsg, or the start of ifaddrmsg/rtmsg). */
static int wants_inet(const uint8_t *p, uint32_t plen)
{
    return plen < 1 || p[0] == AF_UNSPEC || p[0] == AF_INET;
}

static int get_addr(struct nl_reply *r, const uint8_t *p, uint32_t plen)
{
    if (!is_dump(r->req))
        return -EOPNOTSUPP;             /* Linux, too: addresses are dumped */
    if (wants_inet(p, plen)) {
        struct net_device *lo = dev_get_by_name("lo");
        struct net_device *eth = ipv4_dev();
        if (lo)
            put_addr(r, lo, 0x7F000001u, 8, RT_SCOPE_HOST);
        if (eth && ipv4_addr)
            put_addr(r, eth, ipv4_addr, prefix_len(ipv4_mask), RT_SCOPE_UNIVERSE);
    }
    emit_done(r);
    return 0;
}

static void put_route(struct nl_reply *r, uint32_t dst, int dst_len, uint8_t proto, uint8_t scope,
                      uint32_t gw, const struct net_device *dev)
{
    MSG(m);
    struct rtmsg rt = {
        .family = AF_INET, .dst_len = (uint8_t)dst_len, .table = RT_TABLE_MAIN,
        .protocol = proto, .scope = scope, .type = RTN_UNICAST,
    };
    uint32_t v;
    nl_put(&m, &rt, sizeof rt);
    attr_u32(&m, RTA_TABLE, RT_TABLE_MAIN);
    if (dst_len) {
        v = htonl(dst);
        nla_put(&m, RTA_DST, &v, 4);
    }
    if (gw) {
        v = htonl(gw);
        nla_put(&m, RTA_GATEWAY, &v, 4);
    } else {
        v = htonl(ipv4_addr);
        nla_put(&m, RTA_PREFSRC, &v, 4);
    }
    attr_u32(&m, RTA_OIF, (uint32_t)dev->ifindex);
    emit(r, RTM_NEWROUTE, NLM_F_MULTI, &m);
}

static int get_route(struct nl_reply *r, const uint8_t *p, uint32_t plen)
{
    if (!is_dump(r->req))
        return -EOPNOTSUPP;             /* `ip route get` (a lookup): not yet */
    struct net_device *eth = ipv4_dev();
    if (wants_inet(p, plen) && eth && ipv4_addr) {
        int pl = prefix_len(ipv4_mask);
        if (ipv4_gw)
            put_route(r, 0, 0, RTPROT_DHCP, RT_SCOPE_UNIVERSE, ipv4_gw, eth);
        put_route(r, ipv4_addr & ipv4_mask, pl, RTPROT_KERNEL, RT_SCOPE_LINK, 0, eth);
    }
    emit_done(r);
    return 0;
}

static int rtnl_request(struct nl_reply *r, const uint8_t *p, uint32_t plen)
{
    switch (r->req->type) {
    case RTM_GETLINK:  return get_link(r, p, plen);
    case RTM_GETADDR:  return get_addr(r, p, plen);
    case RTM_GETROUTE: return get_route(r, p, plen);
    default:           return -EOPNOTSUPP;     /* changes: not yet */
    }
}

/* --- generic netlink: the controller ---------------------------------------------- */

static void put_family(struct nl_reply *r, const struct genl_family_k *f, uint16_t flags)
{
    MSG(m);
    struct genlmsghdr g = { CTRL_CMD_NEWFAMILY, 2, 0 };
    nl_put(&m, &g, sizeof g);
    attr_str(&m, CTRL_ATTR_FAMILY_NAME, f->name);
    uint16_t id = f->id;
    nla_put(&m, CTRL_ATTR_FAMILY_ID, &id, 2);
    attr_u32(&m, CTRL_ATTR_VERSION, f->version);
    attr_u32(&m, CTRL_ATTR_HDRSIZE, f->hdrsize);
    attr_u32(&m, CTRL_ATTR_MAXATTR, f->maxattr);
    if (f->groups && f->groups[0]) {
        uint32_t list = nla_nest_start(&m, CTRL_ATTR_MCAST_GROUPS);
        for (uint32_t k = 0; f->groups[k]; k++) {
            uint32_t one = nla_nest_start(&m, (uint16_t)(k + 1));
            attr_u32(&m, CTRL_ATTR_MCAST_GRP_ID, f->group_base + k);
            attr_str(&m, CTRL_ATTR_MCAST_GRP_NAME, f->groups[k]);
            nla_nest_end(&m, one);
        }
        nla_nest_end(&m, list);
    }
    emit(r, GENL_ID_CTRL, flags, &m);
}

static int ctrl_request(struct nl_reply *r, const uint8_t *p, uint32_t plen)
{
    if (((const struct genlmsghdr *)p)->cmd != CTRL_CMD_GETFAMILY)
        return -EOPNOTSUPP;
    if (is_dump(r->req)) {
        for (uint32_t k = 0; k < NFAMILIES; k++)
            put_family(r, families[k], NLM_F_MULTI);
        emit_done(r);
        return 0;
    }
    uint32_t n;
    const char *name = find_attr(p, plen, sizeof(struct genlmsghdr), CTRL_ATTR_FAMILY_NAME, &n);
    const uint16_t *id = find_attr(p, plen, sizeof(struct genlmsghdr), CTRL_ATTR_FAMILY_ID, &n);
    for (uint32_t k = 0; k < NFAMILIES; k++)
        if ((name && n && !name[n - 1] && strcmp(name, families[k]->name) == 0) ||
            (!name && id && n >= 2 && *id == families[k]->id)) {
            put_family(r, families[k], 0);
            return 0;
        }
    return name || id ? -ENOENT : -EINVAL;
}

/* A message for a generic netlink family: to its handler, by id. */
static int genl_request(struct nl_reply *r, const uint8_t *p, uint32_t plen)
{
    if (plen < sizeof(struct genlmsghdr))
        return -EINVAL;
    if (r->req->type == GENL_ID_CTRL)
        return ctrl_request(r, p, plen);
    for (uint32_t k = 0; k < NFAMILIES; k++)
        if (families[k]->id == r->req->type && families[k]->request)
            return families[k]->request(r, p, plen);
    return -ENOENT;                     /* no such family (Linux's answer) */
}

/* --- for the families outside this file (include/genetlink.h) -------------------- */

void nl_reply(struct nl_reply *r, uint16_t type, uint16_t flags, const struct nl_msg *m)
{
    emit(r, type, flags, m);
}

void nl_reply_done(struct nl_reply *r)
{
    emit_done(r);
}

int nl_reply_is_dump(const struct nl_reply *r)
{
    return is_dump(r->req);
}

uint32_t nl_reply_seq(const struct nl_reply *r)
{
    return r->req->seq;
}

uint32_t nl_reply_portid(const struct nl_reply *r)
{
    return r->s->portid;
}

/* An event: one datagram per socket that joined the group, as Linux sends
   it (sequence 0, from the kernel). A socket whose queue is full misses
   it. O(sockets). */
void genl_multicast(const struct genl_family_k *f, uint32_t group, const struct nl_msg *m)
{
    if (m->over || group < 1 || group > 64)
        return;
    uint32_t len = sizeof(struct nlmsghdr) + m->len;
    for (struct nlsock *s = socks; s; s = s->next) {
        if (s->protocol != NETLINK_GENERIC || !(s->groups & (1ull << (group - 1))))
            continue;
        if (s->queued + len > QUEUE_MAX)
            continue;
        struct nlbuf *b = kmalloc(sizeof *b + len);
        if (!b)
            continue;
        struct nlmsghdr h = { len, f->id, 0, 0, 0 };
        memcpy(b->data, &h, sizeof h);
        memcpy(b->data + sizeof h, m->b, m->len);
        b->len = len;
        b->next = NULL;
        if (s->tail)
            s->tail->next = b;
        else
            s->head = b;
        s->tail = b;
        s->queued += len;
        wq_wake_all(&s->wq);
    }
    wq_wake_all(&poll_wait);
}

/* --- requests ------------------------------------------------------------------- */

/* Every message in a sent buffer, answered in turn. An error is always
   answered, an ack when asked (a dump's answer is its own ack). */
static int deliver(struct nlsock *s, const uint8_t *buf, uint32_t len)
{
    for (uint32_t off = 0; off + sizeof(struct nlmsghdr) <= len; ) {
        const struct nlmsghdr *h = (const void *)(buf + off);
        if (h->len < sizeof *h || h->len > len - off)
            break;
        if ((h->flags & NLM_F_REQUEST) && h->type >= NLMSG_MIN_TYPE) {
            struct nl_reply r = { .s = s, .req = h };
            const uint8_t *p = buf + off + sizeof *h;
            uint32_t plen = h->len - sizeof *h;
            int err = s->protocol == NETLINK_ROUTE ? rtnl_request(&r, p, plen)
                                                   : genl_request(&r, p, plen);
            if (r.err && !err)
                err = r.err;
            if (err) {
                if (r.cur)
                    r.cur->len = 0;     /* drop a half-built answer */
                r.err = 0;
                emit_error(&r, err);
            } else if ((h->flags & NLM_F_ACK) && !is_dump(h)) {
                /* The ack in a datagram of its own, as Linux sends it:
                   libnl reads the answer, stops, drops the rest of that
                   datagram, then waits for the ack in the next one. */
                reply_flush(&r);
                emit_error(&r, 0);
            }
            reply_flush(&r);
            if (r.err)
                return r.err;
        }
        off += ALIGN4(h->len);
    }
    return 0;
}

/* A destination other than the kernel (nl_pid 0): we carry no messages
   between programs. */
static int check_dest(uintptr_t uaddr, uint32_t alen)
{
    if (!uaddr)
        return 0;
    struct sockaddr_nl a;
    if (alen < sizeof a)
        return -EINVAL;
    if (copy_from_user(&a, (const void *)uaddr, sizeof a) < 0)
        return -EFAULT;
    if (a.family != AF_NETLINK)
        return -EINVAL;
    return a.pid ? -ECONNREFUSED : 0;
}

static int32_t nl_send(struct nlsock *s, struct file *f, const struct user_iovec *iov,
                       uint32_t niov)
{
    uint32_t total = 0;
    for (uint32_t k = 0; k < niov; k++) {
        if (iov[k].len > REQUEST_MAX || total + iov[k].len > REQUEST_MAX)
            return -EMSGSIZE;
        total += (uint32_t)iov[k].len;
    }
    uint8_t *buf = kmalloc(total ? total : 1);
    if (!buf)
        return -ENOMEM;
    uint32_t at = 0;
    for (uint32_t k = 0; k < niov; k++) {
        if (file_copy_in(f, buf + at, (const void *)(uintptr_t)iov[k].base, (uint32_t)iov[k].len) < 0) {
            kfree(buf);
            return -EFAULT;
        }
        at += (uint32_t)iov[k].len;
    }
    autobind(s);
    int err = deliver(s, buf, total);
    kfree(buf);
    return err ? err : (int32_t)total;
}

/* --- reading -------------------------------------------------------------------- */

/* The next datagram: unlinked (or, peeking, a copy), so a copy_to_user that
   sleeps can't race another reader for it. The caller frees it. */
static int32_t take(struct nlsock *s, int nonblock, int peek, struct nlbuf **out)
{
    while (!s->head) {
        if (nonblock)
            return -EAGAIN;
        if (signal_pending())
            return -EINTR;
        wq_sleep(&s->wq);
    }
    struct nlbuf *b = s->head;
    if (peek) {
        struct nlbuf *c = kmalloc(sizeof *c + b->len);
        if (!c)
            return -ENOMEM;
        c->len = b->len;
        memcpy(c->data, b->data, b->len);
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

/* Copy datagram b into the iovecs: bytes copied, or -EFAULT. */
static int32_t copy_out(const struct nlbuf *b, const struct user_iovec *iov, uint32_t niov)
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

static int write_from_kernel(uintptr_t uaddr, uintptr_t ulenp)
{
    struct sockaddr_nl a = { AF_NETLINK, 0, 0, 0 };
    uint32_t have, n = sizeof a;
    if (!uaddr || !ulenp)
        return 0;
    if (get_user32(&have, ulenp) < 0)
        return -EFAULT;
    if (copy_to_user((void *)uaddr, &a, have < n ? have : n) < 0 ||
        copy_to_user((void *)ulenp, &n, 4) < 0)
        return -EFAULT;
    return 0;
}

static int32_t nl_read(struct file *f, void *ubuf, uint32_t n)
{
    struct user_iovec iov = { (user_addr_t)(uintptr_t)ubuf, n };
    struct nlbuf *b;
    int32_t r = take(f->priv, f->flags & O_NONBLOCK, 0, &b);
    if (r < 0)
        return r;
    r = copy_out(b, &iov, 1);
    kfree(b);
    return r;
}

static int32_t nl_write(struct file *f, const void *ubuf, uint32_t n)
{
    struct user_iovec iov = { (user_addr_t)(uintptr_t)ubuf, n };
    return nl_send(f->priv, f, &iov, 1);
}

static uint32_t nl_poll(struct file *f)
{
    struct nlsock *s = f->priv;
    return (s->head ? POLLIN : 0) | POLLOUT;
}

static int32_t nl_ioctl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    struct nlsock *s = f->priv;
    if ((cmd & 0xFF00) == 0x8900)
        return dev_ioctl(cmd, arg);     /* SIOCGIFINDEX and friends */
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

static const struct file_ops netlink_ops = {
    .read = nl_read, .write = nl_write, .release = nl_release,
    .ioctl = nl_ioctl, .poll = nl_poll,
};

/* --- the calls (socket.c sends them here: netlink_fd) --------------------------- */

SYSCALL_DEF(netlink_bind)
{
    struct nlsock *s = nl_of(a1, NULL);
    struct sockaddr_nl a;
    if (a3 < sizeof a)
        return -EINVAL;
    if (copy_from_user(&a, (const void *)a2, sizeof a) < 0)
        return -EFAULT;
    if (a.family != AF_NETLINK)
        return -EINVAL;
    if (a.pid && a.pid != s->portid) {
        if (s->portid)
            return -EINVAL;             /* bound already, to another address */
        if (portid_used(s->protocol, a.pid))
            return -EADDRINUSE;
        s->portid = a.pid;
    }
    autobind(s);
    s->groups |= a.groups;              /* groups 1-32, as a bit mask */
    return 0;
}

SYSCALL_DEF(netlink_connect)
{
    struct nlsock *s = nl_of(a1, NULL);
    int err = check_dest(a2, a3);
    if (err)
        return err;
    autobind(s);
    return 0;
}

SYSCALL_DEF(netlink_getsockname)
{
    struct nlsock *s = nl_of(a1, NULL);
    struct sockaddr_nl a = { AF_NETLINK, 0, s->portid, (uint32_t)s->groups };
    uint32_t have, n = sizeof a;
    if (get_user32(&have, a3) < 0)
        return -EFAULT;
    if (copy_to_user((void *)a2, &a, have < n ? have : n) < 0 ||
        copy_to_user((void *)a3, &n, 4) < 0)
        return -EFAULT;
    return 0;
}

SYSCALL_DEF(netlink_getpeername)
{
    return write_from_kernel(a2, a3);   /* the kernel, always */
}

/* sendto(fd, buf, len, flags, dest, destlen) */
SYSCALL_DEF(netlink_sendto)
{
    struct file *f;
    struct nlsock *s = nl_of(a1, &f);
    int err = check_dest(a5, a6);
    if (err)
        return err;
    struct user_iovec iov = { (user_addr_t)a2, (user_addr_t)a3 };
    return nl_send(s, f, &iov, 1);
}

/* recvfrom(fd, buf, len, flags, src, srclen) */
SYSCALL_DEF(netlink_recvfrom)
{
    struct file *f;
    struct nlsock *s = nl_of(a1, &f);
    struct nlbuf *b;
    int32_t r = take(s, (f->flags & O_NONBLOCK) || (a4 & MSG_DONTWAIT), a4 & MSG_PEEK, &b);
    if (r < 0)
        return r;
    struct user_iovec iov = { (user_addr_t)a2, (user_addr_t)a3 };
    r = copy_out(b, &iov, 1);
    uint32_t full = b->len;
    kfree(b);
    if (r < 0)
        return r;
    if (write_from_kernel(a5, a6) < 0)
        return -EFAULT;
    return (a4 & MSG_TRUNC) ? (int32_t)full : r;    /* MSG_TRUNC: the real size */
}

static int read_msghdr(uintptr_t umsg, struct user_msghdr *m, struct user_iovec *iov)
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

SYSCALL_DEF(netlink_sendmsg)
{
    struct file *f;
    struct nlsock *s = nl_of(a1, &f);
    struct user_msghdr m;
    struct user_iovec iov[IOV_MAX_K];
    int err = read_msghdr(a2, &m, iov);
    if (!err)
        err = check_dest(m.name, m.namelen);
    return err ? err : nl_send(s, f, iov, (uint32_t)m.iovlen);
}

SYSCALL_DEF(netlink_recvmsg)
{
    struct file *f;
    struct nlsock *s = nl_of(a1, &f);
    struct user_msghdr m;
    struct user_iovec iov[IOV_MAX_K];
    int err = read_msghdr(a2, &m, iov);
    if (err)
        return err;
    struct nlbuf *b;
    int32_t r = take(s, (f->flags & O_NONBLOCK) || (a3 & MSG_DONTWAIT), a3 & MSG_PEEK, &b);
    if (r < 0)
        return r;
    r = copy_out(b, iov, (uint32_t)m.iovlen);
    uint32_t full = b->len;
    kfree(b);
    if (r < 0)
        return r;

    int32_t flags = (uint32_t)r < full ? MSG_TRUNC : 0;
    uint32_t namelen = 0;
    if (m.name) {                       /* from the kernel: { AF_NETLINK, pid 0 } */
        struct sockaddr_nl a = { AF_NETLINK, 0, 0, 0 };
        namelen = sizeof a;
        if (m.namelen >= sizeof a && copy_to_user((void *)(uintptr_t)m.name, &a, sizeof a) < 0)
            return -EFAULT;
    }
    /* SO_PASSCRED: who sent it (the kernel: pid, uid and gid 0), as GLib
       checks before it believes a netlink message. */
    user_addr_t clen = 0;
    if (s->passcred) {
        struct { struct user_cmsghdr h; struct ucred c; } cm;
        uint32_t len = sizeof(struct user_cmsghdr) + sizeof(struct ucred);
        uint32_t space = (len + sizeof(user_addr_t) - 1) & ~(uint32_t)(sizeof(user_addr_t) - 1);
        memset(&cm, 0, sizeof cm);
        cm.h.len = len;
        cm.h.level = SOL_SOCKET;
        cm.h.type = SCM_CREDENTIALS;
        if (m.controllen >= space) {
            if (copy_to_user((void *)(uintptr_t)m.control, &cm, len) < 0)
                return -EFAULT;
            clen = space;
        } else {
            flags |= MSG_CTRUNC;
        }
    }
    if (copy_to_user((void *)(a2 + offsetof(struct user_msghdr, namelen)), &namelen, 4) < 0 ||
        copy_to_user((void *)(a2 + offsetof(struct user_msghdr, controllen)), &clen,
                     sizeof clen) < 0 ||
        copy_to_user((void *)(a2 + offsetof(struct user_msghdr, flags)), &flags, 4) < 0)
        return -EFAULT;
    return (a3 & MSG_TRUNC) ? (int32_t)full : r;
}

SYSCALL_DEF(netlink_setsockopt)
{
    struct nlsock *s = nl_of(a1, NULL);
    uint32_t v = 0;
    if (a5 >= 4 && get_user32(&v, a4) < 0)
        return -EFAULT;
    if (a2 == SOL_SOCKET) {
        if (a3 == SO_PASSCRED)
            s->passcred = v != 0;
        return 0;                       /* buffer sizes, timestamps: accepted, ignored */
    }
    if (a2 != SOL_NETLINK)
        return -ENOPROTOOPT;
    if (a3 == NETLINK_ADD_MEMBERSHIP || a3 == NETLINK_DROP_MEMBERSHIP) {
        if (a5 < 4)
            return -EINVAL;
        if (v < 1 || v > 64)
            return -EINVAL;
        if (a3 == NETLINK_ADD_MEMBERSHIP)
            s->groups |= 1ull << (v - 1);
        else
            s->groups &= ~(1ull << (v - 1));
        return 0;
    }
    return 0;                           /* ext ack, strict checks, ...: accepted */
}

SYSCALL_DEF(netlink_getsockopt)
{
    struct nlsock *s = nl_of(a1, NULL);
    uint32_t val;
    if (a2 != SOL_SOCKET)
        return -ENOPROTOOPT;
    switch (a3) {
    case SO_TYPE:     val = (uint32_t)s->type; break;
    case SO_ERROR:    val = 0; break;
    case SO_SNDBUF:
    case SO_RCVBUF:   val = QUEUE_MAX; break;
    case SO_PASSCRED: val = (uint32_t)s->passcred; break;
    case SO_PROTOCOL: val = (uint32_t)s->protocol; break;
    case SO_DOMAIN:   val = AF_NETLINK; break;
    default:          return -ENOPROTOOPT;
    }
    uint32_t len;
    if (get_user32(&len, a5) < 0)
        return -EFAULT;
    if ((int32_t)len < 0)
        return -EINVAL;
    if (len > 4)
        len = 4;
    if (copy_to_user((void *)a4, &val, len) < 0 || copy_to_user((void *)a5, &len, 4) < 0)
        return -EFAULT;
    return 0;
}

/* listen, accept, shutdown: not for netlink */
SYSCALL_DEF(netlink_unsupported)
{
    return -EOPNOTSUPP;
}
