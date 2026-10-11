/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * dev_ioctl.c — the network interface ioctls (Linux: net/core/dev_ioctl.c)
 *
 * Before netlink, programs asked about network interfaces with ioctls on
 * any socket: "what is eth0's index? its flags? its MAC address?" Many
 * still do — wpa_supplicant brings a link up with SIOCSIFFLAGS, musl's
 * if_nametoindex() asks SIOCGIFINDEX on a UNIX socket, BusyBox `ip` and
 * `ifconfig` read the queue length and addresses this way. Linux answers
 * them for every kind of socket, so socket.c, unix.c and netlink.c all
 * send the 0x89xx range here.
 *
 * Each one carries a struct ifreq: the interface's name, then a union whose
 * meaning depends on the request (a sockaddr, flags, an int). Its size
 * follows the CPU (struct ifmap holds longs): 32 bytes on i386, 40 on x86_64.
 */
#include <errno.h>
#include <ipv4.h>
#include <netdevice.h>
#include <proc.h>
#include <skbuff.h>
#include <string.h>
#include <uaccess.h>
#include <user.h>

#define SIOCGIFNAME     0x8910
#define SIOCGIFCONF     0x8912
#define SIOCGIFFLAGS    0x8913
#define SIOCSIFFLAGS    0x8914
#define SIOCGIFADDR     0x8915
#define SIOCGIFBRDADDR  0x8919
#define SIOCGIFNETMASK  0x891b
#define SIOCGIFMTU      0x8921
#define SIOCGIFHWADDR   0x8927
#define SIOCGIFINDEX    0x8933
#define SIOCGIFTXQLEN   0x8942

#define AF_INET         2
#define ARPHRD_ETHER    1
#define ARPHRD_LOOPBACK 772

struct ifreq_k {
    char name[IFNAMSIZ];
    union {
        struct { uint16_t family, port; uint32_t addr; uint8_t zero[8]; } in;  /* sockaddr_in */
        struct { uint16_t family; uint8_t data[14]; } hw;                     /* sockaddr */
        int16_t flags;
        int32_t ivalue;                 /* ifindex, mtu, qlen */
        uint8_t pad[2 * sizeof(user_addr_t) + 8];   /* struct ifmap, the largest */
    };
};
_Static_assert(sizeof(struct ifreq_k) == 16 + 2 * sizeof(user_addr_t) + 8, "Linux's struct ifreq");

struct ifconf_k {                       /* struct ifconf */
    int32_t len;
    user_addr_t buf;                    /* (x86_64: after 4 bytes of padding) */
};

/* An interface's IPv4 address and netmask (host order): 0 if it has none. */
static int inet_of(const struct net_device *d, uint32_t *addr, uint32_t *mask)
{
    if (d->flags & IFF_LOOPBACK) {
        *addr = 0x7F000001u;
        *mask = 0xFF000000u;
        return 1;
    }
    if (d == ipv4_dev() && ipv4_addr) {
        *addr = ipv4_addr;
        *mask = ipv4_mask;
        return 1;
    }
    return 0;
}

static void set_in(struct ifreq_k *r, uint32_t addr)
{
    memset(&r->in, 0, sizeof r->in);
    r->in.family = AF_INET;
    r->in.addr = htonl(addr);
}

/* SIOCGIFCONF: one ifreq (name + IPv4 address) per interface that has an
   address; with no buffer, the room that would take. */
static int ifconf(uintptr_t arg)
{
    struct ifconf_k c;
    if (copy_from_user(&c, (const void *)arg, sizeof c) < 0)
        return -EFAULT;
    int32_t used = 0;
    struct net_device *d;
    for (int i = 1; i <= NETDEV_MAX; i++) {
        uint32_t addr, mask;
        if (!(d = dev_get_by_index(i)) || !inet_of(d, &addr, &mask))
            continue;
        if (c.buf) {
            if (used + (int32_t)sizeof(struct ifreq_k) > c.len)
                break;
            struct ifreq_k r;
            memset(&r, 0, sizeof r);
            memcpy(r.name, d->name, IFNAMSIZ);
            set_in(&r, addr);
            if (copy_to_user((void *)(uintptr_t)(c.buf + (user_addr_t)used), &r, sizeof r) < 0)
                return -EFAULT;
        }
        used += (int32_t)sizeof(struct ifreq_k);
    }
    c.len = used;
    return copy_to_user((void *)arg, &c.len, 4) < 0 ? -EFAULT : 0;
}

int dev_ioctl(uint32_t cmd, uintptr_t arg)
{
    if (cmd == SIOCGIFCONF)
        return ifconf(arg);
    if (cmd < SIOCGIFNAME || cmd > SIOCGIFTXQLEN)
        return -ENOTTY;
    struct ifreq_k r;
    if (copy_from_user(&r, (const void *)arg, sizeof r) < 0)
        return -EFAULT;
    struct net_device *d;
    if (cmd == SIOCGIFNAME) {
        if (!(d = dev_get_by_index(r.ivalue)))
            return -ENODEV;
        memcpy(r.name, d->name, IFNAMSIZ);
        return copy_to_user((void *)arg, &r, sizeof r) < 0 ? -EFAULT : 0;
    }
    r.name[IFNAMSIZ - 1] = '\0';
    if (!(d = dev_get_by_name(r.name)))
        return -ENODEV;
    uint32_t addr, mask;
    switch (cmd) {
    case SIOCGIFFLAGS:
        r.flags = (int16_t)dev_get_flags(d);
        break;
    case SIOCSIFFLAGS:
        if (proc_current()->cred.euid != 0)
            return -EPERM;              /* CAP_NET_ADMIN */
        return dev_change_flags(d, (uint16_t)r.flags);
    case SIOCGIFADDR:
    case SIOCGIFNETMASK:
    case SIOCGIFBRDADDR:
        if (!inet_of(d, &addr, &mask))
            return -EADDRNOTAVAIL;
        set_in(&r, cmd == SIOCGIFADDR ? addr : cmd == SIOCGIFNETMASK ? mask
                   : (d->flags & IFF_LOOPBACK) ? 0 : addr | ~mask);
        break;
    case SIOCGIFMTU:
        r.ivalue = (int32_t)d->mtu;
        break;
    case SIOCGIFHWADDR:
        memset(&r.hw, 0, sizeof r.hw);
        r.hw.family = (d->flags & IFF_LOOPBACK) ? ARPHRD_LOOPBACK : ARPHRD_ETHER;
        memcpy(r.hw.data, d->dev_addr, ETH_ALEN);
        break;
    case SIOCGIFINDEX:
        r.ivalue = d->ifindex;
        break;
    case SIOCGIFTXQLEN:
        r.ivalue = 1000;
        break;
    default:
        return -ENOTTY;
    }
    return copy_to_user((void *)arg, &r, sizeof r) < 0 ? -EFAULT : 0;
}
