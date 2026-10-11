/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * sysfs.c — /sys, the kernel's devices shown as a tree
 *
 * On Linux, /sys has one directory per device, and /sys/class/<kind>/
 * holds links to them grouped by kind. udev builds /dev from it. We make
 * /dev ourselves (fs/dev.c), so only what a real program checks is here.
 *
 * The first user is Xorg's fbdev driver. Before it touches /dev/fb0 it
 * reads the link /sys/class/graphics/fb0: no link, no screen. A target
 * under devices/pci means "a PCI card, use the PCI probe path", which
 * wants all of /sys/bus/pci. So fb0 is shown the way Linux shows a
 * firmware framebuffer (simplefb, vesafb): a platform device.
 *
 *   /sys/class/graphics/fb0 -> ../../devices/platform/framebuffer.0/graphics/fb0
 *
 * Built once at mount time from RAM nodes, like /proc; the link's target
 * is a constant string the node points at, not a copy.
 *
 * /sys/block: one directory per disk, and in it one per partition, laid
 * out as Linux does. The installer lists disks from it (lsblk does too):
 *
 *   /sys/block/sda/size              sectors of 512 bytes
 *   /sys/block/sda/removable         1 for a USB stick
 *   /sys/block/sda/ro                0
 *   /sys/block/sda/device/model      "QEMU HARDDISK"
 *   /sys/block/sda/uevent            DEVNAME=sda, DEVTYPE=disk
 *   /sys/block/sda/sda2/partition    2: its slot in the partition table
 *   /sys/block/sda/sda2/start, size  sectors
 *   /sys/block/sda/sda2/uevent       ... PARTN=2, PARTNAME=myos-root (GPT)
 *
 *   /sys/block/sda/dev               "8:0", Linux's numbers (blockdev_devnum)
 *   /sys/class/block/sda2 -> ../../block/sda/sda2   } every disk and
 *   /sys/dev/block/8:2    -> ../../block/sda/sda2   } partition, by name / number
 *
 * A device that comes after /sys is mounted (a USB stick, a partition
 * table read again) is added when it is registered (blockdev.c calls
 * sysfs_block_add). Each file's text is made once, with its node.
 *
 * /sys/class/net/<if>/{address,operstate,carrier,mtu,ifindex,type,flags}:
 * one directory per network link (netdevice.c calls sysfs_net_add). These
 * change while the link lives, so they are made when read, like /proc.
 *
 * And the few fixed things programs test for: /sys/firmware/efi (the
 * installer: "was this machine started by UEFI?"), cpu0, cgroup,
 * transparent_hugepage.
 */
#include <blockdev.h>
#include <errno.h>
#include <interrupts.h>
#include <kheap.h>
#include <linux/fb.h>
#include <netdevice.h>
#include <string.h>
#include <uaccess.h>
#include <vfs.h>

static const char fb0_target[] = "../../devices/platform/framebuffer.0/graphics/fb0";
static const char cpu_list[] = "0\n";      /* a CPU list: just cpu 0 */

/* dir/a/b/c...: a chain of directories (made where missing), or NULL. */
static struct inode *mkdirs(struct inode *dir, const char *const *names)
{
    for (; dir && *names; names++) {
        uint32_t len = (uint32_t)strlen(*names);
        struct inode *next = vfs_child(dir, *names, len);
        dir = next ? next : vfs_add(dir, *names, len, S_IFDIR | 0555);
    }
    return dir;
}

/* --- /sys/block ------------------------------------------------------------- */

static struct inode *block_dir;         /* /sys/block, once /sys is mounted */
static struct inode *class_block, *dev_block;   /* /sys/class/block, /sys/dev/block */

/* A read-only file holding a copy of text. */
static int text_file(struct inode *dir, const char *name, const char *text)
{
    uint32_t n = (uint32_t)strlen(text);
    char *copy = kmalloc(n ? n : 1);
    struct inode *f = copy ? vfs_add(dir, name, (uint32_t)strlen(name), S_IFREG | 0444) : NULL;
    if (!f) {
        kfree(copy);
        return -ENOMEM;
    }
    memcpy(copy, text, n);
    f->data = (const uint8_t *)copy;
    f->size = n;
    f->flags |= I_DATA_OWNED;
    return 0;
}

/* "1234\n" (the kernel has no snprintf of its own; procfs.c does likewise). */
static void num_line(char out[12], uint32_t v)
{
    char b[11];
    int i = 10;
    b[i] = 0;
    do
        b[--i] = (char)('0' + v % 10);
    while (v /= 10);
    strcpy(out, b + i);
    strcat(out, "\n");
}

static int sysfs_block_make(struct blockdev *d)
{
    char num[12], uevent[128];
    struct inode *dir = block_dir;
    if (d->parent) {                    /* a partition: inside its disk's directory */
        dir = vfs_child(block_dir, d->parent->name, (uint32_t)strlen(d->parent->name));
        if (!dir)
            return -ENOENT;
    }
    uint32_t len = (uint32_t)strlen(d->name);
    if (vfs_child(dir, d->name, len))
        return 0;                       /* already there */
    dir = vfs_add(dir, d->name, len, S_IFDIR | 0555);
    if (!dir)
        return -ENOMEM;
    num_line(num, d->sectors);
    int err = text_file(dir, "size", num) | text_file(dir, "ro", "0\n");
    /* "8:2": its numbers, and the two links to it by name and by number. */
    unsigned ma, mi;
    char devno[24], target[64];
    blockdev_devnum(d, &ma, &mi);
    num_line(devno, ma);
    devno[strlen(devno) - 1] = ':';
    num_line(num, mi);
    strcat(devno, num);
    err |= text_file(dir, "dev", devno);
    devno[strlen(devno) - 1] = 0;       /* the link's name: no newline */
    strcpy(target, "../../block/");
    if (d->parent) {
        strcat(target, d->parent->name);
        strcat(target, "/");
    }
    strcat(target, d->name);
    if (class_block && !vfs_child(class_block, d->name, len))
        err |= vfs_symlink(class_block, d->name, len, target, (uint32_t)strlen(target));
    if (dev_block && !vfs_child(dev_block, devno, (uint32_t)strlen(devno)))
        err |= vfs_symlink(dev_block, devno, (uint32_t)strlen(devno), target, (uint32_t)strlen(target));
    strcpy(uevent, "DEVNAME=");
    strcat(uevent, d->name);
    if (d->parent) {
        num_line(num, d->offset);
        err |= text_file(dir, "start", num);
        num_line(num, d->partno);
        err |= text_file(dir, "partition", num);
        strcat(uevent, "\nDEVTYPE=partition\nPARTN=");
        strcat(uevent, num);            /* ends with its newline */
        if (d->label[0]) {
            strcat(uevent, "PARTNAME=");
            strcat(uevent, d->label);
            strcat(uevent, "\n");
        }
    } else {
        err |= text_file(dir, "removable", d->removable ? "1\n" : "0\n");
        struct inode *dev = vfs_add(dir, "device", 6, S_IFDIR | 0555);
        char model[44];
        strcpy(model, d->model);
        strcat(model, "\n");
        err |= dev ? text_file(dev, "model", model) : -ENOMEM;
        strcat(uevent, "\nDEVTYPE=disk\n");
    }
    return err | text_file(dir, "uevent", uevent) ? -ENOMEM : 0;
}

int sysfs_block_add(struct blockdev *d)
{
    if (!block_dir)
        return 0;                       /* not mounted yet: the mount adds it */
    uint32_t flags = irq_save();        /* the usb thread adds disks too */
    int err = sysfs_block_make(d);
    irq_restore(flags);
    return err;
}

void sysfs_block_remove(struct blockdev *d)
{
    if (!block_dir)
        return;
    char devno[24], num[12];
    unsigned ma, mi;
    blockdev_devnum(d, &ma, &mi);
    num_line(devno, ma);
    devno[strlen(devno) - 1] = ':';
    num_line(num, mi);
    strcat(devno, num);
    devno[strlen(devno) - 1] = 0;
    uint32_t len = (uint32_t)strlen(d->name), flags = irq_save();
    struct inode *dir = d->parent ? vfs_child(block_dir, d->parent->name,
                                              (uint32_t)strlen(d->parent->name)) : block_dir;
    struct inode *gone[3] = {
        dir ? vfs_child(dir, d->name, len) : NULL,
        vfs_child(class_block, d->name, len),
        vfs_child(dev_block, devno, (uint32_t)strlen(devno)),
    };
    for (int k = 0; k < 3; k++)
        if (gone[k])
            vfs_remove(gone[k]);
    irq_restore(flags);
}

/* --- /sys/class/net ---------------------------------------------------------- */

static struct inode *class_net;         /* /sys/class/net, once mounted */

enum { NET_ADDRESS, NET_OPERSTATE, NET_CARRIER, NET_MTU, NET_IFINDEX, NET_TYPE, NET_FLAGS, NET_ATTRS };
static const char *const net_attr[NET_ATTRS] = {
    "address", "operstate", "carrier", "mtu", "ifindex", "type", "flags",
};

static void hex_byte(char *out, unsigned v)
{
    out[0] = "0123456789abcdef"[v >> 4 & 15];
    out[1] = "0123456789abcdef"[v & 15];
}

/* fs_ino = ifindex * NET_ATTRS + which: the text made now, from the device. */
static int32_t net_read(struct file *f, void *ubuf, uint32_t n)
{
    struct net_device *d = dev_get_by_index((int)(f->inode->fs_ino / NET_ATTRS));
    if (!d)
        return -ENODEV;                 /* it went away */
    char text[32] = "";
    switch (f->inode->fs_ino % NET_ATTRS) {
    case NET_ADDRESS:
        for (int k = 0; k < 6; k++) {
            hex_byte(text + k * 3, d->dev_addr[k]);
            text[k * 3 + 2] = k < 5 ? ':' : '\n';
        }
        text[18] = 0;
        break;
    case NET_OPERSTATE:
        strcpy(text, !(d->flags & IFF_UP) ? "down\n" :
                     (d->flags & IFF_LOOPBACK) ? "unknown\n" : d->carrier ? "up\n" : "down\n");
        break;
    case NET_CARRIER: strcpy(text, d->carrier || (d->flags & IFF_LOOPBACK) ? "1\n" : "0\n"); break;
    case NET_MTU:     num_line(text, d->mtu); break;
    case NET_IFINDEX: num_line(text, (uint32_t)d->ifindex); break;
    case NET_TYPE:    num_line(text, d->type); break;
    case NET_FLAGS:
        strcpy(text, "0x");
        for (int k = 3; k >= 0; k--)
            hex_byte(text + 2 + (3 - k) * 2, dev_get_flags(d) >> (k * 8) & 0xFF);
        strcpy(text + 10, "\n");
        break;
    }
    uint32_t len = (uint32_t)strlen(text);
    if (f->pos >= len)
        return 0;
    uint32_t r = n < len - f->pos ? n : len - f->pos;
    if (copy_to_user(ubuf, text + f->pos, r) < 0)
        return -EFAULT;
    f->pos += r;
    return (int32_t)r;
}

static const struct file_ops net_ops = { .read = net_read };

static int sysfs_net_make(struct net_device *d)
{
    uint32_t len = (uint32_t)strlen(d->name);
    if (!len || vfs_child(class_net, d->name, len))
        return 0;
    char *name = kmalloc(len);          /* the device can go; the name stays ours */
    struct inode *dir = name ? vfs_add(class_net, (memcpy(name, d->name, len), name), len,
                                       S_IFDIR | 0555) : NULL;
    if (!dir) {
        kfree(name);
        return -ENOMEM;
    }
    dir->flags |= I_NAME_OWNED;
    for (uint32_t k = 0; k < NET_ATTRS; k++) {
        struct inode *f = vfs_add(dir, net_attr[k], (uint32_t)strlen(net_attr[k]), S_IFREG | 0444);
        if (!f)
            return -ENOMEM;
        f->ops = &net_ops;
        f->fs_ino = (uint32_t)d->ifindex * NET_ATTRS + k;
        f->size = 4096;                 /* Linux shows a page for sysfs files */
    }
    return 0;
}

void sysfs_net_add(struct net_device *d)
{
    if (!class_net || !d)
        return;
    uint32_t flags = irq_save();
    sysfs_net_make(d);
    irq_restore(flags);
}

static const char thp_text[] = "always madvise [never]\n";

int sysfs_mount(const char *path)
{
    int err = vfs_mount_ram(path, 0555);
    if (err)
        return err;
    struct inode *sys;
    err = vfs_lookup(path, &sys);
    if (err)
        return err;
    /* One CPU: Chromium's CPU-feature library counts them here. */
    static const char *const cpudir[] = { "devices", "system", "cpu", NULL };
    struct inode *cpus = mkdirs(sys, cpudir);
    static const char *const names[] = { "possible", "present", "online" };
    for (int k = 0; k < 3; k++) {
        struct inode *f = cpus ? vfs_add(cpus, names[k], (uint32_t)strlen(names[k]),
                                         S_IFREG | 0444) : NULL;
        if (!f)
            return -ENOMEM;
        f->data = (const uint8_t *)cpu_list;
        f->size = sizeof cpu_list - 1;
    }
    /* Every disk and partition known so far (disks before their
       partitions: that is the order they were registered in). */
    static const char *const blockdir[] = { "block", NULL };
    static const char *const classblock[] = { "class", "block", NULL };
    static const char *const devblock[] = { "dev", "block", NULL };
    static const char *const devchar[] = { "dev", "char", NULL };
    class_block = mkdirs(sys, classblock);
    dev_block = mkdirs(sys, devblock);
    block_dir = mkdirs(sys, blockdir);
    if (!block_dir || !class_block || !dev_block || !mkdirs(sys, devchar))
        return -ENOMEM;
    for (int k = 0; k < blockdev_count(); k++)
        sysfs_block_add(blockdev_get(k));
    static const char *const classnet[] = { "class", "net", NULL };
    class_net = mkdirs(sys, classnet);
    if (!class_net)
        return -ENOMEM;
    for (int k = 1; k <= NETDEV_MAX; k++)
        sysfs_net_add(dev_get_by_index(k));
    static const char *const cpu0[] = { "devices", "system", "cpu", "cpu0", NULL };
    static const char *const cgroup[] = { "fs", "cgroup", NULL };
    static const char *const thp[] = { "kernel", "mm", "transparent_hugepage", NULL };
    struct inode *t = mkdirs(sys, thp);
    if (!mkdirs(sys, cpu0) || !mkdirs(sys, cgroup) || !t)
        return -ENOMEM;
    struct inode *en = vfs_add(t, "enabled", 7, S_IFREG | 0444);
    if (!en)
        return -ENOMEM;
    en->data = (const uint8_t *)thp_text;
    en->size = sizeof thp_text - 1;
#ifdef ARCH_X86_64
    /* The 64-bit kernel is only ever started by our UEFI loader. */
    static const char efi_size[] = "64\n";
    static const char *const efi[] = { "firmware", "efi", NULL };
    struct inode *e = mkdirs(sys, efi);
    struct inode *fw = e ? vfs_add(e, "fw_platform_size", 16, S_IFREG | 0444) : NULL;
    if (!fw)
        return -ENOMEM;
    fw->data = (const uint8_t *)efi_size;
    fw->size = sizeof efi_size - 1;
#endif
    static const char *const graphics[] = { "class", "graphics", NULL };
    struct inode *cls = mkdirs(sys, graphics);
    if (!cls)
        return -ENOMEM;
    if (!fb_get(0))
        return 0;                       /* no screen: an empty class */
    static const char *const fbdir[] = {
        "devices", "platform", "framebuffer.0", "graphics", "fb0", NULL
    };
    struct inode *l = mkdirs(sys, fbdir) ? vfs_add(cls, "fb0", 3, S_IFLNK | 0777) : NULL;
    if (!l)
        return -ENOMEM;
    l->data = (const uint8_t *)fb0_target;
    l->size = sizeof fb0_target - 1;
    return 0;
}
