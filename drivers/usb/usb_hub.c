/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * usb_hub.c — USB hubs (the hub class, 9)
 *
 * A hub turns one port into several. To the host it is a device like any
 * other, with a control pipe and one interrupt-IN endpoint, and it answers
 * "class requests" about its ports (USB 2.0 chapter 11):
 *
 *   GET_DESCRIPTOR 0x29       how many ports; how long their power takes
 *   SET_FEATURE PORT_POWER    switch a port's power on
 *   GET_STATUS (a port)       connected? enabled? its speed; what changed
 *   SET_FEATURE PORT_RESET    reset the device on it: it then answers at
 *                             address 0, ready to be given an address
 *   CLEAR_FEATURE C_PORT_...  "I have seen that change"
 *
 * The interrupt endpoint says which ports changed: a bitmap, bit n for
 * port n (bit 0 is the hub itself). The host controller asks the hub for
 * it every bInterval (128 ms or so), so a plug costs the CPU nothing until
 * it happens. The report arrives in the controller's interrupt, which only
 * notes it (usb_work); the requests sleep, so the usb thread sends them.
 *
 * What is plugged in at boot is found inside the probe, in the usb thread,
 * before usb_wait_boot returns: a USB stick behind a hub is there in time
 * to be the root disk. A hub behind a hub is probed the same way, from
 * within (at most 5 deep: USB's limit, and the route string's).
 *
 * A high-speed (USB 2) hub speaks to its low- and full-speed devices
 * (keyboards, mice) through its "transaction translator", which the host
 * controller needs to know about (usb_hub_set: its think time, from
 * wHubCharacteristics bits 6:5). A hub with one TT per port ("multi-TT")
 * is used as a single-TT one, its alternate setting 0, which is allowed.
 *
 * A USB 3 hub is two hubs in one box: a USB 2 hub on the USB 2 side of the
 * cable, which this driver runs (keyboards, mice and USB 2 sticks on it
 * work), and a USB 3 hub on the USB 3 side, which it doesn't yet (that
 * needs SET_HUB_DEPTH and USB 3's port status layout): USB 3 sticks
 * plugged into one aren't seen.
 */
#include <interrupts.h>
#include <kheap.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pmm.h>
#include <string.h>
#include <usb.h>

#define USB_DT_HUB        0x29
#define REQ_GET_STATUS    0
#define REQ_CLEAR_FEATURE 1
#define REQ_SET_FEATURE   3
#define RT_HUB_IN         0xA0          /* class request to the hub, data in */
#define RT_HUB_OUT        0x20
#define RT_PORT_IN        0xA3          /* class request to one of its ports */
#define RT_PORT_OUT       0x23

/* Feature selectors (USB 2.0 table 11-17). The "changed" ones are 16 + the
   bit in wPortChange. */
#define F_C_HUB_LOCAL_POWER  0
#define F_C_HUB_OVER_CURRENT 1
#define F_PORT_RESET      4
#define F_PORT_POWER      8
#define F_C_PORT          16            /* + the change bit: CONNECTION, ENABLE, ... */
#define F_C_PORT_RESET    20

/* wPortStatus, and wPortChange (USB 2.0 tables 11-21, 11-22) */
#define ST_CONNECTION     (1u << 0)
#define ST_ENABLE         (1u << 1)
#define ST_LOW_SPEED      (1u << 9)
#define ST_HIGH_SPEED     (1u << 10)
#define CH_CONNECTION     (1u << 0)
#define CH_OVER_CURRENT   (1u << 3)
#define CH_RESET          (1u << 4)

#define MAX_PORTS         15            /* the most a route string's 4 bits name */

struct hub {
    struct usb_device *d;
    uint8_t ports;
    uint8_t *buf;                       /* the controller writes the change bitmap here */
    volatile uint16_t changed;          /* ports that changed, from the interrupt */
    struct usb_device *child[MAX_PORTS + 1];
};

static int port_status(struct hub *h, int p, uint16_t *status, uint16_t *change)
{
    uint8_t s[4];
    if (usb_control(h->d, RT_PORT_IN, REQ_GET_STATUS, 0, (uint16_t)p, s, 4) < 4)
        return -1;
    *status = (uint16_t)(s[0] | s[1] << 8);
    *change = (uint16_t)(s[2] | s[3] << 8);
    return 0;
}

static int port_feature(struct hub *h, uint8_t req, uint16_t feature, int p)
{
    return usb_control(h->d, RT_PORT_OUT, req, feature, (uint16_t)p, NULL, 0);
}

/* Something is on port p: reset it, and set up the device. */
static void port_attach(struct hub *h, int p)
{
    uint16_t st = 0, ch = 0;
    usb_sleep(100);                     /* a new plug settles (USB 2.0 7.1.7.3) */
    if (port_feature(h, REQ_SET_FEATURE, F_PORT_RESET, p) < 0)
        return;
    for (int t = 0; t < 50 && !(ch & CH_RESET); t++) {  /* 10-20 ms; at most 500 */
        usb_sleep(10);
        if (port_status(h, p, &st, &ch) < 0)
            return;
    }
    port_feature(h, REQ_CLEAR_FEATURE, F_C_PORT_RESET, p);
    if (!(st & ST_CONNECTION))
        return;                         /* unplugged meanwhile */
    if (!(st & ST_ENABLE)) {
        kprintf("usb %s.%d: the port did not come up\n", usb_path(h->d), p);
        return;
    }
    usb_sleep(10);                      /* reset recovery (USB 2.0 7.1.7.5) */
    int speed = (st & ST_LOW_SPEED) ? USB_SPEED_LOW : (st & ST_HIGH_SPEED) ? USB_SPEED_HIGH
                                                                           : USB_SPEED_FULL;
    h->child[p] = usb_hub_attach(h->d, p, speed);
}

/* Port p changed (or, at the start, may have something on it). */
static void port_check(struct hub *h, int p)
{
    uint16_t st, ch;
    if (port_status(h, p, &st, &ch) < 0)
        return;
    for (int k = 0; k < 5; k++)         /* say we've seen each change */
        if (ch & (1u << k))
            port_feature(h, REQ_CLEAR_FEATURE, (uint16_t)(F_C_PORT + k), p);
    if (ch & CH_OVER_CURRENT)
        kprintf("usb %s.%d: too much current: the hub switched the port off\n",
                usb_path(h->d), p);
    /* Gone, or unplugged and plugged in again since we looked. */
    if (h->child[p] && (!(st & ST_CONNECTION) || (ch & CH_CONNECTION))) {
        kprintf("usb %s: unplugged (the kernel can't let go of it yet)\n", usb_path(h->child[p]));
        usb_unplugged(h->child[p]);
        h->child[p] = NULL;
    }
    if ((st & ST_CONNECTION) && !h->child[p])
        port_attach(h, p);
}

/* The usb thread, after the hub said something changed (hub_irq). */
static void hub_work(void *arg)
{
    struct hub *h = arg;
    uint32_t flags = irq_save();
    uint16_t ch = h->changed;
    h->changed = 0;
    irq_restore(flags);
    if (ch & 1) {                       /* the hub itself: its power supply, its current */
        uint8_t s[4];
        if (usb_control(h->d, RT_HUB_IN, REQ_GET_STATUS, 0, 0, s, 4) == 4) {
            if (s[2] & 1)
                usb_control(h->d, RT_HUB_OUT, REQ_CLEAR_FEATURE, F_C_HUB_LOCAL_POWER, 0, NULL, 0);
            if (s[2] & 2)
                usb_control(h->d, RT_HUB_OUT, REQ_CLEAR_FEATURE, F_C_HUB_OVER_CURRENT, 0, NULL, 0);
        }
    }
    for (int p = 1; p <= h->ports; p++)
        if (ch & (1u << p))
            port_check(h, p);
}

/* In the controller's interrupt: the hub's bitmap of changed ports. */
static void hub_irq(void *arg, int n)
{
    struct hub *h = arg;
    h->changed |= (uint16_t)(h->buf[0] | (n > 1 ? h->buf[1] << 8 : 0));
    usb_work(h->d);
}

int usb_hub_probe(struct usb_device *d, const uint8_t *cfg, int len)
{
    /* The hub interface and its interrupt-IN endpoint. */
    const struct usb_endpoint_descriptor *ep = NULL;
    int in_hub = 0, found = 0;
    for (int o = 0; o + 2 <= len && cfg[o] >= 2 && o + cfg[o] <= len; o += cfg[o]) {
        const uint8_t *x = cfg + o;
        if (x[1] == USB_DT_INTERFACE && x[0] >= 9) {
            const struct usb_interface_descriptor *i = (const void *)x;
            in_hub = i->bInterfaceClass == USB_CLASS_HUB && i->bAlternateSetting == 0;
            found |= in_hub;
        } else if (in_hub && !ep && x[1] == USB_DT_ENDPOINT && x[0] >= 7) {
            const struct usb_endpoint_descriptor *e = (const void *)x;
            if ((e->bEndpointAddress & USB_DIR_IN) && (e->bmAttributes & 3) == USB_ENDPOINT_XFER_INT)
                ep = e;
        }
    }
    if (!found)
        return -1;
    const char *at = usb_path(d);
    if (usb_speed(d) >= USB_SPEED_SUPER) {
        klog("usb-hub %s: the USB 3 side of a USB 3 hub: not supported yet "
             "(USB 2 devices on the hub work)\n", at);
        return 0;
    }
    uint8_t desc[16];
    int got = ep ? usb_control(d, RT_HUB_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_HUB << 8, 0,
                               desc, sizeof desc) : -1;
    if (got < 7 || desc[1] != USB_DT_HUB) {
        kprintf("usb-hub %s: no hub descriptor\n", at);
        return 0;
    }
    int ports = desc[2], ttt = (desc[3] >> 5) & 3;     /* wHubCharacteristics bits 6:5 */
    uint32_t power_ms = desc[5] * 2u < 100 ? 100 : desc[5] * 2u;   /* as Linux waits */
    if (ports > MAX_PORTS) {
        kprintf("usb-hub %s: %d ports: MyOS uses the first %d\n", at, ports, MAX_PORTS);
        ports = MAX_PORTS;
    }
    struct hub *h = kmalloc(sizeof *h);
    uint32_t phys = h ? pmm_alloc() : 0;
    if (!phys) {
        kfree(h);
        return 0;
    }
    memset(h, 0, sizeof *h);
    h->d = d;
    h->ports = (uint8_t)ports;
    h->buf = P2V(phys);
    usb_hub_set(d, ports, ttt);
    struct usb_ep_want want = { ep, 0 };
    if (usb_configure(d, ((const struct usb_config_descriptor *)cfg)->bConfigurationValue,
                      &want, 1) < 0) {
        kprintf("usb-hub %s: could not configure it\n", at);
        pmm_free(phys);
        kfree(h);
        return 0;
    }
    klog("usb-hub %s: %d ports\n", at, ports);
    for (int p = 1; p <= ports; p++)
        port_feature(h, REQ_SET_FEATURE, F_PORT_POWER, p);
    usb_sleep(power_ms);
    usb_set_work(d, hub_work, h);
    for (int p = 1; p <= ports; p++)
        port_check(h, p);
    /* From now on the hub says when a port changes. A change since the
       ports were read is still flagged in the hub, and reported at once. */
    uint16_t mps = ep->wMaxPacketSize & 0x7FF;
    if (usb_intr_start(d, ep->bEndpointAddress, phys, mps ? mps : 2, hub_irq, h) < 0)
        kprintf("usb-hub %s: can't listen to it: what is plugged in later won't be seen\n", at);
    return 0;
}
