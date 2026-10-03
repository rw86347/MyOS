/*
 * skbuff.c — packet buffers (step 10)
 *
 * One kmalloc for the descriptor, one for the bytes. A packet is small and
 * short-lived, so this stays off the page allocator. kfree_skb is safe from
 * an interrupt: the reference count drops with interrupts off, and the free
 * itself happens after.
 */
#include <interrupts.h>
#include <kheap.h>
#include <skbuff.h>
#include <string.h>

static uint32_t live;

struct sk_buff *alloc_skb(unsigned int size)
{
    struct sk_buff *skb = kmalloc(sizeof *skb);
    unsigned char *buf = kmalloc(size ? size : 1);
    if (!skb || !buf) {
        kfree(skb);
        kfree(buf);
        return NULL;
    }
    memset(skb, 0, sizeof *skb);
    skb->head = skb->data = skb->tail = buf;
    skb->end = buf + (size ? size : 1);
    skb->users = 1;
    live++;
    return skb;
}

void kfree_skb(struct sk_buff *skb)
{
    if (!skb)
        return;
    uint32_t flags = irq_save();
    if (!skb->users) {
        irq_restore(flags);
        return;
    }
    skb->users--;
    int last = skb->users == 0;
    irq_restore(flags);
    if (!last)
        return;
    kfree(skb->head);
    skb->head = NULL;
    kfree(skb);
    if (live)
        live--;
}

void consume_skb(struct sk_buff *skb)
{
    kfree_skb(skb);
}

uint32_t skb_live(void)
{
    return live;
}

unsigned char *skb_put(struct sk_buff *skb, unsigned int len)
{
    if (len > skb_tailroom(skb))
        return NULL;
    unsigned char *p = skb->tail;
    skb->tail += len;
    skb->len += len;
    return p;
}

unsigned char *skb_pull(struct sk_buff *skb, unsigned int len)
{
    if (len > skb->len)
        return NULL;
    skb->len -= len;
    skb->data += len;
    return skb->data;
}

unsigned char *skb_push(struct sk_buff *skb, unsigned int len)
{
    if (len > skb_headroom(skb))
        return NULL;
    skb->data -= len;
    skb->len += len;
    return skb->data;
}
