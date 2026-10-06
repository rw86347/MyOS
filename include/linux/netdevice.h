#ifndef LINUX_NETDEVICE_H
#define LINUX_NETDEVICE_H

/*
 * Linux's net_device API on the kernel's own network devices
 * (include/netdevice.h has the struct, with Linux's fields; the network
 * stack and Linux drivers share it). kernel/linux_netdev.c has the rest.
 */
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/netdev_features.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <netdevice.h>

typedef int netdev_tx_t;
#define NET_RX_SUCCESS   0
#define NET_RX_DROP      1
#define IFF_LIVE_ADDR_CHANGE (1u << 15)
#define IFF_NO_QUEUE     (1u << 21)
#define NET_NAME_UNKNOWN 0
#define NET_NAME_ENUM    1
#define IF_OPER_DOWN     2
#define IF_OPER_DORMANT  5
#define IF_OPER_UP       6

struct net { int unused; };
extern struct net init_net;
static inline struct net *dev_net(const struct net_device *d) { (void)d; return &init_net; }
#define dev_net_set(d, n) do { (void)(d); (void)(n); } while (0)

static inline void *netdev_priv(const struct net_device *d) { return d->priv; }
struct net_device *alloc_netdev_mqs(int sizeof_priv, const char *name, unsigned char name_assign,
                                    void (*setup)(struct net_device *), unsigned int txqs,
                                    unsigned int rxqs);
#define alloc_netdev(sp, name, assign, setup) alloc_netdev_mqs(sp, name, assign, setup, 1, 1)
struct net_device *alloc_netdev_dummy(int sizeof_priv);
void init_dummy_netdev(struct net_device *dev);
void free_netdev(struct net_device *dev);
int  register_netdevice(struct net_device *dev);
int  register_netdev(struct net_device *dev);   /* (the stack's own) */
void unregister_netdevice(struct net_device *dev);
#define unregister_netdev(d)        unregister_netdevice(d)
#define SET_NETDEV_DEV(n, d)        ((n)->dev.parent = (d))
#define SET_NETDEV_DEVTYPE(n, t)    do { (void)(n); (void)(t); } while (0)
#define netdev_name(d)              ((d)->name)
int  dev_alloc_name(struct net_device *dev, const char *name);
void ether_setup(struct net_device *dev);
int  dev_open(struct net_device *dev, void *extack);
void dev_close(struct net_device *dev);
static inline void dev_hold(struct net_device *d) { (void)d; }
static inline void dev_put(struct net_device *d) { (void)d; }
#define netdev_hold(d, t, g)  dev_hold(d)
#define netdev_put(d, t)      dev_put(d)

/* rtnl: Linux's one lock around changes to network devices. */
void rtnl_lock(void);
void rtnl_unlock(void);
int  rtnl_is_locked(void);
#define ASSERT_RTNL()         do { } while (0)
#define lockdep_rtnl_is_held() 1

/* Queues and carrier */
static inline void netif_tx_start_all_queues(struct net_device *d) { d->txq_stopped = 0; }
static inline void netif_tx_stop_all_queues(struct net_device *d) { d->txq_stopped = ~0ul; }
static inline void netif_tx_wake_all_queues(struct net_device *d) { d->txq_stopped = 0; }
#define netif_start_queue(d)       netif_wake_subqueue(d, 0)
#define netif_stop_queue(d)        netif_stop_subqueue(d, 0)
#define netif_wake_queue(d)        netif_wake_subqueue(d, 0)
#define netif_queue_stopped(d)     ((d)->txq_stopped & 1ul)
#define __netif_subqueue_stopped(d, q) (((d)->txq_stopped >> (q)) & 1ul)
#define netif_tx_disable(d)        netif_tx_stop_all_queues(d)
#define netif_tx_lock_bh(d)        do { (void)(d); } while (0)
#define netif_tx_unlock_bh(d)      do { (void)(d); } while (0)
static inline int netif_set_real_num_tx_queues(struct net_device *d, unsigned int n)
{ d->real_num_tx_queues = n; return 0; }
static inline bool netif_running(const struct net_device *d) { return (d->flags & IFF_UP) != 0; }
static inline void netif_carrier_on(struct net_device *d) { d->carrier = 1; d->operstate = IF_OPER_UP; }
static inline void netif_carrier_off(struct net_device *d) { d->carrier = 0; d->operstate = IF_OPER_DORMANT; }
static inline bool netif_carrier_ok(const struct net_device *d) { return d->carrier != 0; }
static inline void netif_device_attach(struct net_device *d) { (void)d; }
static inline void netif_device_detach(struct net_device *d) { (void)d; }
static inline void netif_keep_dst(struct net_device *d) { (void)d; }
static inline bool netif_is_rxfh_configured(const struct net_device *d) { (void)d; return false; }
#define netdev_for_each_mc_addr(ha, dev) for (ha = NULL; ha; )
#define netdev_mc_count(dev)       0

/* A list of hardware addresses (the multicast groups to receive). */
struct netdev_hw_addr {
    struct list_head list;
    unsigned char addr[32];
    unsigned char type;
};
struct netdev_hw_addr_list {
    struct list_head list;
    int count;
};
#define netdev_hw_addr_list_count(l)            ((l)->count)
#define netdev_hw_addr_list_empty(l)            ((l)->count == 0)
#define netdev_hw_addr_list_for_each(ha, l)     list_for_each_entry(ha, &(l)->list, list)

/* A random key for spreading received flows over queues (RSS): one per
   boot, the same for every device, as in Linux. */
void netdev_rss_key_fill(void *buffer, size_t len);
static inline void dev_sw_netstats_rx_add(struct net_device *d, unsigned int len)
{ d->stats.rx_packets++; d->stats.rx_bytes += len; }
static inline void dev_sw_netstats_tx_add(struct net_device *d, unsigned int n, unsigned int len)
{ d->stats.tx_packets += n; d->stats.tx_bytes += len; }

/* NAPI */
static inline bool napi_schedule_prep(struct napi_struct *n)
{
    if (n->state & (NAPI_STATE_SCHED | NAPI_STATE_DISABLE))
        return false;
    n->state |= NAPI_STATE_SCHED;
    return true;
}
#define __napi_schedule(n)          do { (n)->state &= ~NAPI_STATE_SCHED; napi_schedule(n); } while (0)
#define __napi_schedule_irqoff(n)   __napi_schedule(n)
static inline bool napi_complete(struct napi_struct *n) { napi_complete_done(n, 0); return true; }
void napi_disable(struct napi_struct *n);
#define netif_napi_add_tx(dev, n, poll) netif_napi_add(dev, n, poll)
#define __netif_napi_del(n)         netif_napi_del(n)
#define napi_synchronize(n)         do { (void)(n); } while (0)
static inline int napi_gro_receive(struct napi_struct *n, struct sk_buff *skb)
{ (void)n; return netif_receive_skb(skb); }
#define napi_gro_flush(n, old)      do { (void)(n); } while (0)
#define netif_receive_skb_list(l)   do { struct sk_buff *_s, *_n; skb_list_walk_safe(l, _s, _n) netif_receive_skb(_s); } while (0)

#define netdev_err(d, fmt, ...)    printk(KERN_ERR "%s: " fmt, (d)->name, ##__VA_ARGS__)
#define netdev_warn(d, fmt, ...)   printk(KERN_WARNING "%s: " fmt, (d)->name, ##__VA_ARGS__)
#define netdev_info(d, fmt, ...)   printk(KERN_INFO "%s: " fmt, (d)->name, ##__VA_ARGS__)
#define netdev_dbg(d, fmt, ...)    do { if (0) printk(KERN_DEBUG fmt, ##__VA_ARGS__); } while (0)

#endif
