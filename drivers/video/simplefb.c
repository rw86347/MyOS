/*
 * simplefb.c — the firmware framebuffer UEFI left us (GOP)
 *
 * On a BIOS boot the Bochs driver owns the screen. On a UEFI-only PC
 * there is no Bochs card: the firmware already set a linear framebuffer.
 * The loader writes its address into bootinfo; we publish that as
 * /dev/fb0 and hand it to fbcon. Mode changes are not offered — the
 * firmware picked the mode, and we have no driver for the real GPU yet.
 */
#include <bootinfo.h>
#include <errno.h>
#include <kprintf.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <memlayout.h>
#include <string.h>

static struct fb_info info;

static int no_check(struct fb_var_screeninfo *var, struct fb_info *fb)
{
    (void)var;
    (void)fb;
    return -EINVAL;             /* the firmware's mode is the only mode */
}

static int no_set(struct fb_info *fb)
{
    (void)fb;
    return -EINVAL;
}

static const struct fb_ops ops = {
    .fb_check_var = no_check,
    .fb_set_par = no_set,
};

void simplefb_init(void)
{
    const struct bootinfo *bi = P2V(BOOTINFO_PHYS);
    uint32_t bytes;
    volatile void *va;

    if (bi->fb_magic != BOOTINFO_FB_MAGIC || !bootinfo_fb_phys(bi) || bi->fb_bpp != 32)
        return;
    bytes = bi->fb_pitch * bi->fb_height;
    if (!bytes)
        return;
    va = ioremap_wc64(bootinfo_fb_phys(bi), bytes);
    if (!va) {
        kprintf("simplefb: cannot map %lu bytes at %08lx%08lx\n",
                (unsigned long)bytes,
                (unsigned long)bi->fb_phys_hi, (unsigned long)bi->fb_phys);
        return;
    }
    memset(&info, 0, sizeof info);
    info.var.xres = info.var.xres_virtual = bi->fb_width;
    info.var.yres = info.var.yres_virtual = bi->fb_height;
    info.var.bits_per_pixel = 32;
    if (bi->fb_format == 1) {                   /* RGBX: R in the low byte */
        info.var.red = (struct fb_bitfield){ 0, 8, 0 };
        info.var.green = (struct fb_bitfield){ 8, 8, 0 };
        info.var.blue = (struct fb_bitfield){ 16, 8, 0 };
    } else {
        info.var.red = (struct fb_bitfield){ 16, 8, 0 };
        info.var.green = (struct fb_bitfield){ 8, 8, 0 };
        info.var.blue = (struct fb_bitfield){ 0, 8, 0 };
    }
    memcpy(info.fix.id, "simplefb", 9);
    info.fix.smem_start = (unsigned long)bi->fb_phys;
    info.fix.smem_len = bytes;
    info.fix.type = FB_TYPE_PACKED_PIXELS;
    info.fix.visual = FB_VISUAL_TRUECOLOR;
    info.fix.line_length = bi->fb_pitch;
    info.fbops = &ops;
    info.screen_base = va;
    info.screen_size = bytes;
    if (register_framebuffer(&info) == 0)
        kprintf("simplefb: %lux%lu at %08lx%08lx\n",
                (unsigned long)bi->fb_width, (unsigned long)bi->fb_height,
                (unsigned long)bi->fb_phys_hi, (unsigned long)bi->fb_phys);
}
