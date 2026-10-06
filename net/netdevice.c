/*
 * netdevice.c — the packet path a NIC driver talks to (step 10)
 *
 * Sending: dev_queue_xmit calls the driver's ndo_start_xmit. Receiving:
 * netif_rx drops the skb on a fixed queue (32 packets). net_rx_action,
 * called from the main loop and never from the interrupt itself, hands
 * each one to the protocol that asked for its Ethernet type and runs any
 * NAPI poll a driver scheduled. The queue is the bound: a flood past 32
 * packets is dropped, not queued without limit.
 */
#include <e1000.h>
#include <errno.h>
#include <interrupts.h>
#include <ipv4.h>
#include <kprintf.h>
#include <netdevice.h>
#include <socket.h>
#include <string.h>
#include <thread.h>
#include <tcp.h>
#include <timer.h>

#define NET_RX_MAX  32
#define NET_PACKS   8
#define NET_DEVS    NETDEV_MAX
#define NET_NAPIS   8

static struct sk_buff *rxq[NET_RX_MAX];
static unsigned rx_head, rx_tail, rx_count, rx_dropped;

static struct packet_type *packs[NET_PACKS];
static int npacks;

static struct net_device *devs[NET_DEVS];
static int ndevs;

static struct napi_struct *napis[NET_NAPIS];
static int nnapis;

uint16_t eth_type_trans(struct sk_buff *skb, struct net_device *dev)
{
    skb->dev = dev;
    if (skb->len < ETH_HLEN)
        return 0;
    memcpy(skb->eth_src, skb->data + 6, ETH_ALEN);
    skb->mac_header = (uint16_t)(skb->data - skb->head);
    uint16_t proto = (uint16_t)((skb->data[12] << 8) | skb->data[13]);
    skb->protocol = htons(proto);
    skb_pull(skb, ETH_HLEN);
    skb->network_header = (uint16_t)(skb->data - skb->head);
    return skb->protocol;
}

int dev_add_pack(struct packet_type *pt)
{
    if (!pt || npacks == NET_PACKS)
        return -ENOMEM;
    packs[npacks++] = pt;
    return 0;
}

/* Put a device in the list: the next free slot is its ifindex (slot + 1),
   never reused while it lives, as in Linux. Not brought up. */
int netdev_add(struct net_device *dev)
{
    if (!dev)
        return -EINVAL;
    for (int i = 0; i < NET_DEVS; i++)
        if (!devs[i]) {
            devs[i] = dev;
            dev->ifindex = i + 1;
            if (i + 1 > ndevs)
                ndevs = i + 1;
            return 0;
        }
    return -ENOMEM;
}

void netdev_remove(struct net_device *dev)
{
    if (dev && dev->ifindex >= 1 && dev->ifindex <= NET_DEVS && devs[dev->ifindex - 1] == dev)
        devs[dev->ifindex - 1] = NULL;
}

/* The stack's own devices (lo, eth0): listed and brought up at once. */
int register_netdev(struct net_device *dev)
{
    int err = netdev_add(dev);
    if (err)
        return err;
    if (dev->netdev_ops && dev->netdev_ops->ndo_open)
        dev->netdev_ops->ndo_open(dev);
    dev->flags |= IFF_UP;
    kprintf("net: %s up\n", dev->name);
    return 0;
}

unsigned int dev_get_flags(const struct net_device *dev)
{
    unsigned int f = dev->flags;
    if (f & IFF_UP)
        f |= IFF_RUNNING | IFF_LOWER_UP;
    if (!(f & IFF_LOOPBACK))
        f |= IFF_BROADCAST | IFF_MULTICAST;
    return f;
}

int dev_change_flags(struct net_device *dev, unsigned int flags)
{
    int up = (dev->flags & IFF_UP) != 0, want = (flags & IFF_UP) != 0;
    if (want && !up) {
        int err = dev->netdev_ops && dev->netdev_ops->ndo_open ? dev->netdev_ops->ndo_open(dev) : 0;
        if (err)
            return err;
        dev->flags |= IFF_UP;
    } else if (!want && up) {
        if (dev->netdev_ops && dev->netdev_ops->ndo_stop)
            dev->netdev_ops->ndo_stop(dev);
        dev->flags &= ~(IFF_UP | IFF_RUNNING);
    }
    return 0;
}

struct net_device *dev_get_by_index(int ifindex)
{
    return ifindex >= 1 && ifindex <= ndevs ? devs[ifindex - 1] : NULL;
}

struct net_device *dev_get_by_name(const char *name)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i] && strcmp(devs[i]->name, name) == 0)
            return devs[i];
    return NULL;
}

int dev_queue_xmit(struct sk_buff *skb)
{
    if (!skb)
        return -EINVAL;
    struct net_device *dev = skb->dev;
    if (!dev || !(dev->flags & IFF_UP) || !dev->netdev_ops ||
        !dev->netdev_ops->ndo_start_xmit) {
        kfree_skb(skb);
        return -ENETDOWN;
    }
    if (dev->txq_stopped & (1ul << (skb->queue_mapping % NETDEV_TXQ_MAX)))
        return -EAGAIN;                /* its queue is stopped: the caller keeps it */
    int rc = dev->netdev_ops->ndo_start_xmit(skb, dev);
    if (rc == NETDEV_TX_BUSY)
        return -EAGAIN;                /* driver did not take the skb */
    if (rc != NETDEV_TX_OK) {
        kfree_skb(skb);
        return -EIO;
    }
    return 0;
}

extern struct wait_queue net_wait;

int netif_rx(struct sk_buff *skb)
{
    if (!skb)
        return -EINVAL;
    uint32_t flags = irq_save();
    if (rx_count == NET_RX_MAX) {
        rx_dropped++;
        irq_restore(flags);
        kfree_skb(skb);
        return -ENOMEM;
    }
    rxq[rx_tail] = skb;
    rx_tail = (rx_tail + 1) % NET_RX_MAX;
    rx_count++;
    irq_restore(flags);
    wq_wake_all(&net_wait);         /* the network thread delivers it (Linux: NET_RX softirq) */
    return 0;
}

int netif_receive_skb(struct sk_buff *skb);

static void deliver(struct sk_buff *skb)
{
    netif_receive_skb(skb);
}

int netif_receive_skb(struct sk_buff *skb)
{
    packet_rcv(skb);                /* copies for AF_PACKET sockets (wpa_supplicant's EAPOL) */
    uint16_t proto = ntohs(skb->protocol);
    for (int i = 0; i < npacks; i++) {
        struct packet_type *pt = packs[i];
        if (pt->type != proto && pt->type != ETH_P_ALL)
            continue;
        pt->func(skb);              /* the handler owns it */
        return 0;
    }
    kfree_skb(skb);                 /* nobody wanted it */
    return 1;                       /* NET_RX_DROP */
}

void netif_napi_del(struct napi_struct *napi)
{
    for (int i = 0; i < nnapis; i++)
        if (napis[i] == napi) {
            napis[i] = napis[--nnapis];
            break;
        }
}

void netif_stop_subqueue(struct net_device *dev, uint16_t q)
{
    if (q < NETDEV_TXQ_MAX)
        __atomic_or_fetch(&dev->txq_stopped, 1ul << q, __ATOMIC_SEQ_CST);
}

void netif_wake_subqueue(struct net_device *dev, uint16_t q)
{
    if (q < NETDEV_TXQ_MAX)
        __atomic_and_fetch(&dev->txq_stopped, ~(1ul << q), __ATOMIC_SEQ_CST);
}

void netif_napi_add(struct net_device *dev, struct napi_struct *napi,
                    int (*poll)(struct napi_struct *, int))
{
    netif_napi_add_weight(dev, napi, poll, 64);
}

void netif_napi_add_weight(struct net_device *dev, struct napi_struct *napi,
                           int (*poll)(struct napi_struct *, int), int weight)
{
    if (!napi || nnapis == NET_NAPIS)
        return;
    napi->dev = dev;
    napi->poll = poll;
    napi->weight = weight ? weight : 64;
    napi->state = 0;
    if (dev)
        dev->napi = napi;
    napis[nnapis++] = napi;
}

void napi_enable(struct napi_struct *napi)
{
    if (napi)
        napi->state &= ~NAPI_STATE_SCHED;
}

bool napi_schedule(struct napi_struct *napi)
{
    if (!napi || (napi->state & (NAPI_STATE_DISABLE | NAPI_STATE_SCHED)))
        return false;
    napi->state |= NAPI_STATE_SCHED;
    wq_wake_all(&net_wait);         /* the network thread runs the poll */
    return true;
}

void napi_complete_done(struct napi_struct *napi, int work_done)
{
    (void)work_done;
    if (napi)
        napi->state &= ~NAPI_STATE_SCHED;
}

static void drain(void)
{
    while (rx_count) {
        uint32_t flags = irq_save();
        struct sk_buff *skb = rxq[rx_head];
        rx_head = (rx_head + 1) % NET_RX_MAX;
        rx_count--;
        irq_restore(flags);
        deliver(skb);
    }
}

struct wait_queue net_wait;
static int rx_busy;

void net_sleep(uint32_t ms)
{
    uint32_t flags = irq_save();
    if (flags & (1u << 9)) {
        irq_restore(flags);
        cpu_wait();
        return;
    }
    wq_sleep_until(&net_wait, timer_ms() + (ms ? ms : 1));
}

void net_rx_action(void)
{
    uint32_t flags = irq_save();
    if (rx_busy) {
        irq_restore(flags);
        return;
    }
    rx_busy = 1;
    irq_restore(flags);
    drain();
    for (int i = 0; i < nnapis; i++) {
        struct napi_struct *n = napis[i];
        if (!(n->state & NAPI_STATE_SCHED) || !n->poll)
            continue;
        /* The driver clears NAPI_STATE_SCHED (napi_complete_done) when the
           ring is caught up. If it leaves the bit set and stayed under the
           budget, a frame arrived during the poll: run it again now. The
           cap is so a driver that forgets to complete cannot spin here. */
        for (int spins = 0; spins < 8; spins++) {
            int work = n->poll(n, n->weight);
            if (!(n->state & NAPI_STATE_SCHED) || work >= n->weight)
                break;
        }
    }
    drain();                        /* packets the poll just queued */
    rx_busy = 0;
}

/* --- loopback ----------------------------------------------------------- */

static int loop_xmit(struct sk_buff *skb, struct net_device *dev)
{
    /* A frame we send to ourselves: same path as a card receiving it. */
    eth_type_trans(skb, dev);
    netif_rx(skb);
    return NETDEV_TX_OK;
}

static int loop_open(struct net_device *dev)
{
    dev->flags |= IFF_LOOPBACK | IFF_RUNNING | IFF_UP;
    return 0;
}

static const struct net_device_ops loop_ops = {
    .ndo_open = loop_open,
    .ndo_start_xmit = loop_xmit,
};

static struct net_device loopdev;

/* The network thread: receive and TCP timers when no program is in the
   kernel to do it (Linux: softirq / ksoftirqd). Woken by the card's
   interrupt through net_wait, or when the next TCP timer is due. It runs
   with interrupts off, like a syscall, so it never races one. */
static void net_thread(void *arg)
{
    (void)arg;
    irq_save();
    for (;;) {
        net_rx_action();
        wq_sleep_until(&net_wait, tcp_timers(timer_ms()));
    }
}

void net_init(void)
{
    memset(&loopdev, 0, sizeof loopdev);
    memcpy(loopdev.name, "lo", 3);
    loopdev.mtu = 65535;
    loopdev.netdev_ops = &loop_ops;
    register_netdev(&loopdev);
    e1000_init();                   /* eth0, if the 82540EM is on the bus */
    ipv4_init();
    thread_create("net", net_thread, NULL);
}
