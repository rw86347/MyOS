#ifndef FUZZ_STUBS_H
#define FUZZ_STUBS_H

#include <skbuff.h>
#include <stdint.h>

/* ENETDOWN is not in include/errno.h; the value only has to be negative. */
#define ENETDOWN_FUZZ 100

/* One frame the stack handed to the "driver", Ethernet header included. */
struct fuzz_frame {
    unsigned len;
    uint8_t  b[2048];
};

/*
 * Frames the stack sent, oldest first, as a queue rather than a "last one"
 * slot: answering an ARP request flushes a frame that was waiting on it, so
 * one reply can produce more frames, and a target has to drain them all.
 */
#define FUZZ_SENT_MAX 16

int      fuzz_take(struct fuzz_frame *out);   /* 1 if a frame was dequeued */
void     fuzz_sent_reset(void);
unsigned fuzz_dropped(void);                  /* frames lost to a full queue */

struct net_device *fuzz_dev(void);

/* The fake clock. Advancing it is how retransmit and timeout paths are
   reached without a run taking a real second. */
void fuzz_clock_set(uint32_t t);
void fuzz_clock_add(uint32_t d);
void fuzz_cycles_reset(void);       /* and the fake cycle counter */

/* Called from net_sleep and net_rx_action: a target's chance to answer the
   packet the stack just sent. NULL to disable. */
void fuzz_set_inject(void (*fn)(void));

/* The handler ipv4_init registered for an Ethernet type (ETH_P_IP,
   ETH_P_ARP), or NULL. It takes ownership of the skb. */
int (*fuzz_handler(uint16_t type))(struct sk_buff *);

#endif
