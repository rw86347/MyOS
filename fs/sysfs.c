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
 */
#include <errno.h>
#include <linux/fb.h>
#include <string.h>
#include <vfs.h>

static const char fb0_target[] = "../../devices/platform/framebuffer.0/graphics/fb0";

/* dir/a/b/c...: a chain of new directories, or NULL. */
static struct inode *mkdirs(struct inode *dir, const char *const *names)
{
    for (; dir && *names; names++)
        dir = vfs_add(dir, *names, strlen(*names), S_IFDIR | 0555);
    return dir;
}

int sysfs_mount(const char *path)
{
    int err = vfs_mount_ram(path, 0555);
    if (err)
        return err;
    struct inode *sys;
    err = vfs_lookup(path, &sys);
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
