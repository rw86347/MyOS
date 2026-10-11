/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_DEVICE_H
#define LINUX_DEVICE_H

/*
 * struct device: what Linux drivers hold on to for any device — its name
 * ("0000:00:14.3"), its parent, the driver's private pointer, its DMA
 * limits, and the resources a driver took with devm_* (freed, newest
 * first, when the driver lets go of the device). Embedded in struct
 * pci_dev, as in Linux.
 */
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/printk.h>

struct device;
struct devres;

struct dev_pm_ops {
    int (*suspend)(struct device *dev);
    int (*resume)(struct device *dev);
    int (*freeze)(struct device *dev);
    int (*thaw)(struct device *dev);
    int (*poweroff)(struct device *dev);
    int (*restore)(struct device *dev);
    int (*runtime_suspend)(struct device *dev);
    int (*runtime_resume)(struct device *dev);
    int (*runtime_idle)(struct device *dev);
    int (*suspend_noirq)(struct device *dev);
    int (*resume_noirq)(struct device *dev);
};

struct device_driver {
    const char *name;
    const struct dev_pm_ops *pm;
    void (*coredump)(struct device *dev);   /* dump the device's state (debugging) */
};

/* The kind of bus a device sits on ("pci"). */
struct bus_type {
    const char *name;
};

#include <linux/kobject.h>

struct device {
    struct kobject kobj;
    char name[32];
    struct device *parent;
    struct device_driver *driver;
    const struct bus_type *bus;
    void *driver_data;
    u64 *dma_mask;
    u64 coherent_dma_mask;
    u64 own_dma_mask;               /* (where dma_mask points) */
    struct devres *devres;          /* devm_* resources, newest first */
    void (*release)(struct device *dev);
};

static inline const char *dev_name(const struct device *dev) { return dev->name; }
static inline void *dev_get_drvdata(const struct device *dev) { return dev->driver_data; }
static inline void dev_set_drvdata(struct device *dev, void *data) { dev->driver_data = data; }
static inline struct device *get_device(struct device *dev) { return dev; }
static inline void put_device(struct device *dev) { (void)dev; }
int dev_set_name(struct device *dev, const char *fmt, ...) __printf(2, 3);

/* devres: a resource that goes away with the driver. */
int  devm_add_action(struct device *dev, void (*action)(void *), void *data);
int  devm_add_action_or_reset(struct device *dev, void (*action)(void *), void *data);
void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp);
#define devm_kzalloc(d, s, gfp)       devm_kmalloc(d, s, (gfp) | 0x0100u /* __GFP_ZERO */)
#define devm_kcalloc(d, n, s, gfp)    devm_kzalloc(d, (n) * (s), gfp)
void devm_kfree(struct device *dev, const void *p);
char *devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...) __printf(3, 4);

/* Take the driver off the device, or take it off and match drivers again
   (a driver that wants a fresh start after a firmware crash). */
void device_release_driver(struct device *dev);
int device_reprobe(struct device *dev);
void devres_release_all(struct device *dev);   /* the driver let go: free them */

/* dev_err(dev, ...): printk with the device's name in front. */
#define dev_printk(level, dev, fmt, ...) printk(level "%s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_emerg(dev, fmt, ...)  dev_printk(KERN_EMERG, dev, fmt, ##__VA_ARGS__)
#define dev_crit(dev, fmt, ...)   dev_printk(KERN_CRIT, dev, fmt, ##__VA_ARGS__)
#define dev_alert(dev, fmt, ...)  dev_printk(KERN_ALERT, dev, fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...)    dev_printk(KERN_ERR, dev, fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)   dev_printk(KERN_WARNING, dev, fmt, ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...) dev_printk(KERN_NOTICE, dev, fmt, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...)   dev_printk(KERN_INFO, dev, fmt, ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...)    do { if (0) dev_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__); } while (0)
#define dev_vdbg(dev, fmt, ...)   dev_dbg(dev, fmt, ##__VA_ARGS__)
#define dev_err_once(dev, fmt, ...)  ({ static int _o; if (!_o) { _o = 1; dev_err(dev, fmt, ##__VA_ARGS__); } })
#define dev_warn_once(dev, fmt, ...) ({ static int _o; if (!_o) { _o = 1; dev_warn(dev, fmt, ##__VA_ARGS__); } })
#define dev_info_once(dev, fmt, ...) ({ static int _o; if (!_o) { _o = 1; dev_info(dev, fmt, ##__VA_ARGS__); } })
#define dev_err_ratelimited(dev, fmt, ...)  dev_err(dev, fmt, ##__VA_ARGS__)
#define dev_warn_ratelimited(dev, fmt, ...) dev_warn(dev, fmt, ##__VA_ARGS__)
#define dev_info_ratelimited(dev, fmt, ...) dev_info(dev, fmt, ##__VA_ARGS__)
#define dev_dbg_ratelimited(dev, fmt, ...)  dev_dbg(dev, fmt, ##__VA_ARGS__)
int dev_err_probe(const struct device *dev, int err, const char *fmt, ...) __printf(3, 4);

#endif
