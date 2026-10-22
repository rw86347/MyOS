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
 * /sys/class/powercap/intel-rapl:0/energy_uj: the processor's energy meter
 * (cpupower.c), the joules it has used, in microjoules, as Linux shows it
 * (powerstat, s-tui, a shell loop read it). For before/after numbers of
 * the power savings (docs/performance.md). Only root may read it: its
 * fine readings are a side channel into what other programs compute
 * (PLATYPUS, CVE-2020-8694), and Linux closed it the same way.
 *
 * /sys/class/power_supply/<BAT0, AC>/<status, capacity, energy_now, online
 * ...>: the batteries and chargers (battery.c), made when the AML
 * interpreter finds them; each file is made when read, from the numbers
 * battery.c keeps (it reads the firmware every few seconds, in its own
 * thread: a read never waits for the hardware).
 *
 * /sys/class/backlight/acpi_video0/{brightness, actual_brightness,
 * max_brightness, type}: the panel's brightness through the firmware's
 * _BCM (backlight.c), 0 to max. Root's, 0644, as on Linux: the live
 * system's boot gives brightness to group video (every account), for the
 * screen dimming (myos-idle).
 *
 * /sys/firmware/acpi/platform_profile: low-power, balanced or performance
 * (cpupower.c), read and written as on Linux; platform_profile_choices
 * lists the three. Root's, 0644, as on Linux: the live system's boot
 * gives it to the administrators' group, for the battery tray's slider.
 *
 * /sys/module/hid/parameters/two_finger_scroll: Y or N, whether two
 * fingers on a touchpad scroll (hid.c). Root's, 0644; the live system's
 * boot gives it to group input, which every account is in, for Settings.
 *
 * /sys/firmware/acpi/tables/<NAME>: each ACPI table the firmware has
 * (acpi.c names them as Linux does: DSDT, FACP, SSDT1...), root only, as on
 * Linux (MSDM holds a Windows product key). Each file is the table itself,
 * in the firmware's memory, not a copy.
 *
 * And the few fixed things programs test for: /sys/firmware/efi (the
 * installer: "was this machine started by UEFI?"), cpu0, cgroup,
 * transparent_hugepage.
 */
#include <acpi.h>
#include <blockdev.h>
#include <cpu.h>
#include <drm/drm_drv.h>
#include <errno.h>
#include <evdev.h>
#include <hid.h>
#include <interrupts.h>
#include <kheap.h>
#include <linux/fb.h>
#include <netdevice.h>
#include <pci.h>
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

/* --- /sys/class/drm ----------------------------------------------------------- */

/*
 * /dev/dri/card0 shown as Linux shows a display card, for libudev (a
 * Wayland compositor finds its GPU through it) and libdrm (which checks
 * a node is DRM by its .../device/drm directory, and reads its name from
 * uevent):
 *
 *   /sys/devices/pci0000:00/0000:00:02.0/          the card: vendor, device,
 *       uevent, subsystem -> bus/pci, boot_vga    class, ...; boot_vga 1
 *   /sys/devices/.../0000:00:02.0/drm/card0/       MAJOR=226 MINOR=0
 *       uevent, dev, subsystem -> class/drm,      DEVNAME=dri/card0
 *       device -> the card
 *   /sys/class/drm/card0, /sys/dev/char/226:0      links to card0
 *
 * The UEFI screen has no PCI card to show: it is a platform device,
 * devices/platform/simple-framebuffer.0, as Linux's simpledrm is.
 */
static char drm_slot[16];               /* "0000:00:02.0": a node's name, kept */

static char *put_hex(char *s, uint32_t v, int digits)
{
    for (int k = digits - 1; k >= 0; k--)
        *s++ = "0123456789abcdef"[v >> (k * 4) & 15];
    *s = 0;
    return s;
}

/* "0x" and v in digits hex digits, and a newline. */
static int hex_file(struct inode *dir, const char *name, uint32_t v, int digits)
{
    char text[16] = "0x";
    strcat(put_hex(text + 2, v, digits), "\n");
    return text_file(dir, name, text);
}

static int sys_link(struct inode *dir, const char *name, const char *target)
{
    uint32_t len = (uint32_t)strlen(name);
    if (vfs_child(dir, name, len))
        return 0;
    return vfs_symlink(dir, name, len, target, (uint32_t)strlen(target));
}

static int sysfs_drm(struct inode *sys)
{
    struct pci_dev *p;
    if (!drm_card0(&p))
        return 0;
    const char *parent = "platform", *name = "simple-framebuffer.0";
    char uevent[160], target[96];
    if (p) {
        parent = "pci0000:00";
        strcpy(drm_slot, "0000:");
        char *s = put_hex(drm_slot + 5, p->busnr, 2);
        *s++ = ':';
        s = put_hex(s, p->slot, 2);
        *s++ = '.';
        put_hex(s, p->func, 1);
        name = drm_slot;
    }
    const char *const devdir[] = { "devices", parent, NULL };
    const char *const busdir[] = { "bus", p ? "pci" : "platform", "devices", NULL };
    static const char *const classdrm[] = { "class", "drm", NULL };
    static const char *const devchar[] = { "dev", "char", NULL };
    struct inode *top = mkdirs(sys, devdir), *bus = mkdirs(sys, busdir);
    struct inode *cls = mkdirs(sys, classdrm), *chr = mkdirs(sys, devchar);
    if (!top || !bus || !cls || !chr)
        return -ENOMEM;
    uint32_t len = (uint32_t)strlen(name);
    struct inode *card = vfs_child(top, name, len);
    if (!card)
        card = vfs_add(top, name, len, S_IFDIR | 0555);
    static const char *const drmdir[] = { "drm", "card0", NULL };
    struct inode *minor = card ? mkdirs(card, drmdir) : NULL;
    if (!minor)
        return -ENOMEM;

    int err = 0;
    if (p) {
        strcpy(uevent, "DRIVER=bochs-drm\nPCI_CLASS=");
        char *s = put_hex(uevent + strlen(uevent), p->class_rev >> 8, 6);
        strcpy(s, "\nPCI_ID=");
        s = put_hex(s + 8, p->vendor, 4);
        *s++ = ':';
        s = put_hex(s, p->device, 4);
        strcpy(s, "\nPCI_SLOT_NAME=");
        strcat(strcat(s, drm_slot), "\n");
        err |= hex_file(card, "vendor", p->vendor, 4) | hex_file(card, "device", p->device, 4) |
               hex_file(card, "subsystem_vendor", p->subsystem_vendor, 4) |
               hex_file(card, "subsystem_device", p->subsystem_device, 4) |
               hex_file(card, "class", p->class_rev >> 8, 6) |
               hex_file(card, "revision", p->class_rev & 0xFF, 2) |
               text_file(card, "boot_vga", "1\n") | sys_link(card, "subsystem", "../../../bus/pci");
    } else {
        strcpy(uevent, "DRIVER=simple-framebuffer\n");
        err |= sys_link(card, "subsystem", "../../../bus/platform");
    }
    err |= text_file(card, "uevent", uevent);
    err |= text_file(minor, "uevent", "MAJOR=226\nMINOR=0\nDEVNAME=dri/card0\nDEVTYPE=drm_minor\n") |
           text_file(minor, "dev", "226:0\n") | sys_link(minor, "subsystem", "../../../../../class/drm");
    strcpy(target, "../../../");
    err |= sys_link(minor, "device", strcat(target, name));
    strcpy(target, "../../../devices/");
    strcat(strcat(strcat(target, parent), "/"), name);
    err |= sys_link(bus, name, target);
    memmove(target, target + 3, strlen(target + 3) + 1);   /* "../../devices/..." */
    strcat(target, "/drm/card0");
    err |= sys_link(cls, "card0", target) | sys_link(chr, "226:0", target);
    return err ? -ENOMEM : 0;
}

/* --- /sys/class/input ------------------------------------------------------- */

/*
 * The evdev devices, for libudev and so libinput (a Wayland compositor's
 * keyboard and mouse):
 *
 *   /sys/devices/virtual/input/input0/uevent       NAME, EV=, KEY=, REL=...
 *   /sys/devices/virtual/input/input0/event0/uevent MAJOR=13 MINOR=64
 *                                                  DEVNAME=input/event0
 *   /sys/class/input/{input0,event0}, /sys/dev/char/13:64   links to them
 */
static int sysfs_input(struct inode *sys)
{
    static const char *const virt[] = { "devices", "virtual", "input", NULL };
    static const char *const classinput[] = { "class", "input", NULL };
    static const char *const devchar[] = { "dev", "char", NULL };
    static const char inputs[EVDEV_COUNT][8] = { "input0", "input1", "input2" };
    static const char events[EVDEV_COUNT][8] = { "event0", "event1", "event2" };
    static const char devnos[EVDEV_COUNT][8] = { "13:64", "13:65", "13:66" };
    struct inode *top = mkdirs(sys, virt), *cls = mkdirs(sys, classinput);
    struct inode *chr = mkdirs(sys, devchar);
    if (!top || !cls || !chr)
        return -ENOMEM;
    char text[EVDEV_UEVENT_MAX], target[64];
    int err = 0;
    for (int n = 0; n < EVDEV_COUNT; n++) {
        const char *const in[] = { inputs[n], NULL }, *const ev[] = { inputs[n], events[n], NULL };
        struct inode *dev = mkdirs(top, in), *node = mkdirs(top, ev);
        if (!dev || !node)
            return -ENOMEM;
        evdev_uevent(n, text);
        err |= text_file(dev, "uevent", text) | sys_link(dev, "subsystem", "../../../../class/input");
        evdev_node_uevent(n, text);
        err |= text_file(node, "uevent", text) | text_file(node, "dev", devnos[n]) |
               sys_link(node, "subsystem", "../../../../../class/input");
        strcat(strcpy(target, "../../devices/virtual/input/"), inputs[n]);
        err |= sys_link(cls, inputs[n], target);
        strcat(strcat(target, "/"), events[n]);
        err |= sys_link(cls, events[n], target) | sys_link(chr, devnos[n], target);
    }
    return err ? -ENOMEM : 0;
}

/* --- /sys/class/powercap ---------------------------------------------------- */

/* "18446744073709551615\n" at most */
static void num64_line(char out[22], uint64_t v)
{
    char b[21];
    int i = 20;
    b[i] = 0;
    do
        b[--i] = (char)('0' + v % 10);
    while (v /= 10);
    strcpy(out, b + i);
    strcat(out, "\n");
}

static int32_t energy_read(struct file *f, void *ubuf, uint32_t n)
{
    uint64_t uj, range;
    char text[22];
    if (cpu_energy_uj(&uj, &range) < 0)
        return -EIO;
    num64_line(text, uj);
    uint32_t len = (uint32_t)strlen(text);
    if (f->pos >= len)
        return 0;
    uint32_t r = n < len - f->pos ? n : len - f->pos;
    if (copy_to_user(ubuf, text + f->pos, r) < 0)
        return -EFAULT;
    f->pos += r;
    return (int32_t)r;
}

static const struct file_ops energy_ops = { .read = energy_read };

/* intel-rapl:0 (the package), if the processor has a meter. */
static int sysfs_powercap(struct inode *sys)
{
    uint64_t uj, range;
    char num[22];
    if (cpu_energy_uj(&uj, &range) < 0)
        return 0;
    static const char *const rapl[] = { "class", "powercap", "intel-rapl:0", NULL };
    struct inode *dir = mkdirs(sys, rapl);
    struct inode *e = dir ? vfs_add(dir, "energy_uj", 9, S_IFREG | 0400) : NULL;
    if (!e)
        return -ENOMEM;
    e->ops = &energy_ops;
    e->size = 4096;                     /* Linux shows a page for sysfs files */
    num64_line(num, range);
    return text_file(dir, "name", "package-0\n") | text_file(dir, "max_energy_range_uj", num);
}

/* --- /sys/class/power_supply ------------------------------------------------- */

static struct inode *class_power;

/* fs_ino = supply * power_supply_attrs() + attribute */
static int32_t power_read(struct file *f, void *ubuf, uint32_t n)
{
    int k = power_supply_attrs();
    char text[40];
    int len = power_supply_show((int)(f->inode->fs_ino / (uint32_t)k),
                                (int)(f->inode->fs_ino % (uint32_t)k), text, sizeof text);
    if (len < 0)
        return len;
    if (f->pos >= (uint32_t)len)
        return 0;
    uint32_t r = n < (uint32_t)len - f->pos ? n : (uint32_t)len - f->pos;
    if (copy_to_user(ubuf, text + f->pos, r) < 0)
        return -EFAULT;
    f->pos += r;
    return (int32_t)r;
}

static const struct file_ops power_ops = { .read = power_read };

static int sysfs_power_make(int i)
{
    const char *name = power_supply_name(i);
    uint32_t len = name ? (uint32_t)strlen(name) : 0;
    if (!name || vfs_child(class_power, name, len))
        return 0;
    struct inode *dir = vfs_add(class_power, name, len, S_IFDIR | 0555);
    if (!dir)
        return -ENOMEM;
    for (int k = 0; k < power_supply_attrs(); k++) {
        const char *a = power_supply_attr(i, k);
        struct inode *f = a ? vfs_add(dir, a, (uint32_t)strlen(a), S_IFREG | 0444) : NULL;
        if (!a)
            continue;
        if (!f)
            return -ENOMEM;
        f->ops = &power_ops;
        f->fs_ino = (uint32_t)(i * power_supply_attrs() + k);
        f->size = 4096;                 /* Linux shows a page for sysfs files */
    }
    return 0;
}

void sysfs_power_supply_add(int i)
{
    if (!class_power)
        return;                         /* sysfs_mount adds it */
    uint32_t flags = irq_save();
    sysfs_power_make(i);
    irq_restore(flags);
}

/* --- /sys/class/backlight/acpi_video0 ---------------------------------------- */

static struct inode *class_backlight;
enum { BL_BRIGHTNESS, BL_ACTUAL, BL_MAX, BL_TYPE };

static int32_t backlight_read(struct file *f, void *ubuf, uint32_t n)
{
    char text[16];
    int v = f->inode->fs_ino == BL_BRIGHTNESS ? backlight_get()
          : f->inode->fs_ino == BL_ACTUAL ? backlight_actual() : backlight_max();
    if (f->inode->fs_ino == BL_TYPE)
        strcpy(text, "firmware\n");
    else {
        char d[12];
        int k = 0, len = 0;
        unsigned u = v < 0 ? 0 : (unsigned)v;
        do
            d[k++] = (char)('0' + u % 10);
        while (u /= 10);
        while (k)
            text[len++] = d[--k];
        text[len++] = '\n';
        text[len] = 0;
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

/* A number, with or without echo's newline. */
static int32_t backlight_write(struct file *f, const void *ubuf, uint32_t n)
{
    (void)f;
    char text[12];
    if (!n || n >= sizeof text)
        return -EINVAL;
    if (copy_from_user(text, ubuf, n) < 0)
        return -EFAULT;
    uint32_t len = n, v = 0;
    if (text[len - 1] == '\n')
        len--;
    if (!len)
        return (int32_t)n;
    for (uint32_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9')
            return -EINVAL;
        v = v * 10 + (uint32_t)(text[i] - '0');
    }
    int err = v > 1000 ? -EINVAL : backlight_set((int)v);
    return err ? err : (int32_t)n;
}

static const struct file_ops backlight_ops = { .read = backlight_read, .write = backlight_write };

void sysfs_backlight_add(void)
{
    static const char *const names[] = { "brightness", "actual_brightness", "max_brightness", "type" };
    if (!class_backlight || backlight_max() < 0)
        return;                         /* sysfs_mount adds it */
    uint32_t flags = irq_save();
    struct inode *dir = vfs_child(class_backlight, "acpi_video0", 11) ? NULL
                      : vfs_add(class_backlight, "acpi_video0", 11, S_IFDIR | 0555);
    for (uint32_t k = 0; dir && k < 4; k++) {
        struct inode *i = vfs_add(dir, names[k], (uint32_t)strlen(names[k]),
                                  S_IFREG | (k == BL_BRIGHTNESS ? 0644 : 0444));
        if (!i)
            break;
        i->ops = &backlight_ops;
        i->fs_ino = k;
        i->size = 4096;
    }
    irq_restore(flags);
}

static const char *const profiles[] = { "low-power", "balanced", "performance" };
static const char profile_choices[] = "low-power balanced performance\n";

static int32_t profile_read(struct file *f, void *ubuf, uint32_t n)
{
    char text[16];
    strcpy(text, profiles[cpupower_profile()]);
    strcat(text, "\n");
    uint32_t len = (uint32_t)strlen(text);
    if (f->pos >= len)
        return 0;
    uint32_t r = n < len - f->pos ? n : len - f->pos;
    if (copy_to_user(ubuf, text + f->pos, r) < 0)
        return -EFAULT;
    f->pos += r;
    return (int32_t)r;
}

/* "balanced", with or without the newline echo adds. */
static int32_t profile_write(struct file *f, const void *ubuf, uint32_t n)
{
    (void)f;
    char text[16];
    if (n >= sizeof text)
        return -EINVAL;
    if (copy_from_user(text, ubuf, n) < 0)
        return -EFAULT;
    uint32_t len = n;
    if (len && text[len - 1] == '\n')
        len--;
    if (!len)
        return (int32_t)n;          /* the newline alone: musl's writev sends "balanced", "\n" */
    for (int p = 0; p < 3; p++)
        if (strlen(profiles[p]) == len && !memcmp(text, profiles[p], len))
            return cpupower_set_profile(p) < 0 ? -EIO : (int32_t)n;
    return -EINVAL;
}

static const struct file_ops profile_ops = { .read = profile_read, .write = profile_write };

static int sysfs_acpi(struct inode *sys)
{
    static const char *const acpi[] = { "firmware", "acpi", NULL };
    struct inode *a = acpi_rsdp() ? mkdirs(sys, acpi) : NULL;
    if (a) {
        struct inode *f = vfs_add(a, "platform_profile", 16, S_IFREG | 0644);
        struct inode *c = vfs_add(a, "platform_profile_choices", 24, S_IFREG | 0444);
        if (!f || !c)
            return -ENOMEM;
        f->ops = &profile_ops;
        f->size = 4096;
        c->data = (const uint8_t *)profile_choices;
        c->size = sizeof profile_choices - 1;
    }
    static const char *const path[] = { "firmware", "acpi", "tables", NULL };
    struct inode *dir = NULL;
    const char *name;
    uint32_t len;
    const void *t;
    for (int i = 0; (t = acpi_table_at(i, &name, &len)); i++) {
        if (!dir && !(dir = mkdirs(sys, path)))
            return -ENOMEM;
        struct inode *f = vfs_add(dir, name, (uint32_t)strlen(name), S_IFREG | 0400);
        if (!f)
            return -ENOMEM;
        f->data = t;                    /* (acpi.c keeps the name and the table) */
        f->size = len;
    }
    return 0;
}

/* A kernel switch, read and written as a Linux module's bool parameter:
   "Y" or "N" read; Y, y, 1 or N, n, 0 written, with or without echo's
   newline. */
static int32_t two_finger_read(struct file *f, void *ubuf, uint32_t n)
{
    const char *text = hid_two_finger_scroll ? "Y\n" : "N\n";
    if (f->pos >= 2)
        return 0;
    uint32_t r = n < 2 - f->pos ? n : 2 - f->pos;
    if (copy_to_user(ubuf, text + f->pos, r) < 0)
        return -EFAULT;
    f->pos += r;
    return (int32_t)r;
}

static int32_t two_finger_write(struct file *f, const void *ubuf, uint32_t n)
{
    (void)f;
    char c;
    if (!n)
        return 0;
    if (copy_from_user(&c, ubuf, 1) < 0)
        return -EFAULT;
    if (c == 'Y' || c == 'y' || c == '1')
        hid_two_finger_scroll = 1;
    else if (c == 'N' || c == 'n' || c == '0')
        hid_two_finger_scroll = 0;
    else if (c != '\n')
        return -EINVAL;
    return (int32_t)n;
}

static const struct file_ops two_finger_ops = { .read = two_finger_read, .write = two_finger_write };

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
    static const char *const classpower[] = { "class", "power_supply", NULL };
    if (!(class_power = mkdirs(sys, classpower)))
        return -ENOMEM;
    for (int k = 0; k < power_supply_count(); k++)
        sysfs_power_supply_add(k);
    static const char *const classbl[] = { "class", "backlight", NULL };
    if (!(class_backlight = mkdirs(sys, classbl)))
        return -ENOMEM;
    sysfs_backlight_add();
    static const char *const cpu0[] = { "devices", "system", "cpu", "cpu0", NULL };
    static const char *const cgroup[] = { "fs", "cgroup", NULL };
    static const char *const thp[] = { "kernel", "mm", "transparent_hugepage", NULL };
    struct inode *t = mkdirs(sys, thp);
    if (!mkdirs(sys, cpu0) || !mkdirs(sys, cgroup) || !t || sysfs_powercap(sys) || sysfs_acpi(sys))
        return -ENOMEM;
    struct inode *en = vfs_add(t, "enabled", 7, S_IFREG | 0444);
    if (!en)
        return -ENOMEM;
    en->data = (const uint8_t *)thp_text;
    en->size = sizeof thp_text - 1;
    static const char *const hidparam[] = { "module", "hid", "parameters", NULL };
    struct inode *hp = mkdirs(sys, hidparam);
    struct inode *tf = hp ? vfs_add(hp, "two_finger_scroll", 17, S_IFREG | 0644) : NULL;
    if (!tf)
        return -ENOMEM;
    tf->ops = &two_finger_ops;
    tf->size = 4096;
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
    err = sysfs_drm(sys);
    if (!err)
        err = sysfs_input(sys);
    if (err)
        return err;
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
