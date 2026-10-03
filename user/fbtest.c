/*
 * fbtest.c — step 11b tests: the framebuffer device, /dev/fb0
 *
 * What a program like the X server's fbdev driver does, checked step by step:
 *
 *   1. open /dev/fb0, read the variable and fixed screen info (Linux's
 *      structs, Linux's ioctl numbers): 1920x1200, 32 bits, XRGB
 *   2. take the screen from the console (KDSETMODE KD_GRAPHICS)
 *   3. mmap the pixels, draw a pattern, read it back; a fork'd child's
 *      writes show up in the parent (device memory is shared, not COW)
 *   4. pan, blank and unblank; bad pans refused
 *   5. switch to 1024x768 (FBIOPUT_VSCREENINFO): the line length and the
 *      console's size follow; impossible modes refused
 *   6. a read-only open can't map or mprotect the pixels writable
 *   7. back to 1920x1200 and to the console (KD_TEXT)
 *
 * Exits 0 if all is well, else the number of the first failed check.
 */
#include <mini.h>

static int checks;

#define CHECK(cond)                                         \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            put("fbtest: check ");                          \
            put_u64(checks);                                \
            put(" failed: " #cond "\n");                    \
            return checks;                                  \
        }                                                   \
    } while (0)

/* Linux's structs (include/uapi/linux/fb.h), as a program has them. */
struct fb_bitfield { uint32_t offset, length, msb_right; };
struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};
struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities, reserved[2];
};

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611
#define KDSETMODE   0x4B3A
#define KDGETMODE   0x4B3B
#define KD_TEXT     0
#define KD_GRAPHICS 1
#define MAP_SHARED  0x01
#define SYS_mprotect 125
#define EINVAL 22
#define EACCES 13

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct fb_var_screeninfo var, boot;
    struct fb_fix_screeninfo fix;
    unsigned short ws[4];
    int mode;

    CHECK(sizeof var == 160 && sizeof fix == 68);

    /* 1. What the screen is. */
    int fd = open("/dev/fb0", O_RDWR);
    CHECK(fd >= 0);
    CHECK(ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0);
    CHECK(var.xres == 1920 && var.yres == 1200 && var.bits_per_pixel == 32);
    CHECK(var.xres_virtual == 1920 && var.yres_virtual >= 1200);
    CHECK(var.red.offset == 16 && var.green.offset == 8 && var.blue.offset == 0);
    CHECK(var.pixclock > 0 && var.hsync_len > 0);
    CHECK(ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0);
    CHECK(memcmp(fix.id, "bochsdrmfb", 11) == 0);
    CHECK(fix.line_length == 1920 * 4 && fix.smem_len >= 1920 * 4 * var.yres_virtual);
    CHECK(fix.visual == 2 && fix.ypanstep == 1);
    boot = var;

    /* 2. The screen is ours. */
    CHECK(ioctl(1, KDSETMODE, (void *)KD_GRAPHICS) == 0);
    CHECK(ioctl(1, KDGETMODE, &mode) == 0 && mode == KD_GRAPHICS);

    /* 3. Draw. */
    uint32_t *px = mmap(0, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK((long)px > 0 && (unsigned long)px < 0xC0000000u);
    uint32_t pitch = fix.line_length / 4;
    for (uint32_t y = 0; y < 1200; y += 4)                  /* color bars */
        for (uint32_t x = 0; x < 1920; x++)
            px[y * pitch + x] = (x / 240) * 0x00202020u | 0x000000FFu;
    CHECK(px[0] == 0xFF && px[1919] == (7 * 0x00202020u | 0xFF));
    CHECK(px[400 * pitch + 960] == (4 * 0x00202020u | 0xFF));
    uint32_t last = fix.smem_len / 4 - 1;
    px[last] = 0x12345678;                                  /* the very end */
    CHECK(px[last] == 0x12345678);
    int pid = fork();
    if (pid == 0) {
        px[8] = 0xCAFE;                                     /* the child draws */
        _exit(0);
    }
    int st;
    CHECK(pid > 0 && wait4(pid, &st, 0, 0) == pid && st == 0);
    CHECK(px[8] == 0xCAFE);                                 /* shared, not copied */

    /* 4. Pan, blank. */
    var.yoffset = 100;
    CHECK(ioctl(fd, FBIOPAN_DISPLAY, &var) == 0);
    CHECK(ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 && var.yoffset == 100);
    var.yoffset = var.yres_virtual - var.yres + 1;          /* one line too far */
    CHECK(ioctl(fd, FBIOPAN_DISPLAY, &var) == -EINVAL);
    var.yoffset = 0;
    CHECK(ioctl(fd, FBIOPAN_DISPLAY, &var) == 0);
    CHECK(ioctl(fd, FBIOBLANK, (void *)1) == 0 && ioctl(fd, FBIOBLANK, (void *)0) == 0);

    /* 5. Another mode. */
    var = boot;
    var.xres = var.xres_virtual = 1024;
    var.yres = var.yres_virtual = 768;
    var.yoffset = 0;                    /* the console had it panned */
    CHECK(ioctl(fd, FBIOPUT_VSCREENINFO, &var) == 0);
    CHECK(var.xres == 1024 && var.yres == 768 && var.yres_virtual == 768);
    CHECK(ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0 && fix.line_length == 1024 * 4);
    CHECK(ioctl(1, 0x5413, ws) == 0 && ws[0] == 48 && ws[1] == 128);    /* 8x16 cells */
    px[767 * 1024 + 1023] = 0x00FF00;                       /* bottom-right pixel */
    CHECK(px[767 * 1024 + 1023] == 0x00FF00);
    var.xres = 1000;                                        /* not a standard mode */
    CHECK(ioctl(fd, FBIOPUT_VSCREENINFO, &var) == -EINVAL);
    var.xres = 1024;
    var.bits_per_pixel = 16;
    CHECK(ioctl(fd, FBIOPUT_VSCREENINFO, &var) == -EINVAL);
    var.bits_per_pixel = 32;
    var.yres_virtual = 100000;                              /* more than VRAM */
    CHECK(ioctl(fd, FBIOPUT_VSCREENINFO, &var) == -EINVAL);

    /* 6. Read-only. */
    int ro = open("/dev/fb0", O_RDONLY);
    CHECK(ro >= 0);
    CHECK((long)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0) == -EACCES);
    uint32_t *rp = mmap(0, 4096, PROT_READ, MAP_SHARED, ro, 0);
    CHECK((long)rp > 0 && rp[0] == px[0]);
    CHECK(syscall3(SYS_mprotect, (long)rp, 4096, PROT_READ | PROT_WRITE) == -EACCES);
    CHECK((long)mmap(0, 8192, PROT_READ, MAP_SHARED, ro, fix.smem_len - 4096) == -EINVAL);
    CHECK(munmap(rp, 4096) == 0 && close(ro) == 0);

    /* 7. Back to how it was. */
    boot.yoffset = 0;
    CHECK(ioctl(fd, FBIOPUT_VSCREENINFO, &boot) == 0 && boot.xres == 1920);
    CHECK(ioctl(1, 0x5413, ws) == 0 && ws[0] == 37 && ws[1] == 120);
    CHECK(munmap(px, fix.smem_len) == 0 && close(fd) == 0);
    CHECK(ioctl(1, KDSETMODE, (void *)KD_TEXT) == 0);
    CHECK(ioctl(1, KDGETMODE, &mode) == 0 && mode == KD_TEXT);

    put("fbtest: all ");
    put_u64(checks);
    put(" checks passed\n");
    return 0;
}
