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
#include <interrupts.h>
#include <kprintf.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <memlayout.h>
#include <pmm.h>
#include <string.h>
#include <thread.h>
#include <timer.h>

static struct fb_info info;

/*
 * How a 32-bit kernel reaches a framebuffer above 4 GiB
 * ----------------------------------------------------
 * A 32-bit *pointer* cannot hold 0x400000000. The CPU still can: PSE-36
 * puts physical bits 39:32 in the page-directory entry, so a normal
 * virtual address (here the WC window at 0xF8000000) translates to that
 * 36-bit physical bar. Kernel fbcon writes through that mapping — the
 * MSI already showed 1920×1080 that way.
 *
 * User mmap cannot. fork()/destroy treat every user PDE as a 4 KiB page
 * table, so a user PTE is 32-bit and (uint32_t)smem_start of 4 GiB is 0.
 * Programs therefore mmap a low RAM copy; after that mmap we blit it
 * onto the real bar. Until mmap, screen_base stays the GOP so the
 * kernel log is not invisible on the firmware splash.
 */
static volatile void *gop_va;
static void *shadow;
static uint32_t shadow_bytes;
static int shadow_live;             /* 1: fbcon and X share the RAM copy */

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

static struct wait_queue blit_wait;

static void shadow_sync(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags = irq_save();
        uint32_t deadline = timer_ms() + 16;
        wq_sleep_until(&blit_wait, deadline ? deadline : 1);
        irq_restore(flags);
        fb_shadow_flush();
    }
}

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
    info.screen_base = va;                      /* kernel console: the real bar */
    if (bootinfo_fb_phys(bi) > 0xFFFFFFFFull) {
        uint32_t pages = (bytes + 4095) / 4096;
        uint32_t pa = pmm_alloc_run(pages);
        if (!pa) {
            kprintf("simplefb: no RAM shadow; /dev/fb0 mmap will not reach GOP\n");
            info.fix.smem_start = 0;
        } else {
            shadow = P2V(pa);
            shadow_bytes = bytes;
            gop_va = va;
            memset(shadow, 0, bytes);           /* RAM; do not read the iGPU BAR */
            info.fix.smem_start = pa;
            kprintf("simplefb: RAM shadow at %08lx (used when X mmaps /dev/fb0)\n",
                    (unsigned long)pa);
        }
    } else {
        info.fix.smem_start = (unsigned long)bi->fb_phys;
    }
    info.fix.smem_len = bytes;
    info.fix.type = FB_TYPE_PACKED_PIXELS;
    info.fix.visual = FB_VISUAL_TRUECOLOR;
    info.fix.line_length = bi->fb_pitch;
    info.fbops = &ops;
    info.screen_size = bytes;
    if (register_framebuffer(&info) == 0)
        kprintf("simplefb: %lux%lu at %08lx%08lx\n",
                (unsigned long)bi->fb_width, (unsigned long)bi->fb_height,
                (unsigned long)bi->fb_phys_hi, (unsigned long)bi->fb_phys);
}

void fb_shadow_on_mmap(void)
{
    if (!gop_va || !shadow || shadow_live)
        return;
    info.screen_base = shadow;                  /* fbcon and X now share it */
    shadow_live = 1;
    if (!thread_create("fbblit", shadow_sync, NULL))
        kprintf("simplefb: no blit thread; X will draw only in RAM\n");
}

void fb_shadow_flush(void)
{
    if (shadow_live && gop_va && shadow)
        memcpy((void *)gop_va, shadow, shadow_bytes);
}
