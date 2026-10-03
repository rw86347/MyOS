#ifndef NETDEVICE_H
#define NETDEVICE_H

#include <skbuff.h>
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

#define NETDEV_TX_OK    0
#define NETDEV_TX_BUSY  1

/* Ethernet types, network byte order on the wire. htons() at the compare. */
#define ETH_P_IP        0x0800
#define ETH_P_ARP       0x0806
#define ETH_P_ALL       0x0003      /* every packet; not a real wire type */

struct net_device;

struct net_device_ops {
    int (*ndo_open)(struct net_device *dev);
    int (*ndo_stop)(struct net_device *dev);
    /* Send skb. The driver owns it afterwards and must free it. */
    int (*ndo_start_xmit)(struct sk_buff *skb, struct net_device *dev);
};

/* NAPI: the interrupt does almost nothing, then poll pulls a budget of
   packets. weight is that budget (Linux e1000 uses 64). */
struct napi_struct {
    struct net_device *dev;
    int (*poll)(struct napi_struct *napi, int budget);
    int weight;
    unsigned state;                 /* NAPI_STATE_SCHED when poll should run */
};

#define NAPI_STATE_SCHED 1

struct net_device {
    char name[IFNAMSIZ];
    unsigned char dev_addr[ETH_ALEN];
    unsigned int flags;
    unsigned int mtu;
    const struct net_device_ops *netdev_ops;
    struct napi_struct *napi;
};

/* A protocol that wants received packets of one Ethernet type. func owns
   the skb and must free it. type is host-order here (ETH_P_IP, or ETH_P_ALL). */
struct packet_type {
    uint16_t type;
    int (*func)(struct sk_buff *skb);
};

void net_init(void);                /* loopback, after the heap exists */

int register_netdev(struct net_device *dev);
struct net_device *dev_get_by_name(const char *name);

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

/* Read the Ethernet header: set skb->protocol (network order) and pull
   ETH_HLEN. Returns the protocol in host order. */
uint16_t eth_type_trans(struct sk_buff *skb, struct net_device *dev);

int dev_add_pack(struct packet_type *pt);

void netif_napi_add(struct net_device *dev, struct napi_struct *napi,
                    int (*poll)(struct napi_struct *, int), int weight);
void napi_enable(struct napi_struct *napi);
void napi_schedule(struct napi_struct *napi);
void napi_complete_done(struct napi_struct *napi, int work_done);

#endif
