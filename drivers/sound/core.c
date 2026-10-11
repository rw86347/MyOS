/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * core.c — sound cards and their mixer: /dev/snd/controlC<n>.
 *
 * A card is what a driver registers: a name, a PCM device (pcm.c), and its
 * controls — the mixer's volumes and switches, and read-only "jack"
 * switches that say whether headphones are in. Programs reach all of it
 * through /dev/snd (major 116, Linux's minors: card * 32 for the control
 * device, + 16 for playback, + 24 for capture).
 *
 * The control device answers Linux's ioctls (asound.h) and, to a program
 * that subscribed, read() gives events: "this control changed". That is
 * how a sound server hears headphones go in or a volume key change the
 * Master volume. Each open control device has its own small event queue;
 * a change already queued for the same control is merged, not repeated,
 * so the queue cannot overflow with one busy control.
 */
#include <errno.h>
#include <interrupts.h>
#include <kheap.h>
#include <kprintf.h>
#include <sound/core.h>
#include <string.h>
#include <uaccess.h>
#include <vfs.h>

#define SND_MAJOR   116
#define POLLIN      0x001
#define POLLRDNORM  0x040
#define CTL_QUEUE   16

static struct snd_card *cards[SNDRV_CARDS];

struct ctl_file {
    struct snd_card *card;
    struct ctl_file *next;
    int subscribed;
    unsigned int head, count;
    struct snd_ctl_event q[CTL_QUEUE];
};

static struct wait_queue ctl_wait;      /* readers of every control device */

struct snd_card *snd_card_get(int number)
{
    return number >= 0 && number < SNDRV_CARDS ? cards[number] : NULL;
}

int snd_card_new(struct snd_card **out)
{
    int n = 0;
    while (n < SNDRV_CARDS && cards[n])
        n++;
    if (n == SNDRV_CARDS)
        return -ENODEV;
    struct snd_card *c = kmalloc(sizeof *c);
    if (!c)
        return -ENOMEM;
    memset(c, 0, sizeof *c);
    c->number = n;
    cards[n] = c;
    *out = c;
    return 0;
}

/* /dev/snd/<name>, in the /dev made at boot (main.c mounts it over the
   disk's /dev later), mode 0660 group audio (gid 18), as Alpine has it. */
static void make_node(struct inode *dir, const char *name, unsigned int minor)
{
    uint32_t len = strlen(name);
    struct inode *i = vfs_child(dir, name, len);
    if (!i)
        i = vfs_add(dir, name, len, S_IFCHR | 0660);
    if (!i)
        return;
    i->mode = S_IFCHR | 0660;
    i->gid = 18;
    i->rdev = (SND_MAJOR << 8) | minor;
    dev_attach(i);
}

int snd_card_register(struct snd_card *card)
{
    struct inode *dev = vfs_child(vfs_root(), "dev", 3);
    struct inode *snd = dev ? vfs_child(dev, "snd", 3) : NULL;
    if (dev && !snd)
        snd = vfs_add(dev, "snd", 3, S_IFDIR | 0755);
    if (!snd)
        return -ENOENT;
    unsigned int base = card->number * 32;
    char (*name)[12] = card->node_names;
    memcpy(name[0], "controlC0", 10);
    memcpy(name[1], "pcmC0D0p", 9);
    memcpy(name[2], "pcmC0D0c", 9);
    name[0][8] = name[1][4] = name[2][4] = (char)('0' + card->number);
    make_node(snd, name[0], base);
    if (card->pcm && card->pcm->has[SNDRV_PCM_STREAM_PLAYBACK])
        make_node(snd, name[1], base + 16);
    if (card->pcm && card->pcm->has[SNDRV_PCM_STREAM_CAPTURE])
        make_node(snd, name[2], base + 24);
    klog("sound: card %d: %s, %d controls\n", card->number, card->longname,
            card->ncontrols);
    return 0;
}

/* --- controls ------------------------------------------------------------------ */

struct snd_kcontrol *snd_ctl_new1(const struct snd_kcontrol_new *n, void *private_data)
{
    struct snd_kcontrol *k = kmalloc(sizeof *k);
    if (!k)
        return NULL;
    memset(k, 0, sizeof *k);
    k->ops = n;
    k->id.iface = n->iface;
    k->id.index = n->index;
    strlcpy((char *)k->id.name, n->name, sizeof k->id.name);
    k->access = n->access ? n->access
                          : SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_WRITE;
    k->private_value = n->private_value;
    k->private_data = private_data;
    return k;
}

int snd_ctl_add(struct snd_card *card, struct snd_kcontrol *k)
{
    if (!k)
        return -ENOMEM;
    if (card->ncontrols == SND_CTL_MAX)
        return -ENOSPC;
    card->controls[card->ncontrols++] = k;
    k->id.numid = (unsigned int)card->ncontrols;   /* numid n is controls[n - 1] */
    return 0;
}

void snd_ctl_notify(struct snd_card *card, unsigned int mask, struct snd_ctl_elem_id *id)
{
    unsigned long flags = irq_save();
    for (struct ctl_file *f = card->listeners; f; f = f->next) {
        if (!f->subscribed)
            continue;
        unsigned int i;
        for (i = 0; i < f->count; i++) {
            struct snd_ctl_event *e = &f->q[(f->head + i) % CTL_QUEUE];
            if (e->data.elem.id.numid == id->numid) {
                e->data.elem.mask |= mask;  /* already waiting: merge */
                break;
            }
        }
        if (i < f->count || f->count == CTL_QUEUE)
            continue;
        struct snd_ctl_event *e = &f->q[(f->head + f->count++) % CTL_QUEUE];
        memset(e, 0, sizeof *e);
        e->type = SNDRV_CTL_EVENT_ELEM;
        e->data.elem.mask = mask;
        e->data.elem.id = *id;
    }
    wq_wake_all(&ctl_wait);
    wq_wake_all(&poll_wait);
    irq_restore(flags);
}

/* By numid, or else by interface, name and index (Linux's lookup). */
static struct snd_kcontrol *find(struct snd_card *card, const struct snd_ctl_elem_id *id)
{
    if (id->numid)
        return id->numid <= (unsigned int)card->ncontrols ? card->controls[id->numid - 1]
                                                           : NULL;
    for (int i = 0; i < card->ncontrols; i++) {
        struct snd_kcontrol *k = card->controls[i];
        if (k->id.iface == id->iface && k->id.index == id->index &&
            !strncmp((const char *)k->id.name, (const char *)id->name, sizeof id->name))
            return k;
    }
    return NULL;
}

static void fill_card_info(struct snd_card *c, struct snd_ctl_card_info *i)
{
    memset(i, 0, sizeof *i);
    i->card = c->number;
    strlcpy((char *)i->id, c->id, sizeof i->id);
    strlcpy((char *)i->driver, c->driver, sizeof i->driver);
    strlcpy((char *)i->name, c->shortname, sizeof i->name);
    strlcpy((char *)i->longname, c->longname, sizeof i->longname);
    strlcpy((char *)i->mixername, c->mixername, sizeof i->mixername);
    strlcpy((char *)i->components, c->components, sizeof i->components);
}

static int put_int(uintptr_t arg, int v)
{
    return copy_to_user((void *)arg, &v, sizeof v);
}

static int32_t ctl_ioctl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    struct ctl_file *cf = f->priv;
    struct snd_card *card = cf->card;
    int err;

    switch (cmd) {
    case SNDRV_CTL_IOCTL_PVERSION:
        return put_int(arg, SNDRV_CTL_VERSION);
    case SNDRV_CTL_IOCTL_CARD_INFO: {
        struct snd_ctl_card_info info;
        fill_card_info(card, &info);
        return copy_to_user((void *)arg, &info, sizeof info);
    }
    case SNDRV_CTL_IOCTL_ELEM_LIST: {
        struct snd_ctl_elem_list l;
        if (copy_from_user(&l, (void *)arg, sizeof l) < 0)
            return -EFAULT;
        l.count = (unsigned int)card->ncontrols;
        l.used = 0;
        for (unsigned int i = l.offset; i < l.count && l.used < l.space; i++, l.used++)
            if (copy_to_user(l.pids + l.used, &card->controls[i]->id,
                             sizeof(struct snd_ctl_elem_id)) < 0)
                return -EFAULT;
        return copy_to_user((void *)arg, &l, sizeof l);
    }
    case SNDRV_CTL_IOCTL_ELEM_INFO: {
        struct snd_ctl_elem_info info;
        if (copy_from_user(&info, (void *)arg, sizeof info) < 0)
            return -EFAULT;
        struct snd_kcontrol *k = find(card, &info.id);
        if (!k)
            return -ENOENT;
        memset(&info, 0, sizeof info);
        info.id = k->id;
        if ((err = k->ops->info(k, &info)) < 0)
            return err;
        info.access = k->access | (k->tlv ? SNDRV_CTL_ELEM_ACCESS_TLV_READ : 0);
        info.owner = -1;
        return copy_to_user((void *)arg, &info, sizeof info);
    }
    case SNDRV_CTL_IOCTL_ELEM_READ:
    case SNDRV_CTL_IOCTL_ELEM_WRITE: {
        /* 712 bytes: from the heap, not the kernel stack */
        struct snd_ctl_elem_value *v = kmalloc(sizeof *v);
        if (!v)
            return -ENOMEM;
        err = copy_from_user(v, (void *)arg, sizeof *v) < 0 ? -EFAULT : 0;
        struct snd_kcontrol *k = err ? NULL : find(card, &v->id);
        if (!err && !k)
            err = -ENOENT;
        if (!err && cmd == SNDRV_CTL_IOCTL_ELEM_READ) {
            v->id = k->id;
            if ((err = k->ops->get(k, v)) >= 0)
                err = copy_to_user((void *)arg, v, sizeof *v);
        } else if (!err) {
            if (!(k->access & SNDRV_CTL_ELEM_ACCESS_WRITE) || !k->ops->put)
                err = -EPERM;
            else if ((err = k->ops->put(k, v)) > 0) {
                snd_ctl_notify(card, SNDRV_CTL_EVENT_MASK_VALUE, &k->id);
                err = 0;
            }
        }
        kfree(v);
        return err;
    }
    case SNDRV_CTL_IOCTL_ELEM_LOCK:
    case SNDRV_CTL_IOCTL_ELEM_UNLOCK:
        return 0;
    case SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS: {
        int v;
        if (copy_from_user(&v, (void *)arg, sizeof v) < 0)
            return -EFAULT;
        if (v < 0)
            return put_int(arg, cf->subscribed);
        cf->subscribed = v != 0;
        return 0;
    }
    case SNDRV_CTL_IOCTL_TLV_READ: {
        struct snd_ctl_tlv t;
        if (copy_from_user(&t, (void *)arg, sizeof t) < 0)
            return -EFAULT;
        struct snd_ctl_elem_id id = { .numid = t.numid };
        struct snd_kcontrol *k = find(card, &id);
        if (!k || !k->tlv)
            return -ENXIO;
        unsigned int bytes = k->tlv[1] + 2 * sizeof(unsigned int);
        if (t.length < bytes)
            return -ENOMEM;
        return copy_to_user((uint8_t *)arg + sizeof t, k->tlv, bytes);
    }
    case SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE: {
        int dev;
        if (copy_from_user(&dev, (void *)arg, sizeof dev) < 0)
            return -EFAULT;
        return put_int(arg, dev < 0 && card->pcm ? 0 : -1);
    }
    case SNDRV_CTL_IOCTL_PCM_INFO: {
        struct snd_pcm_info info;
        if (copy_from_user(&info, (void *)arg, sizeof info) < 0)
            return -EFAULT;
        if (!card->pcm || info.device != 0 || info.subdevice != 0 ||
            (unsigned int)info.stream > 1 || !card->pcm->has[info.stream])
            return -ENXIO;
        snd_pcm_info_fill(&card->pcm->streams[info.stream], &info);
        return copy_to_user((void *)arg, &info, sizeof info);
    }
    case SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE:
    case SNDRV_CTL_IOCTL_POWER:
        return 0;
    case SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE:
    case SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE:
    case SNDRV_CTL_IOCTL_UMP_NEXT_DEVICE:
        return put_int(arg, -1);            /* none */
    case SNDRV_CTL_IOCTL_POWER_STATE:
        return put_int(arg, 0);             /* D0: on */
    }
    return -ENOTTY;
}

/* Events, whole ones only, as many as fit. */
static int32_t ctl_read(struct file *f, void *ubuf, uint32_t n)
{
    struct ctl_file *cf = f->priv;
    if (n < sizeof(struct snd_ctl_event))
        return -EINVAL;
    uint32_t done = 0;
    unsigned long flags = irq_save();
    while (!cf->count) {
        if (!cf->subscribed) {
            irq_restore(flags);
            return -EBADFD;
        }
        if (f->flags & O_NONBLOCK) {
            irq_restore(flags);
            return -EAGAIN;
        }
        wq_sleep(&ctl_wait);
        if (signal_pending()) {
            irq_restore(flags);
            return -EINTR;
        }
    }
    while (cf->count && n - done >= sizeof(struct snd_ctl_event)) {
        struct snd_ctl_event e = cf->q[cf->head];
        cf->head = (cf->head + 1) % CTL_QUEUE;
        cf->count--;
        irq_restore(flags);
        if (copy_to_user((uint8_t *)ubuf + done, &e, sizeof e) < 0)
            return done ? (int32_t)done : -EFAULT;
        done += sizeof e;
        flags = irq_save();
    }
    irq_restore(flags);
    return (int32_t)done;
}

static uint32_t ctl_poll(struct file *f)
{
    struct ctl_file *cf = f->priv;
    return cf->count ? POLLIN | POLLRDNORM : 0;
}

static int ctl_open(struct file *f)
{
    struct snd_card *card = snd_card_get((int)((f->inode->rdev & 0xff) / 32));
    if (!card)
        return -ENODEV;
    struct ctl_file *cf = kmalloc(sizeof *cf);
    if (!cf)
        return -ENOMEM;
    memset(cf, 0, sizeof *cf);
    cf->card = card;
    unsigned long flags = irq_save();
    cf->next = card->listeners;
    card->listeners = cf;
    irq_restore(flags);
    f->priv = cf;
    return 0;
}

static void ctl_release(struct file *f)
{
    struct ctl_file *cf = f->priv, **p;
    unsigned long flags = irq_save();
    for (p = &cf->card->listeners; *p; p = &(*p)->next)
        if (*p == cf) {
            *p = cf->next;
            break;
        }
    irq_restore(flags);
    kfree(cf);
}

static const struct file_ops ctl_fops = {
    .open = ctl_open, .release = ctl_release, .ioctl = ctl_ioctl,
    .read = ctl_read, .poll = ctl_poll,
};

const struct file_ops *snd_file_ops(unsigned int minor)
{
    switch (minor % 32) {
    case 0:  return &ctl_fops;
    case 16:
    case 24: return &snd_pcm_fops;
    }
    return NULL;
}
