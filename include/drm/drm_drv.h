/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef DRM_DRV_H
#define DRM_DRV_H

#include <drm/drm_device.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A display driver, as the DRM core sees it (Linux: include/drm/drm_drv.h).
 *
 * The shape every Linux DRM driver's probe has, which ours keep:
 *
 *   struct my_card *card = devm_drm_dev_alloc(pdev, &my_driver, struct my_card, dev);
 *   ... find the registers and video memory, drmm_vram_helper_init ...
 *   drmm_mode_config_init(&card->dev);
 *   drm_connector_init(...); drm_simple_display_pipe_init(...);
 *   drm_dev_register(&card->dev, 0);
 *   drm_fbdev_generic_setup(&card->dev, 32);     -> /dev/fb0 and the console
 */

#define DRIVER_GEM      0x1
#define DRIVER_MODESET  0x2
#define DRIVER_ATOMIC   0x10000

struct drm_driver {
    uint32_t driver_features;           /* DRIVER_* */
    const char *name;                   /* "bochs" */
    const char *desc;
    const char *date;
    int major, minor;
};

struct pci_dev;

/* Allocate the driver's struct (zeroed) with a struct drm_device at
   `member` inside it, set up for driver. NULL if out of memory. In Linux the
   first argument is the parent struct device; ours is the PCI device. */
void *__devm_drm_dev_alloc(struct pci_dev *parent, const struct drm_driver *driver,
                           size_t size, size_t offset);
#define devm_drm_dev_alloc(parent, driver, type, member) \
    ((type *)__devm_drm_dev_alloc(parent, driver, sizeof(type), offsetof(type, member)))

/* The device is ready: modes can be set. flags is unused (as in Linux). */
int drm_dev_register(struct drm_device *dev, unsigned long flags);

/* MyOS: look for display cards and start their drivers (after pci_init).
   Linux does this with module_init and PCI id tables. */
void drm_init(void);

/* MyOS: /dev/dri/card0's file operations (drm_ioctl.c). */
struct file_ops;
const struct file_ops *drm_file_ops(void);
/* Pages its buffer pool keeps (it never gives them back). */
uint32_t drm_pool_pages(void);
/* 1 if /dev/dri/card0 exists; *pdev: its PCI card (NULL: the UEFI screen). */
int drm_card0(struct pci_dev **pdev);

#endif
