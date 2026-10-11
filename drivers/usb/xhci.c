/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * xhci.c — USB host controllers: xHCI, in every PC since about 2012 (the
 * MSI's Intel 8086:43ed, QEMU's -device qemu-xhci). docs/USB-PLAN.md.
 *
 * USB in brief: the computer (the "host") does all the talking. A device
 * has numbered endpoints, one-way pipes; endpoint 0 is the control pipe
 * every device has. A new device answers at address 0: the host resets
 * its port, gives it an address, reads its descriptors (what it is, which
 * endpoints it has) and picks a configuration. Then a class driver takes
 * over (usb_storage.c for USB sticks).
 *
 * xHCI works much like NVMe (nvme.c): rings of 16-byte entries ("TRBs")
 * in RAM, shared with the controller. One command ring (enable a slot,
 * address a device, configure its endpoints), one transfer ring per
 * endpoint, and an event ring the controller writes completions to. A
 * doorbell register says "look at this ring". Every TRB has a cycle bit
 * that flips on each pass around its ring, so whose turn an entry is
 * shows in the entry itself. A ring's last TRB is a Link back to the
 * start.
 *
 * The controller keeps each device's state (its "device context": a slot
 * context, then one context per endpoint) in RAM we give it, and changes
 * it only itself: software asks with a command that points at an "input
 * context" saying what to change.
 *
 * Interrupts: MSI-X or MSI where the PCI code can give one, else the pin.
 * Waits also look at the event ring every POLL_MS, so a machine whose
 * interrupt never arrives is slow, not stuck (and says so once).
 *
 * Who runs what: the "usb" thread starts the controllers, finds and
 * addresses devices and probes class drivers; it alone uses a
 * controller's input context. Transfers come from any thread, one at a
 * time per endpoint (the class driver keeps that order).
 *
 * Not done (docs/USB-PLAN.md): hubs, unplugging (a gone device just stops
 * answering), interrupt and isochronous endpoints, streams.
 */
#include <interrupts.h>
#include <kheap.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pci.h>
#include <pmm.h>
#include <string.h>
#include <thread.h>
#include <timer.h>
#include <usb.h>

#define MAX_HC     4
#define MAX_SLOTS  32                   /* devices per controller */
#define MAX_PORTS  64
#define RING_TRBS  256                  /* a page; the last entry is the Link */
#define POLL_MS    20

/* Capability registers (the start of BAR 0) */
#define CAP_LENGTH     0x00             /* its length; HCIVERSION in the top half */
#define CAP_HCSPARAMS1 0x04             /* slots, interrupters, ports */
#define CAP_HCSPARAMS2 0x08             /* scratchpad pages */
#define CAP_HCCPARAMS1 0x10             /* context size, extended capabilities */
#define CAP_DBOFF      0x14
#define CAP_RTSOFF     0x18

/* Operational registers (after the capability registers) */
#define OP_USBCMD      0x00
#define OP_USBSTS      0x04
#define OP_PAGESIZE    0x08
#define OP_CRCR        0x18             /* the command ring */
#define OP_DCBAAP      0x30             /* the device context pointers */
#define OP_CONFIG      0x38
#define OP_PORTSC(p)   (0x400 + 0x10 * ((p) - 1))   /* ports count from 1 */

#define CMD_RUN        (1u << 0)
#define CMD_HCRST      (1u << 1)
#define CMD_INTE       (1u << 2)
#define STS_HCH        (1u << 0)        /* halted */
#define STS_HSE        (1u << 2)        /* host system error */
#define STS_EINT       (1u << 3)
#define STS_CNR        (1u << 11)       /* controller not ready */

/* Interrupter 0 (runtime registers + 0x20) */
#define IR_IMAN        0x20
#define IR_IMOD        0x24
#define IR_ERSTSZ      0x28
#define IR_ERSTBA      0x30
#define IR_ERDP        0x38
#define IMAN_IP        1u
#define IMAN_IE        2u
#define ERDP_EHB       8u

/* PORTSC */
#define PS_CCS         (1u << 0)        /* something is connected */
#define PS_PED         (1u << 1)        /* enabled (write 1: DISABLE it) */
#define PS_PR          (1u << 4)        /* reset */
#define PS_PLS(v)      (((v) >> 5) & 0xF)   /* link state */
#define PS_PP          (1u << 9)        /* power */
#define PS_SPEED(v)    (((v) >> 10) & 0xF)
#define PS_CSC         (1u << 17)       /* connect changed */
#define PS_WRC         (1u << 19)       /* warm reset done */
#define PS_PRC         (1u << 21)       /* reset done */
#define PS_CHANGES     (0x7Fu << 17)    /* every "changed" bit, write 1 to clear */
#define PS_WPR         (1u << 31)       /* warm reset (USB 3) */
/* The bits to write back so that nothing changes by accident: PED and the
   change bits clear when written as 1, so they are left out (Linux's
   xhci_port_state_to_neutral). */
#define PS_KEEP        ((1u << 0) | (1u << 3) | (0xFu << 5) | (1u << 9) | (0xFu << 10) | \
                        (3u << 14) | (7u << 25) | (1u << 30))
#define PLS_POLLING    7                /* USB 3: link training */
#define PLS_INACTIVE   6
#define PLS_COMPLIANCE 10

/* TRB types */
#define TRB_NORMAL       1
#define TRB_SETUP        2
#define TRB_DATA         3
#define TRB_STATUS       4
#define TRB_LINK         6
#define TRB_NOOP         8
#define TRB_ENABLE_SLOT  9
#define TRB_ADDRESS      11
#define TRB_CONFIG_EP    12
#define TRB_EVALUATE     13
#define TRB_RESET_EP     14
#define TRB_STOP_EP      15
#define TRB_SET_DEQ      16
#define TRB_EV_TRANSFER  32
#define TRB_EV_COMMAND   33
#define TRB_EV_PORT      34

#define T_CYCLE        (1u << 0)
#define T_TC           (1u << 1)        /* Link: toggle the cycle */
#define T_ISP          (1u << 2)        /* an event on a short packet */
#define T_CH           (1u << 4)        /* chain: the next TRB is the same transfer */
#define T_IOC          (1u << 5)        /* an event when done */
#define T_IDT          (1u << 6)        /* the data is in the TRB itself */
#define T_TYPE(t)      ((uint32_t)(t) << 10)
#define T_DIR_IN       (1u << 16)
#define T_EP(dci)      ((uint32_t)(dci) << 16)
#define T_SLOT(s)      ((uint32_t)(s) << 24)

/* Completion codes */
#define CC_SUCCESS     1
#define CC_SHORT       13
#define CC_STOPPED     26
#define CC_STOPPED_LEN 27
#define CC_STOPPED_SHORT 28

/* Endpoint context states */
#define EP_RUNNING     1
#define EP_HALTED      2

struct trb {
    uint64_t param;
    uint32_t status, control;
};

struct ring {
    struct trb *t;
    uint32_t phys;
    uint16_t enq;                       /* the next entry to fill */
    uint8_t cycle;                      /* the cycle bit that means "yours" now */
};

struct ep {
    struct ring ring;
    uint32_t wait_first;                /* the transfer usb_wait waits for: */
    uint32_t wait_phys;                 /* its first and last TRB (the last's event ends it) */
    uint16_t mps;                       /* max packet size */
    volatile uint8_t done;
    volatile uint8_t code;              /* the completion code it ended with */
    volatile uint32_t residue;          /* bytes not moved (a short packet) */
};

struct xhci;
struct usb_device {
    struct xhci *hc;
    uint8_t slot, port, speed;
    volatile uint8_t failed;            /* a transfer failed: its completion code */
    void *out;                          /* the device context (the controller's) */
    uint32_t out_phys;
    uint8_t *buf;                       /* a page for control transfers */
    uint32_t buf_phys;
    struct ep *ep[32];                  /* by device context index; 1 is endpoint 0 */
    struct usb_device_descriptor desc;
    char name[48];
};

struct xhci {
    struct pci_dev *pci;
    volatile uint8_t *cap, *op, *rt, *db;
    uint16_t version;
    uint8_t csz;                        /* context size: 32 or 64 bytes */
    uint8_t ports, slots;
    uint8_t usb3[MAX_PORTS + 1];        /* port p is a USB 3 port */
    uint64_t *dcbaa;                    /* device context pointers, by slot */
    struct ring cmd;
    struct trb *ev;                     /* the event ring: one segment */
    uint32_t ev_phys;
    uint16_t ev_deq;
    uint8_t ev_cycle;
    uint8_t *in;                        /* the input context (the usb thread's) */
    uint32_t in_phys;
    volatile uint8_t cmd_done, cmd_code, cmd_slot;
    uint8_t cmd_busy, warned;
    struct wait_queue cmd_turn;
    volatile uint8_t port_change;
    struct usb_device *dev[MAX_SLOTS + 1];
    struct usb_device *port_dev[MAX_PORTS + 1];
    uint8_t port_tried[MAX_PORTS + 1];
};

static struct xhci hcs[MAX_HC];
static struct pci_dev *found[MAX_HC];
static int nfound, nhc;
static struct wait_queue events_wq;     /* anyone waiting for a completion */
static struct wait_queue port_wq;       /* the usb thread, between port changes */
static struct wait_queue boot_wq;       /* usb_wait_boot */
static volatile uint8_t boot_done;

#define barrier() __asm__ volatile ("" ::: "memory")

static uint32_t rd(volatile uint8_t *base, uint32_t r) { return *(volatile uint32_t *)(base + r); }
static void wr(volatile uint8_t *base, uint32_t r, uint32_t v) { *(volatile uint32_t *)(base + r) = v; }
/* 64-bit registers: the low half first (xHCI 5.1). */
static void wr64(volatile uint8_t *base, uint32_t r, uint64_t v)
{
    wr(base, r, (uint32_t)v);
    wr(base, r + 4, (uint32_t)(v >> 32));
}

void usb_sleep(uint32_t ms)
{
    struct wait_queue q = { 0 };
    uint32_t flags = irq_save();
    wq_sleep_until(&q, timer_ms() + ms);
    irq_restore(flags);
}

static int dci_of(uint8_t ep)
{
    return (ep & 0xF) ? (ep & 0xF) * 2 + ((ep & USB_DIR_IN) ? 1 : 0) : 1;
}

/* Context i of a device or input context. */
static uint32_t *ctx(struct xhci *hc, void *base, int i)
{
    return (uint32_t *)((uint8_t *)base + i * hc->csz);
}

static void *page(uint32_t *phys)
{
    *phys = pmm_alloc();
    if (!*phys)
        return NULL;
    memset(P2V(*phys), 0, PAGE_SIZE);
    return P2V(*phys);
}

/* ---- rings ---- */

static int ring_init(struct ring *r)
{
    if (!(r->t = page(&r->phys)))
        return -1;
    struct trb *link = &r->t[RING_TRBS - 1];
    link->param = r->phys;
    link->control = T_TYPE(TRB_LINK) | T_TC;
    r->enq = 0;
    r->cycle = 1;
    return 0;
}

/* Fill the next entry and hand it to the controller (the cycle bit goes
   last); step over the Link at the end. The entry's physical address. */
static uint32_t ring_put(struct ring *r, uint64_t param, uint32_t status, uint32_t control)
{
    struct trb *t = &r->t[r->enq];
    uint32_t phys = r->phys + r->enq * sizeof *t;
    t->param = param;
    t->status = status;
    barrier();
    t->control = control | r->cycle;
    if (++r->enq == RING_TRBS - 1) {
        struct trb *link = &r->t[RING_TRBS - 1];
        link->control = (link->control & ~T_CYCLE) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return phys;
}

/* Make room for a transfer of n TRBs before the Link, so that no
   transfer is split by it (a split one needs the Link chained, and
   packet-sized pieces on both sides: xHCI 4.11.7.1). No-ops fill the rest. */
static void ring_room(struct ring *r, int n)
{
    if (r->enq + n > RING_TRBS - 1)
        while (r->enq)
            ring_put(r, 0, 0, T_TYPE(TRB_NOOP));
}

static void doorbell(struct xhci *hc, int slot, int target)
{
    barrier();
    ((volatile uint32_t *)hc->db)[slot] = (uint32_t)target;
}

/* ---- events (the interrupt, or a waiter polling; interrupts off) ---- */

static void transfer_event(struct xhci *hc, struct trb *e)
{
    uint8_t slot = e->control >> 24, dci = (e->control >> 16) & 0x1F;
    uint8_t code = e->status >> 24;
    struct usb_device *d = slot <= MAX_SLOTS ? hc->dev[slot] : NULL;
    struct ep *ep = d ? d->ep[dci] : NULL;
    if (!ep)
        return;
    if (code == CC_SHORT)
        ep->residue = e->status & 0xFFFFFF;
    if (code == CC_SUCCESS || code == CC_SHORT) {
        /* A short packet ends a bulk transfer early, at whichever of its
           TRBs it happened (some controllers then also report the last
           TRB: that event finds the next transfer waited for, and must not
           end it, hence the range). A control transfer goes on to its
           status stage, whose event comes next. */
        uint32_t at = (uint32_t)e->param;
        if (at == ep->wait_phys ||
            (code == CC_SHORT && dci != 1 && at >= ep->wait_first && at <= ep->wait_phys)) {
            ep->code = code;
            ep->done = 1;
        }
    } else if (code != CC_STOPPED && code != CC_STOPPED_LEN && code != CC_STOPPED_SHORT) {
        ep->code = code;                /* a stall, a transaction error...: */
        d->failed = code;               /* the endpoint has stopped */
    }
}

static void events(struct xhci *hc)
{
    int any = 0;
    for (;;) {
        struct trb *e = &hc->ev[hc->ev_deq];
        uint32_t ctl = e->control;
        if ((ctl & T_CYCLE) != hc->ev_cycle)
            break;                      /* not written yet */
        barrier();
        switch ((ctl >> 10) & 0x3F) {
        case TRB_EV_COMMAND:
            hc->cmd_code = e->status >> 24;
            hc->cmd_slot = ctl >> 24;
            hc->cmd_done = 1;
            break;
        case TRB_EV_TRANSFER:
            transfer_event(hc, e);
            break;
        case TRB_EV_PORT:
            hc->port_change = 1;
            if (boot_done)
                wq_wake_all(&port_wq);
            break;
        }
        if (++hc->ev_deq == RING_TRBS) {
            hc->ev_deq = 0;
            hc->ev_cycle ^= 1;
        }
        any = 1;
    }
    if (any) {
        wr64(hc->rt, IR_ERDP, (hc->ev_phys + hc->ev_deq * sizeof(struct trb)) | ERDP_EHB);
        wq_wake_all(&events_wq);
    }
}

static void xhci_irq(struct regs *r)
{
    (void)r;
    for (int i = 0; i < nhc; i++) {
        struct xhci *hc = &hcs[i];
        uint32_t sts = rd(hc->op, OP_USBSTS);
        if (!(sts & (STS_EINT | STS_HSE)))
            continue;                   /* not this one (the pin may be shared) */
        wr(hc->op, OP_USBSTS, sts & (STS_EINT | STS_HSE));     /* write 1 to clear */
        wr(hc->rt, IR_IMAN, IMAN_IE | IMAN_IP);
        if ((sts & STS_HSE) && !hc->warned) {
            kprintf("xhci: host system error (bad DMA address?)\n");
            hc->warned = 1;
        }
        events(hc);
    }
}

/* Sleep until *flag is set or ms pass (interrupts off). Looks at the
   event ring itself every POLL_MS in case the interrupt is not coming. */
static int wait_flag(struct xhci *hc, volatile uint8_t *flag, volatile uint8_t *fail, uint32_t ms)
{
    uint32_t end = timer_ms() + ms;
    while (!*flag && !(fail && *fail)) {
        uint32_t now = timer_ms(), next = now + POLL_MS;
        if ((int32_t)(now - end) >= 0)
            return -1;
        if ((int32_t)(next - end) > 0)
            next = end;
        if (wq_sleep_until(&events_wq, next)) {
            events(hc);
            if ((*flag || (fail && *fail)) && !hc->warned) {
                kprintf("xhci: its interrupt does not arrive: polling every %d ms\n", POLL_MS);
                hc->warned = 1;
            }
        }
    }
    return *flag ? 0 : -1;
}

/* One command; its completion code (1: success), or 0 if it timed out. */
static int command(struct xhci *hc, uint64_t param, uint32_t control, uint8_t *slot)
{
    uint32_t flags = irq_save();
    while (hc->cmd_busy)
        wq_sleep(&hc->cmd_turn);
    hc->cmd_busy = 1;
    hc->cmd_done = 0;
    ring_put(&hc->cmd, param, 0, control);
    doorbell(hc, 0, 0);
    int code = wait_flag(hc, &hc->cmd_done, NULL, 5000) == 0 ? hc->cmd_code : 0;
    if (slot)
        *slot = hc->cmd_slot;
    hc->cmd_busy = 0;
    wq_wake_all(&hc->cmd_turn);
    irq_restore(flags);
    if (code != CC_SUCCESS)
        kprintf("xhci: command %lu %s (code %d)\n", (unsigned long)((control >> 10) & 0x3F),
                code ? "failed" : "timed out", code);
    return code;
}

/* ---- transfers ---- */

/* 0, the failed transfer's completion code, or -1 (timed out). */
static int wait_ep(struct usb_device *d, struct ep *ep, uint32_t ms)
{
    if (wait_flag(d->hc, &ep->done, &d->failed, ms) < 0) {
        if (d->failed)
            return d->failed;
        kprintf("usb %d-%d: no answer in %lu ms\n", (int)(d->hc - hcs) + 1, d->port,
                (unsigned long)ms);
        return -1;
    }
    return 0;
}

int usb_queue(struct usb_device *d, uint8_t epa, const struct bio_vec *v, int nv, int wait)
{
    struct ep *ep = d->ep[dci_of(epa)];
    if (!ep)
        return -1;
    /* A TRB may not cross a 64 KiB boundary: count the pieces. */
    int n = 0;
    uint32_t total = 0;
    for (int k = 0; k < nv; k++) {
        n += (int)(((v[k].phys + v[k].len - 1) >> 16) - (v[k].phys >> 16)) + 1;
        total += v[k].len;
    }
    if (n > RING_TRBS / 2)
        return -1;
    struct ring *r = &ep->ring;
    ring_room(r, n);
    uint32_t first = r->phys + r->enq * sizeof(struct trb);
    uint32_t packets = (total + ep->mps - 1) / ep->mps, moved = 0, last = 0;
    for (int k = 0; k < nv; k++) {
        for (uint32_t pa = v[k].phys, end = pa + v[k].len; pa < end; ) {
            uint32_t len = ((pa | 0xFFFF) + 1) - pa;
            if (len > end - pa)
                len = end - pa;
            moved += len;
            int more = moved < total;
            /* "TD size": packets still to come after this TRB, at most 31
               (xHCI 1.0; 0.96 counted KiB). */
            uint32_t left = d->hc->version >= 0x100 ? packets - moved / ep->mps
                                                    : (total - moved) >> 10;
            if (!more || left > 31)
                left = more ? 31 : 0;
            last = ring_put(r, pa, len | (left << 17), T_TYPE(TRB_NORMAL) |
                            (wait ? T_ISP : 0) | (more ? T_CH : wait ? T_IOC : 0));
            pa += len;
        }
    }
    if (wait) {
        ep->wait_first = first;
        ep->wait_phys = last;
        ep->done = 0;
        ep->residue = 0;
    }
    return 0;
}

void usb_start(struct usb_device *d, uint8_t epa)
{
    doorbell(d->hc, d->slot, dci_of(epa));
}

int usb_wait(struct usb_device *d, uint8_t epa, uint32_t ms)
{
    struct ep *ep = d->ep[dci_of(epa)];
    uint32_t flags = irq_save();
    int err = ep ? wait_ep(d, ep, ms) : -1;
    irq_restore(flags);
    return err;
}

int usb_halted(struct usb_device *d, uint8_t epa)
{
    int dci = dci_of(epa);
    return d->ep[dci] && (ctx(d->hc, d->out, dci)[0] & 7) == EP_HALTED;
}

int usb_reset_ep(struct usb_device *d, uint8_t epa)
{
    int dci = dci_of(epa);
    struct ep *ep = d->ep[dci];
    if (!ep)
        return -1;
    uint32_t state = ctx(d->hc, d->out, dci)[0] & 7;
    int halted = state == EP_HALTED;
    if (halted && command(d->hc, 0, T_TYPE(TRB_RESET_EP) | T_EP(dci) | T_SLOT(d->slot), NULL)
                  != CC_SUCCESS)
        return -1;
    if (state == EP_RUNNING)            /* (a timeout): stop it first */
        command(d->hc, 0, T_TYPE(TRB_STOP_EP) | T_EP(dci) | T_SLOT(d->slot), NULL);
    /* Go on after whatever was queued: the ring's next free entry. */
    struct ring *r = &ep->ring;
    if (command(d->hc, (r->phys + r->enq * sizeof(struct trb)) | r->cycle,
                T_TYPE(TRB_SET_DEQ) | T_EP(dci) | T_SLOT(d->slot), NULL) != CC_SUCCESS)
        return -1;
    d->failed = 0;
    return halted;
}

int usb_clear_halt(struct usb_device *d, uint8_t epa)
{
    int halted = usb_reset_ep(d, epa);
    if (halted < 0 || usb_control(d, 0x02, USB_REQ_CLEAR_FEATURE, USB_ENDPOINT_HALT, epa, NULL, 0) < 0)
        return -1;
    return halted;
}

int usb_control(struct usb_device *d, uint8_t type, uint8_t req, uint16_t value,
                uint16_t index, void *data, uint16_t len)
{
    struct ep *ep = d->ep[1];
    int in = type & USB_DIR_IN;
    if (len > 1024)
        return -1;
    if (!in && len)
        memcpy(d->buf, data, len);
    uint32_t flags = irq_save();
    struct ring *r = &ep->ring;
    ring_room(r, 3);
    /* Setup (the 8-byte request, in the TRB itself), data, status. */
    uint32_t trt = !len ? 0 : in ? 3 : 2;
    ring_put(r, type | (uint32_t)req << 8 | (uint32_t)value << 16 | (uint64_t)index << 32 |
                (uint64_t)len << 48, 8, T_TYPE(TRB_SETUP) | T_IDT | trt << 16);
    if (len)
        ring_put(r, d->buf_phys, len, T_TYPE(TRB_DATA) | (in ? T_DIR_IN | T_ISP : 0));
    ep->wait_phys = ring_put(r, 0, 0, T_TYPE(TRB_STATUS) | T_IOC | (len && in ? 0 : T_DIR_IN));
    ep->done = 0;
    ep->residue = 0;
    d->failed = 0;
    doorbell(d->hc, d->slot, 1);
    int err = wait_ep(d, ep, 5000);
    irq_restore(flags);
    if (err) {
        usb_reset_ep(d, 0);             /* a stall: endpoint 0 takes the next request */
        return -1;
    }
    int got = len - (int)ep->residue;
    if (in && got > 0)
        memcpy(data, d->buf, got);
    return got;
}

const struct usb_device_descriptor *usb_desc(struct usb_device *d) { return &d->desc; }
int usb_speed(struct usb_device *d) { return d->speed; }
const char *usb_name(struct usb_device *d) { return d->name; }

/* ---- devices ---- */

static struct ep *ep_new(struct usb_device *d, int dci, uint16_t mps)
{
    struct ep *ep = d->ep[dci];
    if (!ep) {
        ep = kmalloc(sizeof *ep);
        if (!ep)
            return NULL;
        memset(ep, 0, sizeof *ep);
        if (ring_init(&ep->ring) < 0) {
            kfree(ep);
            return NULL;
        }
        d->ep[dci] = ep;
    }
    ep->mps = mps ? mps : 1;
    return ep;
}

int usb_configure(struct usb_device *d, uint8_t config, const struct usb_ep_want *eps, int n)
{
    struct xhci *hc = d->hc;
    memset(hc->in, 0, PAGE_SIZE);
    uint32_t add = 1, last = 1;
    for (int k = 0; k < n; k++) {
        const struct usb_endpoint_descriptor *e = eps[k].desc;
        if ((e->bmAttributes & 3) != USB_ENDPOINT_XFER_BULK)
            return -1;                  /* only bulk so far */
        int in = e->bEndpointAddress & USB_DIR_IN, dci = dci_of(e->bEndpointAddress);
        uint16_t mps = e->wMaxPacketSize & 0x7FF;
        struct ep *ep = ep_new(d, dci, mps);
        if (!ep)
            return -1;
        uint32_t *c = ctx(hc, hc->in, 1 + dci);
        c[1] = (3u << 1) | ((in ? 6u : 2u) << 3) | ((uint32_t)eps[k].burst << 8) |
               ((uint32_t)mps << 16);  /* 3 retries, bulk in/out, burst, packet size */
        c[2] = ep->ring.phys | 1;       /* where it starts; cycle 1 */
        c[4] = 3072;                    /* average TRB length (xHCI 4.14.1.1: bulk) */
        add |= 1u << dci;
        if ((uint32_t)dci > last)
            last = dci;
    }
    ctx(hc, hc->in, 0)[1] = add;
    memcpy(ctx(hc, hc->in, 1), ctx(hc, d->out, 0), 16);       /* the slot as it is, */
    ctx(hc, hc->in, 1)[0] = (ctx(hc, hc->in, 1)[0] & ~(0x1Fu << 27)) | last << 27;  /* more entries */
    if (command(hc, hc->in_phys, T_TYPE(TRB_CONFIG_EP) | T_SLOT(d->slot), NULL) != CC_SUCCESS)
        return -1;
    return usb_control(d, 0x00, USB_REQ_SET_CONFIGURATION, config, 0, NULL, 0) < 0 ? -1 : 0;
}

/* A string descriptor in ASCII ('?' for the rest); "" if none. */
static void get_string(struct usb_device *d, uint8_t index, uint16_t lang, char *out, int size)
{
    uint8_t s[255];
    out[0] = 0;
    int n = index ? usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_STRING << 8 | index,
                                lang, s, sizeof s) : -1;
    if (n < 2 || s[1] != USB_DT_STRING)
        return;
    if (n > s[0])
        n = s[0];
    int o = 0;
    for (int i = 2; i + 1 < n && o < size - 1; i += 2)
        out[o++] = (s[i + 1] || s[i] < ' ' || s[i] > '~') ? '?' : (char)s[i];
    while (o && out[o - 1] == ' ')
        o--;
    out[o] = 0;
}

static void name_device(struct usb_device *d)
{
    uint8_t langs[4];
    char maker[24], product[32];
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_STRING << 8, 0, langs, 4) < 4)
        return;                         /* no strings */
    uint16_t lang = langs[2] | langs[3] << 8;
    get_string(d, d->desc.iManufacturer, lang, maker, sizeof maker);
    get_string(d, d->desc.iProduct, lang, product, sizeof product);
    const char *m = maker;
    while (*m == ' ')
        m++;
    int n = 0;
    for (const char *s = m; *s && n < (int)sizeof d->name - 1; )
        d->name[n++] = *s++;
    if (n && *product && n < (int)sizeof d->name - 1)
        d->name[n++] = ' ';
    for (const char *s = product; *s && n < (int)sizeof d->name - 1; )
        d->name[n++] = *s++;
    d->name[n] = 0;
}

static const char *speed_name(int speed)
{
    switch (speed) {
    case USB_SPEED_LOW:   return "1.5 Mb/s";
    case USB_SPEED_FULL:  return "12 Mb/s";
    case USB_SPEED_HIGH:  return "480 Mb/s";
    case USB_SPEED_SUPER: return "5 Gb/s";
    case 5:               return "10 Gb/s";
    default:              return "? Mb/s";
    }
}

static void device_free(struct usb_device *d)
{
    for (int i = 0; i < 32; i++)
        if (d->ep[i]) {
            pmm_free(d->ep[i]->ring.phys);
            kfree(d->ep[i]);
        }
    if (d->out_phys)
        pmm_free(d->out_phys);
    if (d->buf_phys)
        pmm_free(d->buf_phys);
    kfree(d);
}

/* A device on root port p, just reset: give it a slot and an address,
   read what it is, and find it a driver. */
static struct usb_device *attach(struct xhci *hc, int p)
{
    int bus = (int)(hc - hcs) + 1;
    uint8_t speed = PS_SPEED(rd(hc->op, OP_PORTSC(p)));
    uint8_t slot = 0;
    if (command(hc, 0, T_TYPE(TRB_ENABLE_SLOT), &slot) != CC_SUCCESS || !slot || slot > hc->slots)
        return NULL;
    struct usb_device *d = kmalloc(sizeof *d);
    if (!d)
        return NULL;
    memset(d, 0, sizeof *d);
    d->hc = hc;
    d->slot = slot;
    d->port = (uint8_t)p;
    d->speed = speed;
    /* Endpoint 0's packet size: fixed by the speed, except that a
       full-speed device may use 8 to 64 (64 is the guess; fixed below). */
    uint16_t mps = speed >= USB_SPEED_SUPER ? 512 : speed == USB_SPEED_LOW ? 8 : 64;
    if (!(d->out = page(&d->out_phys)) || !(d->buf = page(&d->buf_phys)) || !ep_new(d, 1, mps))
        goto fail;
    hc->dcbaa[slot] = d->out_phys;
    hc->dev[slot] = d;

    memset(hc->in, 0, PAGE_SIZE);
    ctx(hc, hc->in, 0)[1] = 3;          /* add: the slot and endpoint 0 */
    uint32_t *s = ctx(hc, hc->in, 1);
    s[0] = (1u << 27) | ((uint32_t)speed << 20);   /* one endpoint context; the speed */
    s[1] = (uint32_t)p << 16;                      /* its root port */
    uint32_t *e = ctx(hc, hc->in, 2);
    e[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16);  /* 3 retries, control, packet size */
    e[2] = d->ep[1]->ring.phys | 1;
    e[4] = 8;                           /* average TRB length: a setup packet */
    if (command(hc, hc->in_phys, T_TYPE(TRB_ADDRESS) | T_SLOT(slot), NULL) != CC_SUCCESS)
        goto fail;
    usb_sleep(10);                          /* SET_ADDRESS recovery (2 ms; Linux waits 10) */

    /* The first 8 bytes hold bMaxPacketSize0. */
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, &d->desc, 8) < 8)
        goto fail;
    if (speed == USB_SPEED_FULL && d->desc.bMaxPacketSize0 != mps) {
        mps = d->desc.bMaxPacketSize0;
        memset(hc->in, 0, PAGE_SIZE);
        ctx(hc, hc->in, 0)[1] = 2;      /* change endpoint 0 */
        ctx(hc, hc->in, 2)[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16);
        if (command(hc, hc->in_phys, T_TYPE(TRB_EVALUATE) | T_SLOT(slot), NULL) != CC_SUCCESS)
            goto fail;
        d->ep[1]->mps = mps;
    }
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, &d->desc,
                    sizeof d->desc) < (int)sizeof d->desc)
        goto fail;
    name_device(d);

    /* The first configuration: 9 bytes say how long all of it is. */
    uint8_t *cfg = kmalloc(1024);
    struct usb_config_descriptor *c = (struct usb_config_descriptor *)cfg;
    int len = cfg ? usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, cfg, 9) : -1;
    if (len == 9)
        len = usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, cfg,
                          c->wTotalLength > 1024 ? 1024 : c->wTotalLength);
    if (len < 9) {
        kfree(cfg);
        goto fail;
    }
    /* The class is in the device descriptor, or (class 0) per interface. */
    int cls = d->desc.bDeviceClass;
    for (int o = 0; !cls && o + 9 <= len && cfg[o]; o += cfg[o])
        if (cfg[o + 1] == USB_DT_INTERFACE)
            cls = cfg[o + 5];
    klog("usb %d-%d: %04x:%04x %s%s(USB %x.%x, %s), class %d\n", bus, p,
            d->desc.idVendor, d->desc.idProduct, d->name, *d->name ? " " : "",
            d->desc.bcdUSB >> 8, (d->desc.bcdUSB >> 4) & 0xF, speed_name(speed), cls);
    if (usb_storage_probe(d, cfg, len) < 0) {
        if (cls == 9)
            kprintf("usb %d-%d: a hub: not supported yet\n", bus, p);
    }
    kfree(cfg);
    return d;
fail:
    kprintf("usb %d-%d: the device did not answer (%s)\n", bus, p, speed_name(speed));
    hc->dev[slot] = NULL;
    hc->dcbaa[slot] = 0;
    device_free(d);
    return NULL;
}

/* Reset port p: a USB 2 port is enabled by it; a USB 3 port is up
   already, but the reset puts the device in a known state. A USB 3 link
   that is stuck needs a "warm" reset. 0 if the port is enabled after. */
static int port_reset(struct xhci *hc, int p)
{
    uint32_t sc = rd(hc->op, OP_PORTSC(p));
    int warm = hc->usb3[p] && (PS_PLS(sc) == PLS_INACTIVE || PS_PLS(sc) == PLS_COMPLIANCE);
    wr(hc->op, OP_PORTSC(p), (sc & PS_KEEP) | (warm ? PS_WPR : PS_PR));
    uint32_t end = timer_ms() + 500;
    while (!((sc = rd(hc->op, OP_PORTSC(p))) & (warm ? PS_WRC : PS_PRC))) {
        if ((int32_t)(timer_ms() - end) > 0)
            return -1;
        usb_sleep(2);
    }
    wr(hc->op, OP_PORTSC(p), (sc & PS_KEEP) | (sc & PS_CHANGES));
    usb_sleep(20);                          /* reset recovery (10 ms) and some */
    return (rd(hc->op, OP_PORTSC(p)) & PS_PED) ? 0 : -1;
}

static void ports_scan(struct xhci *hc, int debounce)
{
    int bus = (int)(hc - hcs) + 1;
    for (int p = 1; p <= hc->ports; p++) {
        uint32_t sc = rd(hc->op, OP_PORTSC(p));
        if (sc & PS_CHANGES)
            wr(hc->op, OP_PORTSC(p), (sc & PS_KEEP) | (sc & PS_CHANGES));
        if (!(sc & PS_CCS)) {
            if (hc->port_dev[p])
                kprintf("usb %d-%d: unplugged (the kernel can't let go of it yet)\n", bus, p);
            hc->port_dev[p] = NULL;
            hc->port_tried[p] = 0;
            continue;
        }
        if (hc->port_dev[p] || hc->port_tried[p])
            continue;
        hc->port_tried[p] = 1;
        if (debounce)
            usb_sleep(100);                 /* a new plug settles (USB 2.0 7.1.7.3) */
        if (port_reset(hc, p) < 0) {
            kprintf("usb %d-%d: the port did not come up\n", bus, p);
            continue;
        }
        hc->port_dev[p] = attach(hc, p);
    }
}

/* The firmware may be using the controller (USB keyboards in its setup
   screens): ask for it (xHCI 7.1, "USB Legacy Support"). */
static void take_over(struct xhci *hc, uint32_t off)
{
    volatile uint32_t *c = (volatile uint32_t *)(hc->cap + off);
    if (c[0] & (1u << 16)) {
        c[0] |= 1u << 24;               /* "the OS owns it" */
        uint32_t end = timer_ms() + 1000;
        while ((c[0] & (1u << 16)) && (int32_t)(timer_ms() - end) < 0)
            usb_sleep(5);
        if (c[0] & (1u << 16)) {
            kprintf("xhci: the firmware did not let go: taking it anyway\n");
            c[0] &= ~(1u << 16);
        }
    }
    c[0] |= 1u << 24;
    /* No more firmware interrupts (SMIs): clear their enable bits, keep
       the reserved ones, clear the status bits (as Linux does). */
    c[1] = (c[1] & ((7u << 1) | (0xFFu << 5) | (7u << 17))) | (7u << 29);
}

/* Walk the extended capabilities: the firmware hand-off, and which
   ports are USB 3 ("Supported Protocol"). */
static void ext_caps(struct xhci *hc, uint32_t off)
{
    for (int guard = 0; off && guard < 64; guard++) {
        uint32_t v = rd(hc->cap, off);
        if ((v & 0xFF) == 1)
            take_over(hc, off);
        if ((v & 0xFF) == 2 && (v >> 24) >= 3) {
            uint32_t r = rd(hc->cap, off + 8);
            for (uint32_t p = r & 0xFF; p < (r & 0xFF) + ((r >> 8) & 0xFF) && p <= MAX_PORTS; p++)
                hc->usb3[p] = 1;
        }
        off = (v >> 8) & 0xFF ? off + ((v >> 8) & 0xFF) * 4 : 0;
    }
}

static int wait_reg(volatile uint8_t *base, uint32_t r, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint32_t end = timer_ms() + ms;
    while ((rd(base, r) & mask) != want) {
        if ((int32_t)(timer_ms() - end) > 0)
            return -1;
        usb_sleep(1);
    }
    return 0;
}

/* The firmware may have left the controller powered down (D3, PCI power
   management): its registers then read as all ones. Back to D0; coming
   out of D3 may reset the device, BARs included, so they are written
   back. */
static void power_on(struct pci_dev *pci)
{
    uint8_t pm = pci_find_capability(pci, 0x01);
    if (!pm || !(pci_read_config(pci, pm + 4) & 3))
        return;                         /* already D0 */
    pci_write_config(pci, pm + 4, pci_read_config(pci, pm + 4) & ~3u);
    usb_sleep(10);                      /* D3 -> D0 takes 10 ms (PCI PM 1.2) */
    for (int i = 0; i < 6; i++)
        pci_write_config(pci, (uint8_t)(0x10 + i * 4), pci->bar[i]);
    klog("xhci: %04x:%04x was powered down: woke it\n", pci->vendor, pci->device);
}

static int hc_start(struct pci_dev *pci)
{
    phys_addr_t bar = pci_bar_phys(pci, 0);
    struct xhci *hc = &hcs[nhc];
    power_on(pci);
    memset(hc, 0, sizeof *hc);
    hc->pci = pci;
    /* 64 KiB covers every register (Intel's BAR is that big; QEMU's is
       smaller, and nothing past it is touched). */
    hc->cap = bar ? pci_map_mmio(bar, 0x10000) : NULL;
    if (!hc->cap) {
        kprintf("xhci: cannot map its registers at %08lx%08lx\n",
                (unsigned long)(bar >> 32), (unsigned long)bar);
        return -1;
    }
    pci_write_config(pci, 4, pci_read_config(pci, 4) | 0x6);  /* memory + bus master */
    uint32_t v = rd(hc->cap, CAP_LENGTH);
    if (v == 0xFFFFFFFFu) {
        kprintf("xhci: %04x:%04x does not answer\n", pci->vendor, pci->device);
        return -1;
    }
    hc->version = v >> 16;
    hc->op = hc->cap + (v & 0xFF);
    hc->rt = hc->cap + (rd(hc->cap, CAP_RTSOFF) & ~0x1Fu);
    hc->db = hc->cap + (rd(hc->cap, CAP_DBOFF) & ~3u);
    uint32_t hcs1 = rd(hc->cap, CAP_HCSPARAMS1), hcs2 = rd(hc->cap, CAP_HCSPARAMS2);
    uint32_t hcc1 = rd(hc->cap, CAP_HCCPARAMS1);
    hc->slots = (hcs1 & 0xFF) < MAX_SLOTS ? (hcs1 & 0xFF) : MAX_SLOTS;
    hc->ports = (hcs1 >> 24) < MAX_PORTS ? (hcs1 >> 24) : MAX_PORTS;
    hc->csz = (hcc1 & 4) ? 64 : 32;
    ext_caps(hc, (hcc1 >> 16) * 4);

    /* Stop it, then reset it (Intel's needs a moment before it is looked
       at again: Linux's XHCI_INTEL_HOST quirk). */
    wr(hc->op, OP_USBCMD, rd(hc->op, OP_USBCMD) & ~CMD_RUN);
    if (wait_reg(hc->op, OP_USBSTS, STS_HCH, STS_HCH, 100) < 0)
        kprintf("xhci: it did not stop\n");
    wr(hc->op, OP_USBCMD, CMD_HCRST);
    usb_sleep(1);
    if (wait_reg(hc->op, OP_USBCMD, CMD_HCRST, 0, 1000) < 0 ||
        wait_reg(hc->op, OP_USBSTS, STS_CNR, 0, 1000) < 0) {
        kprintf("xhci: reset did not finish\n");
        return -1;
    }
    if (!(rd(hc->op, OP_PAGESIZE) & 1)) {
        kprintf("xhci: no 4 KiB pages\n");
        return -1;
    }

    /* The device context pointers (with the event ring's segment table in
       the same page), the scratchpad pages the controller asked for, the
       command ring, the event ring, the input context. */
    uint32_t dcbaa_phys, sp_phys = 0, in_phys;
    if (!(hc->dcbaa = page(&dcbaa_phys)) || ring_init(&hc->cmd) < 0 ||
        !(hc->ev = page(&hc->ev_phys)) || !(hc->in = page(&in_phys)))
        return -1;
    hc->in_phys = in_phys;
    uint32_t nsp = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (nsp > PAGE_SIZE / 8) {
        kprintf("xhci: it wants %lu scratchpad pages\n", (unsigned long)nsp);
        return -1;
    }
    if (nsp) {
        uint64_t *sp = page(&sp_phys);
        if (!sp)
            return -1;
        for (uint32_t k = 0; k < nsp; k++) {
            uint32_t pa;
            if (!page(&pa))
                return -1;
            sp[k] = pa;
        }
        hc->dcbaa[0] = sp_phys;
    }
    wr(hc->op, OP_CONFIG, hc->slots);
    wr64(hc->op, OP_DCBAAP, dcbaa_phys);
    wr64(hc->op, OP_CRCR, hc->cmd.phys | 1);

    uint32_t *erst = (uint32_t *)((uint8_t *)hc->dcbaa + 2048);
    erst[0] = hc->ev_phys;
    erst[1] = 0;
    erst[2] = RING_TRBS;
    hc->ev_deq = 0;
    hc->ev_cycle = 1;
    wr(hc->rt, IR_ERSTSZ, 1);
    wr64(hc->rt, IR_ERDP, hc->ev_phys);
    wr64(hc->rt, IR_ERSTBA, dcbaa_phys + 2048);
    wr(hc->rt, IR_IMOD, 160);           /* at most one interrupt per 40 us */
    wr(hc->rt, IR_IMAN, IMAN_IE | IMAN_IP);

    if (pci_alloc_irq_vectors(pci, 1, 1, PCI_IRQ_MSIX | PCI_IRQ_MSI | PCI_IRQ_INTX) < 0)
        kprintf("xhci: no interrupt: polling\n");
    else
        irq_install(pci_irq_vector(pci, 0), xhci_irq);
    nhc++;                              /* the interrupt handler may look at it now */
    wr(hc->op, OP_USBCMD, CMD_RUN | CMD_INTE);
    if (wait_reg(hc->op, OP_USBSTS, STS_HCH, 0, 100) < 0) {
        kprintf("xhci: it did not start\n");
        nhc--;
        return -1;
    }
    for (int p = 1; p <= hc->ports; p++) {
        uint32_t sc = rd(hc->op, OP_PORTSC(p));
        if (!(sc & PS_PP))
            wr(hc->op, OP_PORTSC(p), (sc & PS_KEEP) | PS_PP);
    }
    int first3 = 0, n3 = 0;
    for (int p = 1; p <= hc->ports; p++)
        if (hc->usb3[p] && !n3++)
            first3 = p;
    klog("xhci: %04x:%04x, xHCI %x.%02x, %d ports", pci->vendor, pci->device,
            hc->version >> 8, hc->version & 0xFF, hc->ports);
    if (n3)
        klog(" (USB 3: %d-%d)", first3, first3 + n3 - 1);
    klog(", %d slots, %s irq %d\n", hc->slots,
            pci->irq_type == PCI_IRQ_MSIX ? "MSI-X" : pci->irq_type == PCI_IRQ_MSI ? "MSI" : "pin",
            pci_irq_vector(pci, 0));
    return 0;
}

static void usb_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < nfound; i++)
        hc_start(found[i]);
    /* Give connections time to show: USB 2 devices pull their line up as
       soon as there is power; a USB 3 link trains first ("polling"). */
    if (nhc)
        usb_sleep(100);
    for (int i = 0; i < nhc; i++) {
        uint32_t end = timer_ms() + 1000;
        for (int p = 1; p <= hcs[i].ports; p++)
            while (hcs[i].usb3[p] && PS_PLS(rd(hcs[i].op, OP_PORTSC(p))) == PLS_POLLING &&
                   (int32_t)(timer_ms() - end) < 0)
                usb_sleep(10);
        ports_scan(&hcs[i], 0);
    }
    uint32_t flags = irq_save();
    boot_done = 1;
    wq_wake_all(&boot_wq);
    for (;;) {                          /* then: something plugged in later */
        int i;
        for (i = 0; i < nhc && !hcs[i].port_change; i++)
            ;
        if (i == nhc) {
            wq_sleep(&port_wq);         /* (interrupts are off: no wake-up is missed) */
            continue;
        }
        hcs[i].port_change = 0;
        irq_restore(flags);
        ports_scan(&hcs[i], 1);
        flags = irq_save();
    }
}

void usb_init(void)
{
    for (int i = 0; i < pci_count() && nfound < MAX_HC; i++)
        if (pci_dev_at(i)->class_rev >> 8 == 0x0C0330)        /* USB, xHCI */
            found[nfound++] = pci_dev_at(i);
    if (nfound && !thread_create("usb", usb_thread, NULL)) {
        kprintf("usb: no thread\n");
        nfound = 0;
    }
}

void usb_wait_boot(uint32_t ms)
{
    if (!nfound)
        return;
    uint32_t flags = irq_save(), end = timer_ms() + ms;
    while (!boot_done && !wq_sleep_until(&boot_wq, end))
        ;
    irq_restore(flags);
    if (!boot_done)
        kprintf("usb: still looking for devices: going on without them\n");
}
