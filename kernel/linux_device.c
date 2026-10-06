/*
 * linux_device.c — struct device for Linux drivers: names, and devres —
 * the resources a driver took with devm_* (memory, IRQs, mappings), freed
 * newest first when the driver lets go of the device (probe failed, or it
 * was removed). A driver that uses devm_* needs no cleanup code of its own.
 */
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <stdarg.h>

struct devres {
    struct devres *next;
    void (*action)(void *);
    void *data;
};

int dev_set_name(struct device *dev, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dev->name, sizeof dev->name, fmt, ap);
    va_end(ap);
    return 0;
}

int devm_add_action(struct device *dev, void (*action)(void *), void *data)
{
    struct devres *r = kmalloc(sizeof *r, GFP_KERNEL);
    if (!r)
        return -ENOMEM;
    r->action = action;
    r->data = data;
    r->next = dev->devres;
    dev->devres = r;
    return 0;
}

int devm_add_action_or_reset(struct device *dev, void (*action)(void *), void *data)
{
    int err = devm_add_action(dev, action, data);
    if (err)
        action(data);
    return err;
}

static void free_it(void *p)
{
    kfree(p);
}

void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp)
{
    void *p = kmalloc(size, gfp);
    if (p && devm_add_action(dev, free_it, p)) {
        kfree(p);
        p = NULL;
    }
    return p;
}

/* Free it now, and forget the record. */
void devm_kfree(struct device *dev, const void *p)
{
    for (struct devres **pp = &dev->devres; *pp; pp = &(*pp)->next)
        if ((*pp)->action == free_it && (*pp)->data == p) {
            struct devres *r = *pp;
            *pp = r->next;
            kfree(r);
            break;
        }
    kfree(p);
}

void devres_release_all(struct device *dev)
{
    while (dev->devres) {
        struct devres *r = dev->devres;
        dev->devres = r->next;
        r->action(r->data);
        kfree(r);
    }
}

int dev_err_probe(const struct device *dev, int err, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vscnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (err != -517)                    /* -EPROBE_DEFER: not an error yet */
        printk(KERN_ERR "%s: error %d: %s", dev_name(dev), err, buf);
    return err;
}

/* A string that lives as long as the driver has the device. */
char *devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *p = devm_kmalloc(dev, (size_t)n + 1, gfp);
    if (p) {
        va_start(ap, fmt);
        vsnprintf(p, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    return p;
}

/* --- firmware, later (request_firmware_nowait) ------------------------------------ */

/* A driver's probe asks for its firmware and returns; the file is read
   and handed over on a work thread (iwlwifi then tries older versions
   from the callback, one request at a time). */
struct fw_later {
    struct work_struct work;
    struct device *dev;
    void *context;
    void (*cont)(const struct firmware *fw, void *context);
    char name[];
};

static void fw_later_run(struct work_struct *w)
{
    struct fw_later *l = container_of(w, struct fw_later, work);
    const struct firmware *fw;
    if (request_firmware(&fw, l->name, l->dev))
        fw = NULL;
    l->cont(fw, l->context);
    kfree(l);
}

int request_firmware_nowait(struct module *module, bool uevent, const char *name, struct device *dev,
                            unsigned int gfp, void *context,
                            void (*cont)(const struct firmware *fw, void *context))
{
    (void)module; (void)uevent; (void)gfp;
    size_t n = strlen(name);
    struct fw_later *l = kmalloc(sizeof(*l) + n + 1, GFP_KERNEL);
    if (!l)
        return -ENOMEM;
    memcpy(l->name, name, n + 1);
    l->dev = dev;
    l->context = context;
    l->cont = cont;
    INIT_WORK(&l->work, fw_later_run);
    schedule_work(&l->work);
    return 0;
}
