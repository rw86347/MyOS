/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * stubs.c — the rest of the kernel, as far as the net sources can tell.
 *
 * The five net sources reference exactly 20 symbols they do not
 * define (i686-elf-nm -u says so). Everything below is one of those. The
 * real net sources are compiled unmodified: whatever the fuzzer finds is a
 * bug in the kernel, not in a rewritten copy of it.
 *
 * Two of these are more than stubs:
 *
 *   dev_queue_xmit  keeps the last frame the stack sent. The DNS and TCP
 *                   targets read it to learn the transaction id, the source
 *                   port and the sequence numbers we chose, which is what
 *                   lets a crafted reply get past the matching checks and
 *                   reach the parser underneath.
 *   net_sleep       is where the stack waits for a reply, so it is where a
 *                   target injects one. It also advances the fake clock,
 *                   which is what makes the retransmit and timeout paths
 *                   reachable without actually waiting.
 */

#include <kprintf.h>
#include <netdevice.h>
#include <skbuff.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stubs.h"

/* --- heap ------------------------------------------------------------------
   Straight to the host allocator on purpose: under ASan every kmalloc gets
   redzones and every kfree poisons, so a one-byte overrun in the stack is a
   hard failure here instead of silent corruption on real hardware. */

void *kmalloc(size_t size) { return malloc(size ? size : 1); }
void  kfree(void *ptr)     { free(ptr); }

/* --- clock ----------------------------------------------------------------
   A fake clock the fuzzer owns. Real time would make runs non-reproducible,
   and waiting for a 1-second ARP timeout would cap us at one run a second. */

static uint64_t fuzz_cycles;
static uint32_t now_ms = 1;

uint32_t timer_ms(void)             { return now_ms; }

/* A counting stand-in for the cycle counter: kernel/random.c is compiled into
   this build for real, so the tests drive the actual ChaCha20 generator, and
   it has to produce the same stream on every run for -replay to mean
   anything. Advancing by a prime keeps the low bits moving. */
uint64_t cycles_now(void)           { return (fuzz_cycles += 1000003); }
void     fuzz_cycles_reset(void)    { fuzz_cycles = 0; }
void     fuzz_clock_set(uint32_t t) { now_ms = t; }
void     fuzz_clock_add(uint32_t d) { now_ms += d; }

/* --- console --------------------------------------------------------------
   Silent unless FUZZ_VERBOSE is set: the stack prints on some error paths,
   and a million runs of that is a million lines of noise. */

static int verbose = -1;

static int want_output(void)
{
    if (verbose < 0)
        verbose = getenv("FUZZ_VERBOSE") != NULL;
    return verbose;
}

void kprintf(const char *fmt, ...)
{
    if (!want_output())
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void klog(const char *fmt, ...)
{
    if (!want_output())
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void console_write(const char *s, size_t n)
{
    if (want_output())
        fwrite(s, 1, n, stderr);
}

/* --- the network device ---------------------------------------------------- */

static int fake_xmit(struct sk_buff *skb, struct net_device *dev);

static const struct net_device_ops fake_ops = {
    .ndo_open       = NULL,
    .ndo_stop       = NULL,
    .ndo_start_xmit = fake_xmit,
};

static struct net_device fake_dev = {
    .name        = "eth0",
    .dev_addr    = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 },
    .flags       = IFF_UP | IFF_RUNNING | IFF_BROADCAST,
    .mtu         = ETH_DATA_LEN,
    .netdev_ops  = &fake_ops,
    .napi        = NULL,
};

struct net_device *dev_get_by_name(const char *name)
{
    return strcmp(name, "eth0") == 0 ? &fake_dev : NULL;
}

struct net_device *fuzz_dev(void) { return &fake_dev; }

/* A queue of what the stack sent, drained by the target. */
static struct fuzz_frame sent[FUZZ_SENT_MAX];
static unsigned sent_head, sent_tail, sent_dropped;

static int fake_xmit(struct sk_buff *skb, struct net_device *dev)
{
    (void)dev;
    if (sent_tail - sent_head < FUZZ_SENT_MAX) {
        struct fuzz_frame *f = &sent[sent_tail++ % FUZZ_SENT_MAX];
        f->len = skb->len < sizeof f->b ? skb->len : (unsigned)sizeof f->b;
        memcpy(f->b, skb->data, f->len);
    } else {
        sent_dropped++;
    }
    kfree_skb(skb);             /* the driver owns it, same as the real one */
    return NETDEV_TX_OK;
}

int fuzz_take(struct fuzz_frame *out)
{
    if (sent_head == sent_tail)
        return 0;
    *out = sent[sent_head++ % FUZZ_SENT_MAX];
    return 1;
}

void fuzz_sent_reset(void)
{
    sent_head = sent_tail = 0;
    sent_dropped = 0;
}

unsigned fuzz_dropped(void) { return sent_dropped; }

int dev_queue_xmit(struct sk_buff *skb)
{
    if (!(fake_dev.flags & IFF_UP)) {
        kfree_skb(skb);
        return -ENETDOWN_FUZZ;
    }
    return fake_xmit(skb, &fake_dev);
}

/* --- the protocol table ---------------------------------------------------
   ipv4_init() registers its ARP and IP handlers through this, and the
   targets fetch them back out. Going through the real registration means the
   fuzzer enters on_ip/on_arp by the same route a packet from the wire does. */

static struct packet_type *packs[8];
static unsigned packs_n;

int dev_add_pack(struct packet_type *pt)
{
    if (packs_n == sizeof packs / sizeof packs[0])
        return -1;
    packs[packs_n++] = pt;
    return 0;
}

int (*fuzz_handler(uint16_t type))(struct sk_buff *)
{
    for (unsigned i = 0; i < packs_n; i++)
        if (packs[i]->type == type)
            return packs[i]->func;
    return NULL;
}

/* --- waiting --------------------------------------------------------------
   The real net_sleep halts until a packet arrives; net_rx_action drains the
   receive queue. Here they are the hook a target uses to answer the stack.
   Without this, dns_resolve and tcp_connect just spin until they time out
   and the parsers underneath are never reached. */

static void (*inject)(void);

void fuzz_set_inject(void (*fn)(void)) { inject = fn; }

void net_sleep(uint32_t ms)
{
    now_ms += ms ? ms : 1;
    if (inject)
        inject();
}

void net_rx_action(void)
{
    if (inject)
        inject();
}

/* --- odds and ends -------------------------------------------------------- */

/* ping(8) reads ICMP through raw sockets. Reading every byte matters: if the
   caller passed a length past the end of the packet, ASan reports it here
   rather than nowhere. */
void raw_icmp_input(const uint8_t *ip, uint32_t len)
{
    volatile uint8_t sink = 0;
    for (uint32_t i = 0; i < len; i++)
        sink = (uint8_t)(sink + ip[i]);
    (void)sink;
}

/* No signals in a fuzz run; a pending one would just abort the operation. */
int signal_pending(void) { return 0; }
