/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * linux_netdev.c — Linux's net_device API: allocating a device with room
 * for its driver's private data, registering it (listed, still down —
 * bringing it up is a separate step, as in Linux), the dummy device that
 * a driver hangs its NAPI contexts on, the rtnl lock, Ethernet defaults.
 */
#include <linux/netdevice.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <random.h>

struct net init_net;
static DEFINE_MUTEX(rtnl);

void rtnl_lock(void) { mutex_lock(&rtnl); }
void rtnl_unlock(void) { mutex_unlock(&rtnl); }
int rtnl_is_locked(void) { return mutex_is_locked(&rtnl); }

void ether_setup(struct net_device *dev)
{
    dev->type = ARPHRD_ETHER;
    dev->hard_header_len = ETH_HLEN;
    dev->mtu = ETH_DATA_LEN;
    dev->min_mtu = 68;
    dev->max_mtu = ETH_DATA_LEN;
    dev->addr_len = ETH_ALEN;
    dev->tx_queue_len = 1000;
    dev->flags = IFF_BROADCAST | IFF_MULTICAST;
    eth_broadcast_addr(dev->broadcast);
}

/* The device and its private area in one block, the private part aligned
   to 32 bytes after the struct (where netdev_priv finds it). */
struct net_device *alloc_netdev_mqs(int sizeof_priv, const char *name, unsigned char name_assign,
                                    void (*setup)(struct net_device *), unsigned int txqs,
                                    unsigned int rxqs)
{
    (void)name_assign;
    (void)rxqs;
    size_t base = ALIGN(sizeof(struct net_device), 32);
    void *mem = kzalloc(base + (size_t)(sizeof_priv > 0 ? sizeof_priv : 0) + 31, GFP_KERNEL);
    if (!mem)
        return NULL;
    struct net_device *dev = PTR_ALIGN(mem, 32);     /* so the private part is aligned too */
    dev->mem = mem;
    dev->priv = (char *)dev + base;
    dev->num_tx_queues = dev->real_num_tx_queues = txqs ? txqs : 1;
    strscpy(dev->name, name ? name : "", sizeof dev->name);
    if (setup)
        setup(dev);
    return dev;
}

/* A device that is never registered: a place for NAPI contexts. */
void init_dummy_netdev(struct net_device *dev)
{
    memset(dev, 0, sizeof *dev);
    strscpy(dev->name, "dummy", sizeof dev->name);
}

struct net_device *alloc_netdev_dummy(int sizeof_priv)
{
    return alloc_netdev_mqs(sizeof_priv, "dummy", NET_NAME_ENUM, NULL, 1, 1);
}

void free_netdev(struct net_device *dev)
{
    if (!dev)
        return;
    if (dev->priv_destructor)
        dev->priv_destructor(dev);
    kfree(dev->mem);
}

/* "wlan%d": the first free number. */
int dev_alloc_name(struct net_device *dev, const char *pattern)
{
    const char *pct = strchr(pattern, '%');
    if (!pct) {
        strscpy(dev->name, pattern, sizeof dev->name);
        return 0;
    }
    for (int n = 0; n < 100; n++) {
        char name[IFNAMSIZ];
        snprintf(name, sizeof name, pattern, n);
        if (!dev_get_by_name(name)) {
            strscpy(dev->name, name, sizeof dev->name);
            return n;
        }
    }
    return -ENFILE;
}

int register_netdevice(struct net_device *dev)
{
    if (strchr(dev->name, '%') && dev_alloc_name(dev, dev->name) < 0)
        return -ENFILE;
    if (dev->netdev_ops && dev->netdev_ops->ndo_init) {
        int err = dev->netdev_ops->ndo_init(dev);
        if (err)
            return err;
    }
    if (is_zero_ether_addr(dev->perm_addr))
        ether_addr_copy(dev->perm_addr, dev->dev_addr);
    int err = netdev_add(dev);
    if (!err) {
        dev->registered = 1;
        dev->operstate = IF_OPER_DOWN;
        printk(KERN_INFO "net: %s registered\n", dev->name);
    }
    return err;
}

void unregister_netdevice(struct net_device *dev)
{
    if (!dev->registered)
        return;
    dev_close(dev);
    netdev_remove(dev);
    if (dev->netdev_ops && dev->netdev_ops->ndo_uninit)
        dev->netdev_ops->ndo_uninit(dev);
    dev->registered = 0;
    if (dev->needs_free_netdev)
        free_netdev(dev);
}

int dev_open(struct net_device *dev, void *extack)
{
    (void)extack;
    return dev_change_flags(dev, dev->flags | IFF_UP);
}

void dev_close(struct net_device *dev)
{
    dev_change_flags(dev, dev->flags & ~IFF_UP);
}

void napi_disable(struct napi_struct *n)
{
    n->state |= NAPI_STATE_DISABLE;
    n->state &= ~NAPI_STATE_SCHED;
}

int mac_pton(const char *s, u8 *mac)
{
    for (int i = 0; i < ETH_ALEN; i++) {
        int hi = hex_to_bin((unsigned char)s[3 * i]), lo = hex_to_bin((unsigned char)s[3 * i + 1]);
        if (hi < 0 || lo < 0 || (i < ETH_ALEN - 1 && s[3 * i + 2] != ':'))
            return 0;
        mac[i] = (u8)(hi << 4 | lo);
    }
    return 1;
}

void get_random_bytes(void *buf, size_t n)
{
    random_bytes(buf, (uint32_t)n);
}

/* One RSS key per boot, made on first use (Linux's netdev_rss_key). */
void netdev_rss_key_fill(void *buffer, size_t len)
{
    static u8 key[52];
    static bool made;
    if (!made) {
        get_random_bytes(key, sizeof key);
        made = true;
    }
    memcpy(buffer, key, len < sizeof key ? len : sizeof key);
}

u32 get_random_u32(void)
{
    return random_u32();
}
