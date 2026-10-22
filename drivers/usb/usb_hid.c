/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * usb_hid.c — USB keyboards and mice (the HID class, 3)
 *
 * A USB keyboard or mouse is a "Human Interface Device". It says how its
 * reports are laid out in a report descriptor (hid.c reads it) and sends a
 * report on its interrupt-IN endpoint whenever something changes: the keys
 * held now, or how far it moved and which buttons are down. The host
 * controller asks for one every bInterval (8-10 ms for most keyboards,
 * 1 ms for gaming mice), and a device with no news answers "nothing"
 * (a NAK). That costs the CPU nothing: the controller polls, not the
 * kernel. MyOS hears only about real reports, in the controller's
 * interrupt (usb_intr_start), which hid_report turns into key and
 * movement events then and there: keyboard_key and mouse_report, the same
 * paths a PS/2 keyboard and mouse take. So the console and X see a USB
 * keyboard as /dev/input/event0 and a USB mouse as event1, as before.
 *
 * One device can hold several HID interfaces. A laptop's built-in
 * keyboard often is a keyboard and a touchpad in one (the GPD P2 Max's
 * HAILUCK keyboard is): each interface has its own report descriptor and
 * endpoint, and gets its own struct hid.
 *
 * SET_IDLE 0 asks for reports only on a change. Without it, keyboards
 * repeat their last report every 500 ms: two wake-ups a second for
 * nothing.
 */
#include <hid.h>
#include <kheap.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pmm.h>
#include <string.h>
#include <usb.h>

#define USB_DT_HID     0x21             /* after a HID interface: its descriptors */
#define USB_DT_REPORT  0x22             /* the report descriptor */
#define HID_SET_IDLE   0x0A
#define MAX_IFS        4                /* HID interfaces per device */
#define DESC_MAX       1024             /* the longest report descriptor read */

struct hid_if {
    struct hid hid;
    uint8_t *buf;                       /* the controller writes each report here */
};

/* In the controller's interrupt: a report of n bytes came. */
static void report_in(void *arg, int n)
{
    struct hid_if *h = arg;
    hid_report(&h->hid, h->buf, n);
}

int usb_hid_probe(struct usb_device *d, const uint8_t *cfg, int len)
{
    /* Each HID interface: its number, its report descriptor's length and
       its interrupt-IN endpoint. */
    struct { uint8_t num; uint16_t rlen; const struct usb_endpoint_descriptor *ep; } f[MAX_IFS];
    int n = 0, cur = -1;                /* cur: the HID interface being read */
    for (int o = 0; o + 2 <= len && cfg[o] >= 2 && o + cfg[o] <= len; o += cfg[o]) {
        const uint8_t *x = cfg + o;
        if (x[1] == USB_DT_INTERFACE && x[0] >= 9) {
            const struct usb_interface_descriptor *i = (const void *)x;
            if (cur >= 0 && f[cur].ep && f[cur].rlen)
                n++;                    /* the one before is complete */
            cur = -1;
            if (i->bInterfaceClass == USB_CLASS_HID && i->bAlternateSetting == 0 && n < MAX_IFS) {
                cur = n;
                f[n].num = i->bInterfaceNumber;
                f[n].rlen = 0;
                f[n].ep = NULL;
            }
        } else if (cur >= 0 && x[1] == USB_DT_HID && x[0] >= 9) {
            /* bNumDescriptors at 5, then (type, length) pairs of 3 bytes */
            for (int k = 0; k < x[5] && 8 + 3 * k < x[0]; k++)
                if (x[6 + 3 * k] == USB_DT_REPORT)
                    f[cur].rlen = (uint16_t)(x[7 + 3 * k] | x[8 + 3 * k] << 8);
        } else if (cur >= 0 && x[1] == USB_DT_ENDPOINT && x[0] >= 7) {
            const struct usb_endpoint_descriptor *e = (const void *)x;
            if (!f[cur].ep && (e->bEndpointAddress & USB_DIR_IN) &&
                (e->bmAttributes & 3) == USB_ENDPOINT_XFER_INT)
                f[cur].ep = e;
        }
    }
    if (cur >= 0 && f[cur].ep && f[cur].rlen)
        n++;
    if (!n)
        return -1;

    const char *name = *usb_name(d) ? usb_name(d) : "a USB device";
    struct usb_ep_want eps[MAX_IFS] = { { 0 } };
    for (int k = 0; k < n; k++)
        eps[k].desc = f[k].ep;
    if (usb_configure(d, ((const struct usb_config_descriptor *)cfg)->bConfigurationValue,
                      eps, n) < 0) {
        kprintf("usb-hid: %s: could not configure it\n", name);
        return 0;
    }
    uint8_t *desc = kmalloc(DESC_MAX);
    if (!desc)
        return 0;
    int keyboards = 0, mice = 0, touch = 0, pads = 0;
    for (int k = 0; k < n; k++) {
        usb_control(d, 0x21, HID_SET_IDLE, 0, f[k].num, NULL, 0);  /* (a mouse may refuse) */
        int got = usb_control(d, 0x81, USB_REQ_GET_DESCRIPTOR, USB_DT_REPORT << 8, f[k].num,
                              desc, f[k].rlen < DESC_MAX ? f[k].rlen : DESC_MAX);
        struct hid_if *h = got > 0 ? kmalloc(sizeof *h) : NULL;
        uint32_t phys = h ? pmm_alloc() : 0;
        if (!phys || hid_parse(&h->hid, desc, got) < 0) {
            if (phys)
                pmm_free(phys);
            kfree(h);
            continue;                   /* nothing MyOS uses (a vendor's own reports) */
        }
        h->buf = P2V(phys);
        /* One report per transfer: the longest the descriptor allows, and
           at least a packet (a transfer ends at a short packet anyway). */
        uint16_t mps = f[k].ep->wMaxPacketSize & 0x7FF;
        uint16_t want = h->hid.bytes > mps ? h->hid.bytes : mps;
        if (want > PAGE_SIZE)
            want = PAGE_SIZE;
        if (usb_intr_start(d, f[k].ep->bEndpointAddress, phys, want, report_in, h) < 0) {
            pmm_free(phys);
            kfree(h);
            continue;
        }
        keyboards += h->hid.keyboard;
        mice += h->hid.mouse && !h->hid.touchpad;   /* (a touchpad's mouse mode is the touchpad) */
        touch += h->hid.touch;
        pads += h->hid.touchpad;
        /* A pointer that knows where it is: its descriptor, to dmesg, for
           working out a new pad or screen from a boot log. */
        if (h->hid.touch || h->hid.touchpad) {
            static const char hex[] = "0123456789abcdef";
            for (int o = 0; o < got; o += 32) {
                char line[3 * 32 + 1];
                int m = 0;
                for (int b = o; b < got && b < o + 32; b++) {
                    line[m++] = hex[desc[b] >> 4];
                    line[m++] = hex[desc[b] & 15];
                    line[m++] = ' ';
                }
                line[m ? m - 1 : 0] = 0;
                klog("usb-hid: %s if %d desc %03x: %s\n", name, f[k].num, o, line);
            }
        }
    }
    kfree(desc);
    const char *what[4];
    int nw = 0;
    if (keyboards)
        what[nw++] = "a keyboard";
    if (mice)
        what[nw++] = "a mouse";
    if (pads)
        what[nw++] = "a touchpad";
    if (touch)
        what[nw++] = "a touch screen or tablet";
    klog("usb-hid: %s: %s%s%s%s%s%s%s\n", name, nw ? what[0] : "nothing MyOS uses yet",
         nw > 1 ? (nw > 2 ? ", " : " and ") : "", nw > 1 ? what[1] : "",
         nw > 2 ? (nw > 3 ? ", " : " and ") : "", nw > 2 ? what[2] : "",
         nw > 3 ? " and " : "", nw > 3 ? what[3] : "");
    return 0;
}
