/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * shim_net_test.c — Linux's packet and network-device API (W1c), checked
 * at boot in `make test` builds: sk_buffs (contiguous, frags, clones,
 * copies, queues), compound pages, a net_device registered and brought up,
 * stopped queues, and a NAPI poll. Everything must come back: no skb and
 * no page left over.
 */
#include <linux/netdevice.h>
#include <pmm.h>
#include <selftest.h>

uint32_t kheap_pages(void);         /* (kheap.h clashes with the shim's kfree) */

static int opened, sent, polled;

static int t_open(struct net_device *d) { (void)d; opened = 1; return 0; }
static int t_stop(struct net_device *d) { (void)d; opened = 0; return 0; }
static int t_xmit(struct sk_buff *skb, struct net_device *d) { (void)d; sent++; kfree_skb(skb); return NETDEV_TX_OK; }
static const struct net_device_ops t_ops = { .ndo_open = t_open, .ndo_stop = t_stop, .ndo_start_xmit = t_xmit };

static struct napi_struct t_napi;
static int t_poll(struct napi_struct *n, int budget)
{
    (void)budget;
    polled++;
    napi_complete_done(n, 0);
    return 0;
}

static void t_skbs(void)
{
    struct sk_buff *skb = alloc_skb(3000, GFP_KERNEL);
    check(skb && virt_to_phys(skb->data + 2999) - virt_to_phys(skb->data) == 2999,
          "shim net: a 3000-byte skb is physically contiguous");
    skb_reserve(skb, 64);
    u8 payload[100];
    for (int k = 0; k < 100; k++)
        payload[k] = (u8)k;
    skb_put_data(skb, payload, 100);
    skb_reset_network_header(skb);
    u8 *hdr = skb_push(skb, 14);
    skb_reset_mac_header(skb);
    check(hdr && skb->len == 114 && skb_network_offset(skb) == 14 && skb_mac_header(skb) == hdr &&
          skb_headroom(skb) == 50, "shim net: put, push, header offsets");

    /* A received page as frag 0: 500 bytes from offset 100. */
    struct page *pg = alloc_page(GFP_KERNEL);
    u8 *pa = page_address(pg);
    for (int k = 0; k < (int)PAGE_SIZE; k++)
        pa[k] = (u8)(k ^ 0x5a);
    skb_add_rx_frag(skb, 0, pg, 100, 500, PAGE_SIZE);
    u8 buf[40], *p = skb_header_pointer(skb, 100, 40, buf);   /* 14 linear + 26 from the page */
    int ok = skb->len == 614 && skb->data_len == 500 && skb_is_nonlinear(skb) && p == buf &&
             p[13] == (u8)99 && p[14] == (u8)(100 ^ 0x5a) && p[39] == (u8)(125 ^ 0x5a);
    check(ok, "shim net: a page frag, read across the boundary");

    struct sk_buff *cl = skb_clone(skb, GFP_ATOMIC);
    check(cl && cl->data == skb->data && skb_cloned(skb) && skb_shinfo(skb)->dataref == 2,
          "shim net: a clone shares the bytes");
    check(skb_linearize(cl) == 0 && !skb_is_nonlinear(cl) && cl->len == 614 && cl->data != skb->data &&
          cl->data[114 + 499] == (u8)(599 ^ 0x5a) && skb_is_nonlinear(skb),
          "shim net: linearizing a clone unshares it");
    struct sk_buff *cp = skb_copy(skb, GFP_ATOMIC);
    check(cp && !skb_is_nonlinear(cp) && cp->len == 614 && !memcmp(cp->data, cl->data, 614) &&
          skb_network_offset(cp) == 14, "shim net: skb_copy is a linear copy, headers kept");
    check(pskb_expand_head(cp, 128, 0, GFP_ATOMIC) == 0 && skb_headroom(cp) >= 128 + 50 &&
          !memcmp(cp->data, cl->data, 614) && skb_network_offset(cp) == 14,
          "shim net: pskb_expand_head moves the bytes and headers");
    skb_trim(cp, 20);
    check(cp->len == 20 && skb_tail_pointer(cp) == cp->data + 20, "shim net: trim");
    kfree_skb(cp);
    kfree_skb(cl);
    kfree_skb(skb);                     /* the page goes with it */
}

static void t_queues_pages(void)
{
    struct sk_buff_head q;
    skb_queue_head_init(&q);
    struct sk_buff *a = alloc_skb(16, GFP_KERNEL), *b = alloc_skb(16, GFP_KERNEL), *c = alloc_skb(16, GFP_KERNEL);
    skb_queue_tail(&q, b);
    skb_queue_tail(&q, c);
    skb_queue_head(&q, a);
    struct sk_buff_head q2;
    skb_queue_head_init(&q2);
    skb_queue_splice_tail_init(&q, &q2);
    struct sk_buff *x = skb_dequeue(&q2), *y = skb_dequeue(&q2);
    check(skb_queue_empty(&q) && x == a && y == b && skb_queue_len(&q2) == 1 && skb_peek(&q2) == c,
          "shim net: queues keep order, splice");
    kfree_skb(x);
    kfree_skb(y);
    skb_queue_purge(&q2);
    check(skb_queue_empty(&q2), "shim net: purge");

    uint32_t before = pmm_free_count();
    struct page *big = alloc_pages(GFP_KERNEL, 2);
    get_page(big);
    put_page(big);
    int held = pmm_free_count() == before - 4;
    put_page(big);                      /* the last owner: all four pages */
    check(big && held && pmm_free_count() == before, "shim net: a compound page goes with its last put");
}

static void t_netdev(void)
{
    struct net_device *d = alloc_netdev_mqs(64, "wtest%d", NET_NAME_ENUM, ether_setup, 4, 1);
    check(d && netdev_priv(d) && ((unsigned long)netdev_priv(d) & 31) == 0 && d->mtu == 1500 &&
          d->num_tx_queues == 4, "shim net: alloc_netdev, ether_setup, priv area");
    d->netdev_ops = &t_ops;
    static const u8 mac[6] = { 0x02, 0, 0, 0, 0, 0x42 };
    eth_hw_addr_set(d, mac);
    check(register_netdevice(d) == 0 && !strcmp(d->name, "wtest0") && dev_get_by_name("wtest0") == d &&
          !(d->flags & IFF_UP) && !opened, "shim net: registered, named wtest0, still down");
    check(dev_open(d, NULL) == 0 && opened && netif_running(d), "shim net: dev_open");

    struct sk_buff *skb = alloc_skb(64, GFP_KERNEL);
    skb_put_zero(skb, 60);
    skb->dev = d;
    netif_stop_queue(d);
    int busy = dev_queue_xmit(skb) == -EAGAIN && sent == 0;
    netif_wake_queue(d);
    check(busy && dev_queue_xmit(skb) == 0 && sent == 1, "shim net: a stopped queue keeps the packet");

    struct net_device *dummy = alloc_netdev_dummy(0);
    netif_napi_add(dummy, &t_napi, t_poll);
    napi_enable(&t_napi);
    napi_schedule(&t_napi);
    net_rx_action();
    check(polled == 1 && !(t_napi.state & NAPI_STATE_SCHED), "shim net: NAPI poll runs");
    napi_disable(&t_napi);
    netif_napi_del(&t_napi);
    free_netdev(dummy);

    unregister_netdevice(d);
    check(!dev_get_by_name("wtest0") && !opened, "shim net: unregister (closed first)");
    free_netdev(d);
}

void shim_net_selftest(void)
{
    uint32_t skbs = skb_live(), pages = pmm_free_count(), heap = kheap_pages();
    t_skbs();
    t_queues_pages();
    t_netdev();
    check(skb_live() == skbs, "shim net: every skb freed");
    /* (Pages the kernel heap took to grow stay with it, by design.) */
    check(pmm_free_count() + (kheap_pages() - heap) == pages, "shim net: every page given back");
}
