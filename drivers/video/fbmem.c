/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * fbmem.c — /dev/fb0, the framebuffer device (step 11b)
 *
 * The generic part of Linux's fbdev (drivers/video/fbdev/core/fbmem.c):
 * a driver fills in a struct fb_info and calls register_framebuffer; this
 * file turns it into the device file programs use, and hands it to the
 * console (fbcon.c). Here the "driver" is the DRM core's fbdev emulation
 * (drivers/gpu/drm/drm_fbdev.c), as on a modern Linux.
 *
 * Every ioctl copies its argument in or out FIRST, then acts: a copy can
 * fault and sleep (a page of an mmap'd file), and nothing must be half
 * done while we sleep.
 */
#include <errno.h>
#include <kprintf.h>
#include <linux/fb.h>
#include <mm.h>
#include <pmm.h>
#include <string.h>
#include <uaccess.h>
#include <vfs.h>

static struct fb_info *registered_fb[FB_MAX];

struct fb_info *fb_get(int node)
{
    return node >= 0 && node < FB_MAX ? registered_fb[node] : NULL;
}

int fb_pan_display(struct fb_info *info, struct fb_var_screeninfo *var)
{
    if (var->xoffset + info->var.xres > info->var.xres_virtual ||
        var->yoffset > info->var.yres_virtual ||
        info->var.yres > info->var.yres_virtual - var->yoffset)
        return -EINVAL;
    if (var->xoffset == info->var.xoffset && var->yoffset == info->var.yoffset)
        return 0;
    if (!info->fbops->fb_pan_display)
        return -EINVAL;
    int err = info->fbops->fb_pan_display(var, info);
    if (err)
        return err;
    info->var.xoffset = var->xoffset;
    info->var.yoffset = var->yoffset;
    return 0;
}

/* Does var ask for a different mode than the current one (or just a pan)? */
static int mode_differs(const struct fb_var_screeninfo *a, const struct fb_var_screeninfo *b)
{
    return a->xres != b->xres || a->yres != b->yres ||
           a->xres_virtual != b->xres_virtual || a->yres_virtual != b->yres_virtual ||
           a->bits_per_pixel != b->bits_per_pixel;
}

int fb_set_var(struct fb_info *info, struct fb_var_screeninfo *var)
{
    uint32_t activate = var->activate;
    if (!info->fbops->fb_check_var) {       /* one fixed mode (simplefb): any */
        *var = info->var;                   /* request gets it, as on Linux; */
        return 0;                           /* X sets the current mode at start */
    }
    int err = info->fbops->fb_check_var(var, info);
    if (err)
        return err;
    if ((activate & FB_ACTIVATE_MASK) == FB_ACTIVATE_TEST)
        return 0;
    if (!mode_differs(var, &info->var))
        return fb_pan_display(info, var);   /* same mode: at most a pan */
    struct fb_var_screeninfo old = info->var;
    info->var = *var;
    err = info->fbops->fb_set_par(info);
    if (err) {
        info->var = old;
        return err;
    }
    *var = info->var;
    fbcon_mode_changed(info);               /* a new console size */
    return 0;
}

/* --- the device file ---------------------------------------------------------- */

static int32_t fb_ioctl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    (void)f;
    struct fb_info *info = registered_fb[0];
    struct fb_var_screeninfo var;
    if (!info)
        return -ENODEV;
    switch (cmd) {
    case FBIOGET_VSCREENINFO:
        var = info->var;                    /* a snapshot: the copy may sleep */
        return copy_to_user((void *)arg, &var, sizeof var);
    case FBIOGET_FSCREENINFO: {
        struct fb_fix_screeninfo fix = info->fix;
        return copy_to_user((void *)arg, &fix, sizeof fix);
    }
    case FBIOPUT_VSCREENINFO: {
        if (copy_from_user(&var, (void *)arg, sizeof var) < 0)
            return -EFAULT;
        int err = fb_set_var(info, &var);
        if (err)
            return err;
        return copy_to_user((void *)arg, &var, sizeof var);
    }
    case FBIOPAN_DISPLAY:
        if (copy_from_user(&var, (void *)arg, sizeof var) < 0)
            return -EFAULT;
        return fb_pan_display(info, &var);
    case FBIOBLANK:
        if (!info->fbops->fb_blank)
            return -EINVAL;
        return info->fbops->fb_blank((int)arg, info);
    default:
        return -EINVAL;
    }
}

/* mmap: the pixels themselves, write-combined, shared with the card. */
static int fb_mmap(struct file *f, struct vma *vma)
{
    (void)f;
    struct fb_info *info = registered_fb[0];
    if (!info || !info->fix.smem_start)
        return -ENODEV;
    fb_shadow_on_mmap();
    /* RAM shadows are write-back (same type as the kernel's direct map).
       Real video memory stays write-combining. */
    if (info->fix.smem_start >= pmm_end())
        vma->flags |= VM_WC;
    return vm_iomap_memory(vma, info->fix.smem_start, info->fix.smem_len);   /* all 64 bits */
}

static const struct file_ops fb_fops = {
    .ioctl = fb_ioctl,
    .mmap = fb_mmap,
};

const struct file_ops *fb_file_ops(void)
{
    return registered_fb[0] ? &fb_fops : NULL;
}

int register_framebuffer(struct fb_info *info)
{
    if (registered_fb[0])
        return -EBUSY;
    info->node = 0;
    registered_fb[0] = info;
    dev_make_node("fb0", (29 << 8) | 0);
    fbcon_attach(info);
    return 0;
}
