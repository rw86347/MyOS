#ifndef LINUX_FB_H
#define LINUX_FB_H

#include <stdint.h>

/*
 * The framebuffer device, /dev/fb0 (Linux: include/uapi/linux/fb.h for the
 * part programs see, include/linux/fb.h for the kernel's).
 *
 * The oldest Linux graphics interface and the simplest: open /dev/fb0, ask
 * the screen's size and layout with two ioctls, mmap the pixels, draw.
 * The X server's fbdev driver, SDL, DirectFB and many small programs use
 * it. Our numbers and struct layouts are Linux's, byte for byte (checked
 * below), so those programs run unchanged.
 */

/* --- what programs see (uapi) ---------------------------------------------- */

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611

#define FB_TYPE_PACKED_PIXELS  0
#define FB_VISUAL_TRUECOLOR    2
#define FB_ACCEL_NONE          0

#define FB_ACTIVATE_NOW        0        /* set it now */
#define FB_ACTIVATE_TEST       2        /* only check it */
#define FB_ACTIVATE_MASK       15

#define FB_VMODE_NONINTERLACED 0

#define FB_BLANK_UNBLANK       0
#define FB_BLANK_NORMAL        1
#define FB_BLANK_POWERDOWN     4

/* Where one color sits in a pixel: bits [offset, offset + length). */
struct fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

/* "Variable" screen info: what a program may change — the resolution, the
   virtual (memory) size, the panning offset, the pixel format. */
struct fb_var_screeninfo {
    uint32_t xres, yres;                /* visible */
    uint32_t xres_virtual, yres_virtual;/* in memory */
    uint32_t xoffset, yoffset;          /* visible part's top-left in memory */
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd;
    uint32_t activate;                  /* FB_ACTIVATE_* */
    uint32_t height, width;             /* picture size in mm (0: unknown... ~0 in Linux) */
    uint32_t accel_flags;
    /* Timings, from the display mode. pixclock is in picoseconds. */
    uint32_t pixclock;
    uint32_t left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

/* "Fixed" screen info: what the hardware is — where its memory is and how
   long a line is. */
struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;           /* physical address of the pixels */
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;               /* bytes per line */
    unsigned long mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

#ifdef __i386__
_Static_assert(sizeof(struct fb_var_screeninfo) == 160, "Linux i386 layout");
_Static_assert(sizeof(struct fb_fix_screeninfo) == 68, "Linux i386 layout");
#endif

/* --- the kernel side -------------------------------------------------------- */

#define FB_MAX 1                        /* /dev/fb0 only, for now */

struct fb_info;
struct file_ops;

/* What a framebuffer driver does (Linux: struct fb_ops, the subset we use). */
struct fb_ops {
    /* Adjust *var to what the hardware can do; -EINVAL if impossible. */
    int (*fb_check_var)(struct fb_var_screeninfo *var, struct fb_info *info);
    /* Switch to info->var (already checked); update info->fix. */
    int (*fb_set_par)(struct fb_info *info);
    /* Show the memory from var->xoffset, var->yoffset on. O(1). */
    int (*fb_pan_display)(struct fb_var_screeninfo *var, struct fb_info *info);
    int (*fb_blank)(int blank, struct fb_info *info);
};

struct fb_info {
    int node;                           /* the 0 in fb0 */
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    const struct fb_ops *fbops;
    volatile uint8_t *screen_base;      /* kernel address of the pixels */
    uint32_t screen_size;
    void *par;                          /* the driver's own data */
};

/* Make info /dev/fb<node> and give it to the console. 0 or -errno. */
int register_framebuffer(struct fb_info *info);

/* The registered framebuffer, or NULL. */
struct fb_info *fb_get(int node);

/* Show info's memory from var->xoffset, var->yoffset on (checked against
   the virtual size); info->var keeps the new offsets. 0 or -EINVAL. */
int fb_pan_display(struct fb_info *info, struct fb_var_screeninfo *var);

/* Change the mode to *var (FBIOPUT_VSCREENINFO). *var gets what was set. */
int fb_set_var(struct fb_info *info, struct fb_var_screeninfo *var);

/* /dev/fb0's file operations (fs/dev.c attaches them; NULL until a
   framebuffer is registered, so opening it fails). */
const struct file_ops *fb_file_ops(void);

/* If /dev/fb0 is a RAM shadow of a GOP above 4 GiB, copy it to the
   hardware. No-op on BIOS/Bochs. Not for interrupt handlers (8 MiB). */
void fb_shadow_flush(void);

/* First mmap of that shadow: switch fbcon onto it and start the blit. */
void fb_shadow_on_mmap(void);

/* fbcon (drivers/video/fbcon.c): take over the console on info; and
   info's mode changed (re-measure, redraw). */
void fbcon_attach(struct fb_info *info);
void fbcon_mode_changed(struct fb_info *info);

#endif
