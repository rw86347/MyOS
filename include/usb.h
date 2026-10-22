/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * usb.h — USB devices, as the class drivers (usb_storage.c, usb_hid.c)
 * see them.
 *
 * A device has numbered endpoints: one-way pipes. Endpoint 0 is the
 * control pipe every device has (questions and settings); a USB stick adds
 * a bulk-in and a bulk-out pipe for its data. An endpoint "address" is its
 * number plus 0x80 for IN (device to host), as in the descriptors.
 *
 * The descriptor layouts are the USB 2.0 specification's (chapter 9);
 * the names are Linux's (include/uapi/linux/usb/ch9.h).
 */
#ifndef USB_H
#define USB_H

#include <blockdev.h>
#include <stdint.h>

#define USB_DT_DEVICE        1
#define USB_DT_CONFIG        2
#define USB_DT_STRING        3
#define USB_DT_INTERFACE     4
#define USB_DT_ENDPOINT      5
#define USB_DT_SS_EP_COMP    0x30       /* USB 3: an endpoint's burst size */

#define USB_DIR_IN           0x80
#define USB_REQ_CLEAR_FEATURE     1
#define USB_REQ_GET_DESCRIPTOR    6
#define USB_REQ_SET_CONFIGURATION 9
#define USB_ENDPOINT_HALT    0

#define USB_ENDPOINT_XFER_BULK 2
#define USB_ENDPOINT_XFER_INT  3

#define USB_CLASS_HID          3
#define USB_CLASS_MASS_STORAGE 8
#define USB_CLASS_HUB          9

/* Speeds, as xHCI numbers them (PORTSC, the slot context). */
#define USB_SPEED_FULL   1              /* 12 Mb/s, USB 1.1 */
#define USB_SPEED_LOW    2              /* 1.5 Mb/s */
#define USB_SPEED_HIGH   3              /* 480 Mb/s, USB 2.0 */
#define USB_SPEED_SUPER  4              /* 5 Gb/s, USB 3 */

struct usb_device_descriptor {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t  iManufacturer, iProduct, iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t  bLength, bDescriptorType;
    uint16_t wTotalLength;              /* this and everything after it */
    uint8_t  bNumInterfaces, bConfigurationValue, iConfiguration;
    uint8_t  bmAttributes, bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t  bLength, bDescriptorType;
    uint8_t  bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    uint8_t  bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t  bLength, bDescriptorType;
    uint8_t  bEndpointAddress;          /* number | USB_DIR_IN */
    uint8_t  bmAttributes;              /* low 2 bits: the transfer type */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

struct usb_ss_ep_comp_descriptor {
    uint8_t  bLength, bDescriptorType;
    uint8_t  bMaxBurst;                 /* packets per burst, minus 1 */
    uint8_t  bmAttributes;
    uint16_t wBytesPerInterval;
} __attribute__((packed));

/* One endpoint a class driver wants: its descriptor and, on USB 3, the
   companion's burst size (0 otherwise). */
struct usb_ep_want {
    const struct usb_endpoint_descriptor *desc;
    uint8_t burst;
};

struct usb_device;                      /* the host controller's (xhci.c) */

/* What a class driver may read. */
const struct usb_device_descriptor *usb_desc(struct usb_device *d);
int usb_speed(struct usb_device *d);
const char *usb_name(struct usb_device *d);   /* "SanDisk Ultra Fit", or "" */
const char *usb_path(struct usb_device *d);   /* where it is: "1-2", "1-2.3" (bus-port.hub port) */

/* A control request on endpoint 0, with up to 1024 bytes of data in or
   out. The bytes moved, or -1 (an error, a stall: the request was
   refused). */
int usb_control(struct usb_device *d, uint8_t type, uint8_t req, uint16_t value,
                uint16_t index, void *data, uint16_t len);

/* Give the device these endpoints and switch to configuration `config`. */
int usb_configure(struct usb_device *d, uint8_t config, const struct usb_ep_want *eps, int n);

/*
 * Bulk transfers, split in two so that several can be on their way at
 * once (usb_storage.c queues command, data and status together):
 *   usb_queue  puts a transfer of the pieces v on endpoint `ep`. With
 *              wait = 1, usb_wait waits for this one (it ends at its last
 *              byte or at a short packet). It does not start it.
 *   usb_start  rings the endpoint's doorbell: the controller begins.
 *   usb_wait   sleeps until the transfer queued with wait = 1 on `ep` is
 *              done, or any transfer of this device failed, or `ms` pass.
 *              0; USB_STALL or another xHCI completion code if a transfer
 *              failed (that endpoint has stopped: see below); -1 if time
 *              ran out.
 * Queue only while the endpoint is idle (its last transfer is over).
 */
#define USB_STALL 6
int  usb_queue(struct usb_device *d, uint8_t ep, const struct bio_vec *v, int nv, int wait);
void usb_start(struct usb_device *d, uint8_t ep);
int  usb_wait(struct usb_device *d, uint8_t ep, uint32_t ms);

/* After a failure. usb_halted: did `ep` stop on an error (a stall)?
   usb_reset_ep forgets whatever is still queued on `ep` and makes it
   usable again (1 if it had halted). usb_clear_halt does that and tells
   the device too (CLEAR_FEATURE ENDPOINT_HALT). */
int usb_halted(struct usb_device *d, uint8_t ep);
int usb_reset_ep(struct usb_device *d, uint8_t ep);
int usb_clear_halt(struct usb_device *d, uint8_t ep);

/*
 * Interrupt-IN transfers (keyboards, mice). The controller asks the device
 * every bInterval whether it has news; it answers only when it does.
 * usb_intr_start keeps one transfer of len bytes into phys queued on IN
 * endpoint `ep` for good: each time the device answers, fn(arg, n) runs
 * in the controller's interrupt with the n bytes it sent, and the
 * transfer is queued again. An endpoint that fails (a stall, an error) is
 * cleared and started again by the usb thread. The endpoint must be one
 * of those given to usb_configure.
 */
int usb_intr_start(struct usb_device *d, uint8_t ep, uint32_t phys, uint16_t len,
                   void (*fn)(void *arg, int n), void *arg);

/* Sleep (a kernel thread, interrupts on). */
void usb_sleep(uint32_t ms);

/*
 * For the hub driver (usb_hub.c). The host controller does the hub's work
 * on the wire; it needs to know which devices are hubs, and where each
 * device behind one is.
 *   usb_hub_set     d is a hub with this many ports and this TT think time
 *                   (its descriptor's): call before usb_configure, which
 *                   tells the controller.
 *   usb_hub_attach  a device was just reset on the hub's port: address it,
 *                   find its driver (it may be a hub too). NULL if it failed.
 *   usb_unplugged   it is gone, and everything behind it.
 *   usb_set_work, usb_work: have fn(arg) run in the usb thread soon (it may
 *                   sleep and send control requests there); usb_work may be
 *                   called from the controller's interrupt.
 */
void usb_hub_set(struct usb_device *d, int ports, int ttt);
struct usb_device *usb_hub_attach(struct usb_device *hub, int port, int speed);
void usb_unplugged(struct usb_device *d);
void usb_set_work(struct usb_device *d, void (*fn)(void *arg), void *arg);
void usb_work(struct usb_device *d);

/* The class drivers: 0 if this one takes the device. */
int usb_storage_probe(struct usb_device *d, const uint8_t *config, int len);
int usb_hid_probe(struct usb_device *d, const uint8_t *config, int len);
int usb_hub_probe(struct usb_device *d, const uint8_t *config, int len);

/* Boot: start the controllers and their thread; then wait (at most ms) for
   the devices already plugged in to be found. */
void usb_init(void);
void usb_wait_boot(uint32_t ms);

#endif
