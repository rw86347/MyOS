/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * drmtest.c — /dev/dri/card0, the way wlroots uses it
 *
 *   1. open it; the version, the caps; atomic refused, universal planes taken
 *   2. master: the first opener is; libdrm's drmIsMaster test (AUTH_MAGIC 0)
 *      tells master from not; a second file can't take it
 *   3. the objects: one CRTC, connector, encoder and primary plane, the
 *      mode list with 1920x1200 preferred, the plane's "type" property
 *   4. two 1024x768 dumb buffers: mapped, drawn, and found in video memory
 *      (through /dev/fb0); framebuffers; SETCRTC shows the first
 *   5. a page flip with an event: a second one is refused until the event
 *      is read; poll says readable; the event carries user_data back
 *   6. PRIME: a buffer out as an fd and back is the same handle; the fd maps
 *   7. a file without the handle can't map the buffer; not master: no SETCRTC
 *   8. a 1920x1200 buffer (too big for two in 16 MiB): RAM, copied to the screen
 *   9. closing the master gives the console back, at its own size
 *  10. /sys shows the card and the input devices the way libdrm, libudev
 *      and libinput look for them
 *
 * Exits 0 if all is well, else the number of the first failed check.
 */
#include <mini.h>
#include "../include/uapi/drm.h"

static int checks;

#define CHECK(cond)                                         \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            put("drmtest: check ");                         \
            put_u64(checks);                                \
            put(" failed: " #cond "\n");                    \
            return checks;                                  \
        }                                                   \
    } while (0)

#define MAP_SHARED 0x01
#define SYS_poll   168
#define SYS_mkdir  39
#define SYS_mount  21
#define KDSETMODE  0x4B3A
#define KDGETMODE  0x4B3B
#define KD_TEXT    0

/* Linux's _IOWR('d', nr, type) etc., spelled out. */
#define IOC(dir, nr, size) (((unsigned long)(dir) << 30) | ((unsigned long)(size) << 16) | ('d' << 8) | (nr))
#define IOWR(nr, t) IOC(3, nr, sizeof(t))
#define IOW(nr, t)  IOC(1, nr, sizeof(t))
#define IOR(nr, t)  IOC(2, nr, sizeof(t))
#define IO(nr)      IOC(0, nr, 0)

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static uint64_t ptr(const void *p) { return (uint64_t)(uintptr_t)p; }

/* text has line as one of its lines. */
static int has_line(const char *text, const char *line)
{
    while (*text) {
        const char *a = text, *b = line;
        while (*b && *a == *b)
            a++, b++;
        if (!*b && (*a == '\n' || !*a))
            return 1;
        while (*text && *text++ != '\n')
            ;
    }
    return 0;
}

/* A small file's text, as a string. */
static long slurp(const char *path, char *buf, long size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    long n = read(fd, buf, (size_t)(size - 1));
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    return n;
}

static struct drm_mode_modeinfo modes[32];

int main(void)
{
    /* 1. */
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    char name[16] = { 0 };
    struct drm_version v = { 0 };
    v.name = name;
    v.name_len = sizeof name - 1;
    CHECK(ioctl(fd, IOWR(DRM_NR_VERSION, struct drm_version), &v) == 0);
    CHECK(streq(name, "bochs") && v.name_len == 5);
    struct drm_get_cap cap = { DRM_CAP_DUMB_BUFFER, 0 };
    CHECK(ioctl(fd, IOWR(DRM_NR_GET_CAP, struct drm_get_cap), &cap) == 0 && cap.value == 1);
    cap.capability = DRM_CAP_PRIME;
    CHECK(ioctl(fd, IOWR(DRM_NR_GET_CAP, struct drm_get_cap), &cap) == 0 && cap.value == 3);
    cap.capability = DRM_CAP_TIMESTAMP_MONOTONIC;
    CHECK(ioctl(fd, IOWR(DRM_NR_GET_CAP, struct drm_get_cap), &cap) == 0 && cap.value == 1);
    struct drm_set_client_cap cc = { DRM_CLIENT_CAP_ATOMIC, 1 };
    CHECK(ioctl(fd, IOW(DRM_NR_SET_CLIENT_CAP, struct drm_set_client_cap), &cc) == -95);
    cc.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    CHECK(ioctl(fd, IOW(DRM_NR_SET_CLIENT_CAP, struct drm_set_client_cap), &cc) == 0);

    /* 2. */
    struct drm_auth auth = { 0 };
    CHECK(ioctl(fd, IOW(DRM_NR_AUTH_MAGIC, struct drm_auth), &auth) == -EINVAL);
    int other = open("/dev/dri/card0", O_RDWR);
    CHECK(other >= 0);
    CHECK(ioctl(other, IOW(DRM_NR_AUTH_MAGIC, struct drm_auth), &auth) == -EACCES);
    CHECK(ioctl(other, IO(DRM_NR_SET_MASTER), 0) == -16);      /* EBUSY */
    CHECK(ioctl(other, IOR(DRM_NR_GET_MAGIC, struct drm_auth), &auth) == 0 && auth.magic);
    CHECK(ioctl(fd, IOW(DRM_NR_AUTH_MAGIC, struct drm_auth), &auth) == 0);

    /* 3. */
    struct drm_mode_card_res res = { 0 };
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_GETRESOURCES, struct drm_mode_card_res), &res) == 0);
    CHECK(res.count_crtcs == 1 && res.count_connectors == 1 && res.count_encoders == 1);
    uint32_t crtc, conn, enc;
    res.crtc_id_ptr = ptr(&crtc);
    res.connector_id_ptr = ptr(&conn);
    res.encoder_id_ptr = ptr(&enc);
    res.count_fbs = 0;
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_GETRESOURCES, struct drm_mode_card_res), &res) == 0);
    struct drm_mode_get_connector c = { 0 };
    c.connector_id = conn;
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_GETCONNECTOR, struct drm_mode_get_connector), &c) == 0);
    CHECK(c.connection == DRM_MODE_CONNECTED && c.count_modes > 3 && c.count_modes <= 32);
    uint32_t nmodes = c.count_modes, cenc, cprop;
    uint64_t cval;
    c.modes_ptr = ptr(modes);
    c.encoders_ptr = ptr(&cenc);
    c.props_ptr = ptr(&cprop);
    c.prop_values_ptr = ptr(&cval);
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_GETCONNECTOR, struct drm_mode_get_connector), &c) == 0);
    int pref = -1, small = -1;
    for (uint32_t i = 0; i < nmodes; i++) {
        if (modes[i].type & 8)                  /* DRM_MODE_TYPE_PREFERRED */
            pref = (int)i;
        if (modes[i].hdisplay == 1024 && modes[i].vdisplay == 768)
            small = (int)i;
    }
    CHECK(pref >= 0 && modes[pref].hdisplay == 1920 && modes[pref].vdisplay == 1200);
    CHECK(small >= 0 && cenc == enc && cval == DRM_MODE_DPMS_ON);
    struct drm_mode_get_encoder e = { enc, 0, 0, 0, 0 };
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_GETENCODER, struct drm_mode_get_encoder), &e) == 0);
    CHECK(e.crtc_id == crtc && e.possible_crtcs == 1);
    uint32_t plane;
    struct drm_mode_get_plane_res pr = { ptr(&plane), 1 };
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_GETPLANERESOURCES, struct drm_mode_get_plane_res), &pr) == 0);
    CHECK(pr.count_planes == 1);
    uint32_t formats[4];
    struct drm_mode_get_plane gp = { 0 };
    gp.plane_id = plane;
    gp.count_format_types = 4;
    gp.format_type_ptr = ptr(formats);
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_GETPLANE, struct drm_mode_get_plane), &gp) == 0);
    CHECK(gp.count_format_types == 2 && formats[0] == 0x34325258);     /* XRGB8888 */
    uint32_t pprop;
    uint64_t pval;
    struct drm_mode_obj_get_properties op = { ptr(&pprop), ptr(&pval), 1, plane, DRM_MODE_OBJECT_PLANE };
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_OBJ_GETPROPERTIES, struct drm_mode_obj_get_properties), &op) == 0);
    CHECK(op.count_props == 1 && pval == DRM_PLANE_TYPE_PRIMARY);
    struct drm_mode_get_property prop = { 0 };
    prop.prop_id = pprop;
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_GETPROPERTY, struct drm_mode_get_property), &prop) == 0);
    CHECK(streq(prop.name, "type") && prop.count_enum_blobs == 3);

    /* 4. */
    struct drm_mode_create_dumb cd[2];
    uint32_t *px[2], fb[2];
    for (int k = 0; k < 2; k++) {
        cd[k] = (struct drm_mode_create_dumb){ 768, 1024, 32, 0, 0, 0, 0 };
        CHECK(ioctl(fd, IOWR(DRM_NR_MODE_CREATE_DUMB, struct drm_mode_create_dumb), &cd[k]) == 0);
        CHECK(cd[k].pitch == 4096 && cd[k].size == 4096 * 768);
        struct drm_mode_map_dumb md = { cd[k].handle, 0, 0 };
        CHECK(ioctl(fd, IOWR(DRM_NR_MODE_MAP_DUMB, struct drm_mode_map_dumb), &md) == 0);
        px[k] = mmap(0, cd[k].size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (long)md.offset);
        CHECK((long)px[k] > 0 && px[k][0] == 0);            /* new buffers are black */
        for (uint32_t i = 0; i < 1024 * 768; i += 997)
            px[k][i] = 0x00A0B0C0u + (uint32_t)k;
        struct drm_mode_fb_cmd2 f = { 0 };
        f.width = 1024;
        f.height = 768;
        f.pixel_format = 0x34325258;
        f.handles[0] = cd[k].handle;
        f.pitches[0] = cd[k].pitch;
        CHECK(ioctl(fd, IOWR(DRM_NR_MODE_ADDFB2, struct drm_mode_fb_cmd2), &f) == 0);
        fb[k] = f.fb_id;
    }
    /* In video memory: /dev/fb0 maps all of it; buffer 0 is in slot 1. */
    int fb0 = open("/dev/fb0", O_RDWR);
    CHECK(fb0 >= 0);
    uint32_t *vram = mmap(0, 16 << 20, PROT_READ, MAP_SHARED, fb0, 0);
    CHECK((long)vram > 0);
    CHECK(vram[(4096 * 768) / 4 + 997] == 0x00A0B0C0u && vram[2 * (4096 * 768) / 4 + 997] == 0x00A0B0C1u);
    struct drm_mode_crtc sc = { 0 };
    sc.set_connectors_ptr = ptr(&conn);
    sc.count_connectors = 1;
    sc.crtc_id = crtc;
    sc.fb_id = fb[0];
    sc.mode_valid = 1;
    sc.mode = modes[small];
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_SETCRTC, struct drm_mode_crtc), &sc) == 0);
    struct drm_mode_crtc gc = { 0 };
    gc.crtc_id = crtc;
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_GETCRTC, struct drm_mode_crtc), &gc) == 0);
    CHECK(gc.mode_valid && gc.fb_id == fb[0] && gc.mode.hdisplay == 1024);
    CHECK(vram[(4096 * 768) / 4 + 997] == 0x00A0B0C0u);    /* the mode change kept it */

    /* 5. */
    struct drm_mode_crtc_page_flip pf = { crtc, fb[1], DRM_MODE_PAGE_FLIP_EVENT, 0, 0x1234 };
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_PAGE_FLIP, struct drm_mode_crtc_page_flip), &pf) == 0);
    pf.fb_id = fb[0];
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_PAGE_FLIP, struct drm_mode_crtc_page_flip), &pf) == -16);
    struct { int fd; short events, revents; } pfd = { fd, 1, 0 };
    CHECK(syscall3(SYS_poll, (long)&pfd, 1, 100) == 1 && (pfd.revents & 1));
    struct drm_event_vblank ev;
    CHECK(read(fd, &ev, sizeof ev) == sizeof ev);
    CHECK(ev.base.type == DRM_EVENT_FLIP_COMPLETE && ev.base.length == 32);
    CHECK(ev.user_data == 0x1234 && ev.crtc_id == crtc && ev.sequence);
    CHECK(fcntl(fd, 4, O_RDWR | O_NONBLOCK) == 0);          /* F_SETFL */
    CHECK(read(fd, &ev, sizeof ev) == -EAGAIN);
    CHECK(fcntl(fd, 4, O_RDWR) == 0);
    uint32_t seq = ev.sequence;
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_PAGE_FLIP, struct drm_mode_crtc_page_flip), &pf) == 0);
    CHECK(read(fd, &ev, sizeof ev) == sizeof ev && ev.sequence > seq);   /* blocks for it */

    /* 6. */
    struct drm_prime_handle ph = { cd[1].handle, DRM_CLOEXEC | DRM_RDWR, -1 };
    CHECK(ioctl(fd, IOWR(DRM_NR_PRIME_HANDLE_TO_FD, struct drm_prime_handle), &ph) == 0 && ph.fd > 0);
    uint32_t *dp = mmap(0, cd[1].size, PROT_READ, MAP_SHARED, ph.fd, 0);
    CHECK((long)dp > 0 && dp[997] == 0x00A0B0C1u);
    struct drm_prime_handle back = { 0, 0, ph.fd };
    CHECK(ioctl(fd, IOWR(DRM_NR_PRIME_FD_TO_HANDLE, struct drm_prime_handle), &back) == 0);
    CHECK(back.handle == cd[1].handle);

    /* 7. */
    struct drm_mode_map_dumb md = { cd[0].handle, 0, 0 };
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_MAP_DUMB, struct drm_mode_map_dumb), &md) == 0);
    CHECK((long)mmap(0, 4096, PROT_READ, MAP_SHARED, other, (long)md.offset) == -EACCES);
    CHECK(ioctl(other, IOWR(DRM_NR_MODE_SETCRTC, struct drm_mode_crtc), &sc) == -EACCES);
    back.handle = 0;                    /* the dma-buf gives the other file a handle */
    CHECK(ioctl(other, IOWR(DRM_NR_PRIME_FD_TO_HANDLE, struct drm_prime_handle), &back) == 0);
    CHECK(back.handle == 1 && close(ph.fd) == 0);
    CHECK(munmap(dp, cd[1].size) == 0);

    /* 8. */
    for (int k = 0; k < 2; k++) {
        CHECK(ioctl(fd, IOWR(DRM_NR_MODE_RMFB, uint32_t), &fb[k]) == 0);
        CHECK(munmap(px[k], cd[k].size) == 0);
        struct drm_mode_destroy_dumb dd = { cd[k].handle };
        CHECK(ioctl(fd, IOWR(DRM_NR_MODE_DESTROY_DUMB, struct drm_mode_destroy_dumb), &dd) == 0);
    }
    CHECK(close(other) == 0);           /* its handle to buffer 1 goes too */
    struct drm_mode_create_dumb big = { 1200, 1920, 32, 0, 0, 0, 0 };
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_CREATE_DUMB, struct drm_mode_create_dumb), &big) == 0);
    CHECK(big.pitch == 7680);
    md.handle = big.handle;
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_MAP_DUMB, struct drm_mode_map_dumb), &md) == 0);
    uint32_t *bp = mmap(0, big.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (long)md.offset);
    CHECK((long)bp > 0);
    bp[1920 * 600 + 960] = 0x00123456u;
    struct drm_mode_fb_cmd lf = { 0, 1920, 1200, 7680, 32, 24, big.handle };
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_ADDFB, struct drm_mode_fb_cmd), &lf) == 0);
    sc.fb_id = lf.fb_id;
    sc.mode = modes[pref];
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_SETCRTC, struct drm_mode_crtc), &sc) == 0);
    CHECK(vram[1920 * 600 + 960] == 0x00123456u);           /* copied to slot 0 */
    bp[1920 * 600 + 960] = 0x00654321u;
    struct drm_mode_fb_dirty_cmd dirty = { lf.fb_id, 0, 0, 0, 0 };
    CHECK(ioctl(fd, IOWR(DRM_NR_MODE_DIRTYFB, struct drm_mode_fb_dirty_cmd), &dirty) == 0);
    CHECK(vram[1920 * 600 + 960] == 0x00654321u);

    /* 9. */
    CHECK(close(fd) == 0);              /* buffers, framebuffers, master: all gone */
    int mode = -1;
    CHECK(ioctl(1, KDGETMODE, &mode) == 0 && mode == KD_TEXT);
    unsigned short ws[4];
    CHECK(ioctl(1, 0x5413, ws) == 0 && ws[0] == 37 && ws[1] == 120);
    CHECK(munmap(bp, big.size) == 0 && munmap(vram, 16 << 20) == 0 && close(fb0) == 0);
    fd = open("/dev/dri/card0", O_RDWR);
    CHECK(fd >= 0);                     /* the next opener is master again */
    auth.magic = 0;
    CHECK(ioctl(fd, IOW(DRM_NR_AUTH_MAGIC, struct drm_auth), &auth) == -EINVAL);
    CHECK(close(fd) == 0);

    /* 10. What libdrm and libudev read in /sys. */
    fd = open("/sys/class", O_RDONLY | O_DIRECTORY);   /* boottest mounted it */
    if (fd < 0) {
        syscall3(SYS_mkdir, (long)"/sys", 0555, 0);
        CHECK(syscall5(SYS_mount, (long)"sys", (long)"/sys", (long)"sysfs", 0, 0) == 0);
    } else {
        close(fd);
    }
    char buf[128];
    long n = readlink("/sys/dev/char/226:0", buf, sizeof buf - 1);
    CHECK(n > 0);
    buf[n] = 0;
    CHECK(streq(buf, "../../devices/pci0000:00/0000:00:02.0/drm/card0"));
    CHECK(slurp("/sys/dev/char/226:0/uevent", buf, sizeof buf) > 0 &&
          streq(buf, "MAJOR=226\nMINOR=0\nDEVNAME=dri/card0\nDEVTYPE=drm_minor\n"));
    CHECK(slurp("/sys/dev/char/226:0/device/vendor", buf, sizeof buf) > 0 && streq(buf, "0x1234\n"));
    CHECK(slurp("/sys/class/drm/card0/device/boot_vga", buf, sizeof buf) > 0 && streq(buf, "1\n"));
    fd = open("/sys/dev/char/226:0/device/drm", O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0 && close(fd) == 0);
    n = readlink("/sys/class/drm/card0/subsystem", buf, sizeof buf - 1);
    CHECK(n > 0);
    buf[n] = 0;
    CHECK(streq(buf, "../../../../../class/drm"));
    CHECK(slurp("/sys/class/input/event1/uevent", buf, sizeof buf) > 0 &&
          streq(buf, "MAJOR=13\nMINOR=65\nDEVNAME=input/event1\nID_INPUT=1\nID_INPUT_MOUSE=1\n"));
    static char big_text[512];
    CHECK(slurp("/sys/class/input/input1/uevent", big_text, sizeof big_text) > 0);
    CHECK(has_line(big_text, "EV=7") && has_line(big_text, "KEY=70000 0 0 0 0 0 0 0 0") &&
          has_line(big_text, "REL=103"));

    put("drmtest: all ");
    put_u64(checks);
    put(" checks passed\n");
    return 0;
}
