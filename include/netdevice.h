/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef NETDEVICE_H
#define NETDEVICE_H

#include <linux/device.h>
#include <skbuff.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * A network card, as a Linux driver sees it (struct net_device).
 *
 * The driver fills net_device_ops and calls register_netdev. To send, the
 * stack calls ndo_start_xmit and the driver owns the skb. To receive, the
 * driver builds an skb and calls netif_rx (or, from an interrupt, schedules
 * a NAPI poll and builds skbs there). This is the boundary a ported Linux
 * NIC driver calls. The driver source itself is not in the tree yet: those
 * files are GPLv2, and MyOS has no licence file.
 *
 * Flag values match Linux include/uapi/linux/if.h.
 */

#define IFNAMSIZ        16
#define ETH_ALEN        6
#define ETH_HLEN        14
#define ETH_ZLEN        60              /* shortest legal frame, without the CRC */
#define ETH_DATA_LEN    1500

#define IFF_UP          0x1
#define IFF_BROADCAST   0x2
#define IFF_LOOPBACK    0x8
#define IFF_RUNNING     0x40
#define IFF_NOARP       0x80
#define IFF_PROMISC     0x100
#define IFF_MULTICAST   0x1000
#define IFF_LOWER_UP    0x10000

#define NETDEV_TX_OK    0
#define NETDEV_TX_BUSY  1

/* Ethernet types, network byte order on the wire. htons() at the compare. */
#define ETH_P_IP        0x0800
#define ETH_P_ARP       0x0806
#define ETH_P_ALL       0x0003      /* every packet; not a real wire type */

struct net_device;

struct net_device_stats {
    unsigned long rx_packets, tx_packets, rx_bytes, tx_bytes;
    unsigned long rx_errors, tx_errors, rx_dropped, tx_dropped, multicast;
};
struct rtnl_link_stats64 {
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
    uint64_t rx_errors, tx_errors, rx_dropped, tx_dropped, multicast;
};

/* Linux's struct net_device_ops: what the stack calls on a device. The
   network stack uses the first three; Linux drivers and the 802.11 code
   fill in the others. */
struct net_device_ops {
    int (*ndo_open)(struct net_device *dev);
    int (*ndo_stop)(struct net_device *dev);
    /* Send skb. The driver owns it afterwards and must free it. */
    int (*ndo_start_xmit)(struct sk_buff *skb, struct net_device *dev);
    int (*ndo_init)(struct net_device *dev);
    void (*ndo_uninit)(struct net_device *dev);
    uint16_t (*ndo_select_queue)(struct net_device *dev, struct sk_buff *skb,
                                 struct net_device *sb_dev);
    void (*ndo_set_rx_mode)(struct net_device *dev);
    int (*ndo_set_mac_address)(struct net_device *dev, void *addr);
    int (*ndo_validate_addr)(struct net_device *dev);
    int (*ndo_change_mtu)(struct net_device *dev, int mtu);
    void (*ndo_tx_timeout)(struct net_device *dev, unsigned int txqueue);
    void (*ndo_get_stats64)(struct net_device *dev, struct rtnl_link_stats64 *s);
    uint64_t (*ndo_features_check)(struct sk_buff *skb, struct net_device *dev, uint64_t f);
};

/* NAPI: the interrupt does almost nothing, then poll pulls a budget of
   packets. weight is that budget (Linux e1000 uses 64). */
struct napi_struct {
    struct net_device *dev;
    int (*poll)(struct napi_struct *napi, int budget);
    int weight;
    unsigned state;                 /* NAPI_STATE_SCHED when poll should run */
};

#define NAPI_STATE_SCHED   1
#define NAPI_STATE_DISABLE 2

#define NETDEV_TXQ_MAX 8

struct wireless_dev;
struct net_device {
    char name[IFNAMSIZ];
    int ifindex;                    /* 1, 2, ... in order of registration (lo is 1) */
    unsigned char dev_addr[ETH_ALEN];
    unsigned int flags;
    unsigned int mtu;
    const struct net_device_ops *netdev_ops;
    struct napi_struct *napi;

    /* Linux's fields that drivers and the 802.11 code use (linux/netdevice.h). */
    unsigned char perm_addr[ETH_ALEN];
    unsigned char broadcast[ETH_ALEN];
    unsigned char addr_len;
    unsigned short type;            /* ARPHRD_ETHER, ... */
    unsigned short hard_header_len;
    unsigned short needed_headroom, needed_tailroom;
    unsigned int min_mtu, max_mtu;
    uint64_t features, hw_features;
    unsigned int priv_flags;
    unsigned int tx_queue_len;
    unsigned int num_tx_queues, real_num_tx_queues;
    unsigned long txq_stopped;      /* bit q: transmit queue q is stopped */
    int carrier;                    /* the link is up (netif_carrier_on) */
    unsigned char operstate;
    int watchdog_timeo;
    bool needs_free_netdev;
    void (*priv_destructor)(struct net_device *dev);
    struct wireless_dev *ieee80211_ptr;
    struct net_device_stats stats;
    const void *ethtool_ops;
    const void *header_ops;
    struct device dev;              /* .parent: SET_NETDEV_DEV */
    struct {                        /* its IPv4 lease (net/ipv4.c), 0: none */
        uint32_t addr, mask, gw, dns;
    } in4;
    void *priv;                     /* netdev_priv (alloc_netdev) */
    void *mem;                      /* the block it came in (alloc_netdev aligns) */
    int registered;
};

/* A protocol that wants received packets of one Ethernet type. func owns
   the skb and must free it. type is host-order here (ETH_P_IP, or ETH_P_ALL). */
struct packet_type {
    uint16_t type;
    int (*func)(struct sk_buff *skb);
};

void net_init(void);                /* loopback, after the heap exists */

#define NETDEV_MAX 8                /* devices at once; ifindex is 1..NETDEV_MAX */

int register_netdev(struct net_device *dev);   /* list it and bring it up */
int netdev_add(struct net_device *dev);        /* list it (ifindex), still down */
void netdev_remove(struct net_device *dev);
struct net_device *dev_get_by_name(const char *name);
/* NULL for a free slot: walk 1..NETDEV_MAX, don't stop at the first NULL. */
struct net_device *dev_get_by_index(int ifindex);

/* The flags as programs see them (IFF_RUNNING, IFF_LOWER_UP and so on
   added: no carrier sense yet). */
unsigned int dev_get_flags(const struct net_device *dev);

/* Bring a device up or down (IFF_UP): ndo_open / ndo_stop. 0 or -errno. */
int dev_change_flags(struct net_device *dev, unsigned int flags);

/* The interface ioctls (0x89xx: SIOCGIFINDEX, SIOCSIFFLAGS, ...) on any
   socket. net/dev_ioctl.c. */
int dev_ioctl(uint32_t cmd, uintptr_t arg);

/* Hand skb to dev->ndo_start_xmit. The device must be IFF_UP.
   NETDEV_TX_OK: the driver owns the skb. NETDEV_TX_BUSY: the caller still
   owns it (dev_queue_xmit returns -EAGAIN and does not free it). Any other
   failure frees it here. 0, or -errno. */
int dev_queue_xmit(struct sk_buff *skb);

/* Queue a received skb and take ownership. Drained by net_rx_action. */
int netif_rx(struct sk_buff *skb);

/* Deliver queued packets and run scheduled NAPI polls. Bounded. One caller
   at a time: a second caller (another thread) returns at once. */
void net_rx_action(void);

/* Wait for a packet, at most about `ms` milliseconds. Works with interrupts
   on (kernel threads, self-tests: halts until the next interrupt) and off
   (inside a syscall: sleeps on net_wait so other threads run). The caller
   loops and re-checks its condition. */
void net_sleep(uint32_t ms);

/* Woken by a NIC interrupt (drivers/e1000.c). */
struct wait_queue;
extern struct wait_queue net_wait;

/* Read the Ethernet header: set skb->protocol and pull ETH_HLEN. Returns
   the protocol in network order (Linux's __be16), as skb->protocol has it. */
uint16_t eth_type_trans(struct sk_buff *skb, struct net_device *dev);

int dev_add_pack(struct packet_type *pt);

/* Linux's two forms: the budget is 64 unless given. */
void netif_napi_add_weight(struct net_device *dev, struct napi_struct *napi,
                           int (*poll)(struct napi_struct *, int), int weight);
void netif_napi_add(struct net_device *dev, struct napi_struct *napi,
                    int (*poll)(struct napi_struct *, int));
void netif_napi_del(struct napi_struct *napi);

/* Hand a received packet to the protocols now (from a NAPI poll), rather
   than queueing it as netif_rx does. Takes the skb. */
int netif_receive_skb(struct sk_buff *skb);

/* Stop sending into the device (its ring is full), start again. */
void netif_stop_subqueue(struct net_device *dev, uint16_t q);
void netif_wake_subqueue(struct net_device *dev, uint16_t q);
void napi_enable(struct napi_struct *napi);
bool napi_schedule(struct napi_struct *napi);   /* false: it was already scheduled */
void napi_complete_done(struct napi_struct *napi, int work_done);

#endif
