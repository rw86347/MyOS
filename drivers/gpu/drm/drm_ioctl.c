/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * drm_ioctl.c — /dev/dri/card0: the display, the way a Wayland compositor
 * drives it
 *
 * X drew through /dev/fb0: one screen-sized piece of memory, written in
 * place. A Wayland compositor (wlroots) wants Linux's DRM/KMS interface
 * instead, and with it three new ideas:
 *
 *   Buffers. The compositor asks for "dumb buffers" (CREATE_DUMB), maps
 *   them (MAP_DUMB + mmap), draws a whole frame into one while another is
 *   on the screen, then hands the new one over.
 *
 *   Page flips. Handing over is PAGE_FLIP: "show this framebuffer from the
 *   next refresh on, and tell me when you did". The answer is an event the
 *   compositor read()s from the card's fd; it draws the next frame only
 *   then. That is what paces the desktop at 60 frames a second without a
 *   timer of its own, and why an animation (Mission Control's zoom) is
 *   smooth: one frame per refresh, never two in the same one.
 *
 *   A master. One open file at a time may change what is shown — the
 *   compositor. When it closes or drops master, the text console gets
 *   the screen back.
 *
 * Only the "legacy" half of KMS is here (SETCRTC, PAGE_FLIP, properties),
 * not the atomic one: wlroots asks for the atomic client cap, is refused,
 * and uses legacy, which is all a single-screen, single-plane card needs.
 * There is no hardware cursor plane either (CURSOR with a buffer fails),
 * so wlroots draws the pointer into its frames.
 *
 * Two kinds of screen:
 *   - QEMU's std VGA (Bochs, drm_primary): buffers live in video memory
 *     when they fit, and a flip is two register writes (the scan-out
 *     offset) — no copying. Buffers that don't fit go to RAM and a flip
 *     copies the frame into the start of video memory.
 *   - The UEFI framebuffer (simplefb, real hardware): one fixed mode, no
 *     registers. Buffers live in RAM; a flip copies the frame to it.
 * Copying a 1920x1200 frame is ~9 MiB per flip: fine for QEMU and for a
 * preview, and the reason the Bochs path flips in place when it can.
 *
 * Costs: every ioctl is O(1) or a scan of a small fixed table (64
 * buffers, 32 framebuffers, 32 handles per file). A flip is O(1) on the
 * Bochs card, a frame copy otherwise. The flip event is timed by one
 * kernel thread ("drmvbl") that sleeps until the next 60 Hz boundary.
 */
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_simple_kms_helper.h>
#include <errno.h>
#include <interrupts.h>
#include <kheap.h>
#include <linux/fb.h>
#include <memlayout.h>
#include <mm.h>
#include <pmm.h>
#include <proc.h>
#include <signal.h>
#include <string.h>
#include <thread.h>
#include <timer.h>
#include <uaccess.h>
#include <uapi/drm.h>
#include <vfs.h>
#include <vt.h>
#include "drm_internal.h"

#define MAX_BOS      64                 /* buffers, system-wide */
#define MAX_FBS      32                 /* framebuffers, system-wide */
#define MAX_HANDLES  32                 /* buffer handles per open file */
#define FB_ID_BASE   64                 /* framebuffer ids: 64, 65, ... */
#define MAP_STRIDE   (32u << 20)        /* mmap offset of buffer k: (k + 1) * 32 MiB */
#define BO_MAX_BYTES (32u << 20)        /* one buffer: 4096x2048 at most */
#define RAM_CAP      ((48u << 20) / PAGE_SIZE)  /* RAM for buffers, all of them */
#define POOL_MAX     32
#define PAGE_UP(x)   (((x) + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1))

/* Object ids: a fixed set, because there is one of each. */
enum {
    PROP_DPMS = 1, PROP_TYPE = 2,
    ID_PLANE = 31, ID_CRTC = 32, ID_ENCODER = 33, ID_CONNECTOR = 34,
};

/* A buffer (a "GEM object"): where its pixels are and who still uses it. */
struct drm_bo {
    uint32_t refs;                      /* handles + framebuffers + dma-buf files; 0: free */
    uint32_t width, height, pitch, size;
    uint8_t vram;                       /* 1: in video memory at vram_off */
    uint8_t pool;                       /* else: its block in ram_pool */
    uint32_t vram_off;
    phys_addr_t phys;
    uint8_t *kva;                       /* the kernel's address of its first byte */
};

/* A framebuffer: a buffer plus how to read pixels out of it. */
struct drm_fbo {
    struct drm_bo *bo;                  /* NULL: free slot */
    struct drm_file *owner;             /* removed when it closes, as in Linux */
    uint32_t width, height, pitch, offset, format;
};

/* One open of /dev/dri/card0. */
struct drm_file {
    struct drm_bo *handles[MAX_HANDLES];    /* handle h is handles[h - 1] */
    uint32_t magic;
    uint8_t authenticated;
    uint8_t universal_planes;
    uint8_t ev_state;                   /* EV_* below */
    uint32_t ev_due, ev_seq;            /* the flip's refresh: timer_ms() and count */
    uint64_t ev_data;                   /* the compositor's user_data, given back */
    struct wait_queue wait;             /* a blocking read() */
    struct drm_file *next;              /* every open file, for drmvbl */
};
enum { EV_NONE, EV_QUEUED, EV_READY };

static struct drm_bo bos[MAX_BOS];
static struct drm_fbo fbs[MAX_FBS];

/* RAM blocks for buffers. Freed buffers give their block back here, not
   to the page allocator: a program can keep a mapping of a buffer after
   closing its handle (Linux removes such mappings; we can't yet), and
   this way such a mapping can only ever reach another buffer's pixels,
   never memory the kernel reused for something else. docs/gaps.md. */
static struct {
    uint32_t phys, pages;               /* pages 0: unused entry */
    uint8_t used;
} ram_pool[POOL_MAX];
static uint32_t ram_pages;

static struct {
    int ready;                          /* there is a screen to drive */
    struct drm_device *dev;             /* the Bochs card, or NULL: simplefb */
    struct fb_info *info;               /* simplefb */
    struct drm_display_mode fixed;      /* simplefb's one mode */
    uint32_t format;                    /* the screen's own pixel format */
    const char *name;
    /* What the compositor set. */
    int active;                         /* its picture is on the screen */
    int graphics;                       /* we took the screen from the console */
    struct drm_display_mode mode;
    uint32_t fb_id;                     /* shown (0: none) */
    uint32_t dpms;
    struct drm_framebuffer scan;        /* Bochs: what the display pipe shows */
    uint32_t slot_bytes, slots_used;    /* Bochs video memory, below */
    struct drm_file *master;
    struct drm_file *files;
    uint32_t magic_next;
    struct thread *vbl;
    struct wait_queue vbl_wait;
} kms;

/* --- small helpers ------------------------------------------------------ */

/* A user pointer from a 64-bit ioctl field (0 on i386 if it can't be one). */
static void *uptr(uint64_t v)
{
    return (uint64_t)(uintptr_t)v == v ? (void *)(uintptr_t)v : (void *)(uintptr_t)0;
}

/* libdrm asks twice: first with counts of 0 to learn them, then with
   arrays that big. An array is filled only if it has room, as in Linux. */
static int put_array(uint64_t ptr, uint32_t room, const void *src, uint32_t n, uint32_t size)
{
    if (!n || room < n)
        return 0;
    return copy_to_user(uptr(ptr), src, n * size) < 0 ? -EFAULT : 0;
}

static const struct drm_display_mode *mode_list(int *n)
{
    if (kms.dev) {
        struct drm_connector *c = kms.dev->pipe->connector;
        *n = c->num_modes;
        return c->modes;
    }
    *n = 1;
    return &kms.fixed;
}

static void modeinfo_from(struct drm_mode_modeinfo *u, const struct drm_display_mode *m)
{
    memset(u, 0, sizeof *u);
    u->clock = (uint32_t)m->clock;
    u->hdisplay = m->hdisplay; u->hsync_start = m->hsync_start;
    u->hsync_end = m->hsync_end; u->htotal = m->htotal; u->hskew = m->hskew;
    u->vdisplay = m->vdisplay; u->vsync_start = m->vsync_start;
    u->vsync_end = m->vsync_end; u->vtotal = m->vtotal; u->vscan = m->vscan;
    u->vrefresh = 60;
    u->flags = m->flags;
    u->type = m->type | DRM_MODE_TYPE_DRIVER;
    memcpy(u->name, m->name, DRM_DISPLAY_MODE_LEN);
}

/* The mode of ours a client's modeinfo names (it copied it from our list). */
static const struct drm_display_mode *mode_find(const struct drm_mode_modeinfo *u)
{
    int n;
    const struct drm_display_mode *m = mode_list(&n);
    for (int i = 0; i < n; i++)
        if (m[i].hdisplay == u->hdisplay && m[i].vdisplay == u->vdisplay &&
            (uint32_t)m[i].clock == u->clock)
            return &m[i];
    return NULL;
}

static int format_ok(uint32_t f)
{
    if (kms.format == DRM_FORMAT_XRGB8888)
        return f == DRM_FORMAT_XRGB8888 || f == DRM_FORMAT_ARGB8888;
    return f == DRM_FORMAT_XBGR8888 || f == DRM_FORMAT_ABGR8888;
}

/* --- buffers ------------------------------------------------------------ */

/*
 * Bochs video memory is cut into equal slots, one buffer each, the size of
 * the first buffer made (a compositor's buffers are all the screen's
 * size). Slot 0 is kept back: it is where frames from RAM buffers are
 * copied to be shown. When no slot is in use the size can change.
 */
static int vram_alloc(uint32_t bytes, uint32_t *off)
{
    if (!kms.dev)
        return -1;
    if (!kms.slots_used)
        kms.slot_bytes = bytes;
    if (bytes != kms.slot_bytes)
        return -1;
    uint32_t slots = kms.dev->vram_size / bytes;
    for (uint32_t k = 1; k < slots && k < 32; k++)
        if (!(kms.slots_used & (1u << k))) {
            kms.slots_used |= 1u << k;
            *off = k * bytes;
            return 0;
        }
    return -1;
}

static int pool_alloc(uint32_t pages)
{
    int best = -1;
    for (int i = 0; i < POOL_MAX; i++)      /* the smallest free block that fits */
        if (ram_pool[i].pages >= pages && !ram_pool[i].used &&
            (best < 0 || ram_pool[i].pages < ram_pool[best].pages))
            best = i;
    if (best >= 0) {
        ram_pool[best].used = 1;
        return best;
    }
    if (ram_pages + pages > RAM_CAP)
        return -1;
    for (int i = 0; i < POOL_MAX; i++)
        if (!ram_pool[i].pages) {
            uint32_t pa = pmm_alloc_run(pages);
            if (!pa)
                return -1;
            ram_pool[i].phys = pa;
            ram_pool[i].pages = pages;
            ram_pool[i].used = 1;
            ram_pages += pages;
            return i;
        }
    return -1;
}

static struct drm_bo *bo_new(uint32_t width, uint32_t height)
{
    struct drm_bo *bo = NULL;
    for (int i = 0; i < MAX_BOS && !bo; i++)
        if (!bos[i].refs)
            bo = &bos[i];
    if (!bo)
        return NULL;
    uint32_t pitch = (width * 4 + 63) & ~63u;   /* whole cache lines per row */
    uint32_t size = PAGE_UP(pitch * height);
    memset(bo, 0, sizeof *bo);
    bo->width = width;
    bo->height = height;
    bo->pitch = pitch;
    bo->size = size;
    uint32_t off;
    if (vram_alloc(size, &off) == 0) {
        bo->vram = 1;
        bo->vram_off = off;
        bo->phys = kms.dev->vram_base + off;
        bo->kva = (uint8_t *)kms.dev->vram + off;
    } else {
        int p = pool_alloc(size / PAGE_SIZE);
        if (p < 0)
            return NULL;
        bo->pool = (uint8_t)p;
        bo->phys = ram_pool[p].phys;
        bo->kva = P2V(ram_pool[p].phys);
    }
    memset(bo->kva, 0, size);           /* never show a program old pixels */
    bo->refs = 1;
    return bo;
}

static void bo_put(struct drm_bo *bo)
{
    if (--bo->refs)
        return;
    if (bo->vram)
        kms.slots_used &= ~(1u << (bo->vram_off / kms.slot_bytes));
    else
        ram_pool[bo->pool].used = 0;
}

static int bo_index(const struct drm_bo *bo) { return (int)(bo - bos); }

/* Map bo's pixels: file offset base is its first byte. */
static int bo_mmap(struct drm_bo *bo, uint32_t base, struct vma *vma)
{
    int err = vm_map_ram(vma, base, bo->phys, bo->size);
    if (err == 0 && bo->vram) {
        vma->flags &= ~VM_RAM;          /* video memory: write-combined, like fb0 */
        vma->flags |= VM_WC;
    }
    return err;
}

static struct drm_bo *handle_bo(struct drm_file *df, uint32_t h)
{
    return h - 1 < MAX_HANDLES ? df->handles[h - 1] : NULL;
}

/* A handle in df for bo (one more reference), or -ENOSPC. */
static int handle_new(struct drm_file *df, struct drm_bo *bo)
{
    for (uint32_t i = 0; i < MAX_HANDLES; i++)
        if (!df->handles[i]) {
            df->handles[i] = bo;
            bo->refs++;
            return (int)i + 1;
        }
    return -ENOSPC;
}

static int handle_close(struct drm_file *df, uint32_t h)
{
    struct drm_bo *bo = handle_bo(df, h);
    if (!bo)
        return -EINVAL;
    df->handles[h - 1] = NULL;
    bo_put(bo);
    return 0;
}

/* --- framebuffers ---------------------------------------------------------- */

static struct drm_fbo *fb_lookup(uint32_t id)
{
    uint32_t k = id - FB_ID_BASE;
    return k < MAX_FBS && fbs[k].bo ? &fbs[k] : NULL;
}

static int fb_add(struct drm_file *df, uint32_t handle, uint32_t w, uint32_t h,
                  uint32_t pitch, uint32_t offset, uint32_t format, uint32_t *id)
{
    struct drm_bo *bo = handle_bo(df, handle);
    if (!bo)
        return -ENOENT;
    if (!w || !h || w > 4096 || h > 4096 || !format_ok(format) ||
        pitch < w * 4 || pitch % 4 || offset % 4)
        return -EINVAL;
    if ((uint64_t)offset + (uint64_t)pitch * (h - 1) + w * 4 > bo->size)
        return -EINVAL;
    for (uint32_t k = 0; k < MAX_FBS; k++)
        if (!fbs[k].bo) {
            fbs[k] = (struct drm_fbo){ bo, df, w, h, pitch, offset, format };
            bo->refs++;
            *id = FB_ID_BASE + k;
            return 0;
        }
    return -ENOSPC;
}

static void fb_remove(struct drm_fbo *fb)
{
    if (kms.fb_id == FB_ID_BASE + (uint32_t)(fb - fbs))
        kms.fb_id = 0;                  /* the picture stays until the next one */
    bo_put(fb->bo);
    fb->bo = NULL;
}

/* --- showing a frame -------------------------------------------------------- */

/* Rows of a frame from a RAM buffer onto the screen (dst, dpitch). */
static void copy_frame(uint8_t *dst, uint32_t dpitch, const struct drm_fbo *fb,
                       uint32_t w, uint32_t h)
{
    const uint8_t *src = fb->bo->kva + fb->offset;
    if (w > fb->width)
        w = fb->width;
    if (h > fb->height)
        h = fb->height;
    if (dpitch == fb->pitch && w * 4 == fb->pitch) {
        memcpy(dst, src, fb->pitch * h);    /* one run: the usual case */
        return;
    }
    for (uint32_t y = 0; y < h; y++)
        memcpy(dst + y * dpitch, src + y * fb->pitch, w * 4);
}

static int same_mode(const struct drm_display_mode *a, const struct drm_display_mode *b)
{
    return a->hdisplay == b->hdisplay && a->vdisplay == b->vdisplay && a->clock == b->clock;
}

/* Put fb on the screen in mode m. */
static int show(struct drm_fbo *fb, const struct drm_display_mode *m)
{
    if (!kms.dev) {                     /* simplefb: copy it there */
        struct fb_info *info = kms.info;
        copy_frame((uint8_t *)info->screen_base, info->fix.line_length, fb,
                   info->var.xres, info->var.yres);
        return 0;
    }
    struct drm_device *dev = kms.dev;
    struct drm_bo *bo = fb->bo;
    int recommit = !kms.active || !same_mode(m, &kms.mode) || kms.scan.pitches[0] != fb->pitch;
    kms.scan.dev = dev;
    kms.scan.format = drm_format_info(DRM_FORMAT_XRGB8888);
    kms.scan.width = fb->width;
    kms.scan.height = dev->vram_size / fb->pitch > 0xFFFF ? 0xFFFF : dev->vram_size / fb->pitch;
    kms.scan.pitches[0] = fb->pitch;
    if (bo->vram) {
        kms.scan.offsets[0] = bo->vram_off + fb->offset;    /* flip in place */
    } else {
        uint32_t lines = m->vdisplay;   /* slot 0; never past it into slot 1 */
        if (kms.slots_used && lines > kms.slot_bytes / fb->pitch)
            lines = kms.slot_bytes / fb->pitch;
        copy_frame((uint8_t *)dev->vram, fb->pitch, fb, m->hdisplay, lines);
        kms.scan.offsets[0] = 0;
    }
    if (!recommit) {
        drm_plane_show(dev, &kms.scan);
        return 0;
    }
    dev->keep_vram = 1;                 /* the buffers in video memory stay */
    int err = drm_mode_commit(dev, m, &kms.scan);
    dev->keep_vram = 0;
    return err;
}

static void take_screen(void)
{
    if (!kms.graphics) {
        vt_set_graphics(1);             /* the console stops drawing */
        kms.graphics = 1;
    }
}

/* The compositor let go: the console's mode and text again. */
static void give_back_screen(void)
{
    kms.active = 0;
    kms.fb_id = 0;
    kms.dpms = DRM_MODE_DPMS_ON;
    if (!kms.graphics)
        return;
    kms.graphics = 0;
    if (kms.dev)
        drm_fbdev_restore();
    vt_set_graphics(0);                 /* redraws every row */
}

static void set_dpms(uint64_t v)
{
    kms.dpms = v == DRM_MODE_DPMS_ON ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF;
    if (kms.dev && kms.active)
        drm_blank(kms.dev, kms.dpms != DRM_MODE_DPMS_ON);
}

/* --- page-flip events --------------------------------------------------------- */

/* The next 60 Hz refresh after now: its time (ms) and its number. */
static uint32_t next_vblank(uint32_t now, uint32_t *seq)
{
    uint64_t k = (uint64_t)now * 60 / 1000 + 1;
    *seq = (uint32_t)k;
    return (uint32_t)((k * 1000 + 59) / 60);
}

static int ev_ready(struct drm_file *df)
{
    return df->ev_state == EV_READY ||
           (df->ev_state == EV_QUEUED && (int32_t)(timer_ms() - df->ev_due) >= 0);
}

/* drmvbl: sleeps until the earliest queued flip's refresh, then wakes
   that file's readers and pollers. Idle (asleep, no timer) with no flips. */
static void vbl_thread(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags = irq_save(), now = timer_ms(), next = 0;
        int any = 0, woke = 0;
        for (struct drm_file *df = kms.files; df; df = df->next) {
            if (df->ev_state != EV_QUEUED)
                continue;
            if ((int32_t)(now - df->ev_due) >= 0) {
                df->ev_state = EV_READY;
                wq_wake_all(&df->wait);
                woke = 1;
            } else if (!any || (int32_t)(df->ev_due - next) < 0) {
                next = df->ev_due;
                any = 1;
            }
        }
        if (woke)
            wq_wake_all(&poll_wait);
        if (any)
            wq_sleep_until(&kms.vbl_wait, next ? next : 1);
        else
            wq_sleep(&kms.vbl_wait);
        irq_restore(flags);
    }
}

static int queue_event(struct drm_file *df, uint64_t user_data)
{
    if (!kms.vbl) {
        kms.vbl = thread_create("drmvbl", vbl_thread, NULL);
        if (!kms.vbl)
            return -ENOMEM;
    }
    df->ev_data = user_data;
    df->ev_due = next_vblank(timer_ms(), &df->ev_seq);
    df->ev_state = EV_QUEUED;
    wq_wake_all(&kms.vbl_wait);
    return 0;
}

static int32_t drm_read(struct file *f, void *ubuf, uint32_t n)
{
    struct drm_file *df = f->priv;
    if (n < sizeof(struct drm_event_vblank))
        return -EINVAL;
    file_get(f);                        /* across the sleep (vfs.h) */
    int32_t r = 0;
    while (!ev_ready(df)) {
        if (f->flags & O_NONBLOCK)
            r = -EAGAIN;
        else if (signal_pending())
            r = -EINTR;
        if (r)
            break;
        wq_sleep_until(&df->wait, df->ev_state == EV_QUEUED ? df->ev_due : 0);
    }
    if (!r) {
        struct drm_event_vblank ev = {
            .base = { DRM_EVENT_FLIP_COMPLETE, sizeof ev },
            .user_data = df->ev_data,
            .tv_sec = df->ev_due / 1000,
            .tv_usec = df->ev_due % 1000 * 1000,    /* CLOCK_MONOTONIC, like clock_ns */
            .sequence = df->ev_seq,
            .crtc_id = ID_CRTC,
        };
        df->ev_state = EV_NONE;
        r = copy_to_user(ubuf, &ev, sizeof ev) < 0 ? -EFAULT : (int32_t)sizeof ev;
    }
    file_put(f);
    return r;
}

static uint32_t drm_poll(struct file *f)
{
    return ev_ready(f->priv) ? 0x001 | 0x040 : 0;   /* POLLIN | POLLRDNORM */
}

/* --- dma-buf: a buffer passed between programs as an fd ------------------------ */

static struct inode dmabuf_inode = {
    .ino = 0, .mode = S_IFREG | 0600, .nlink = 1, .name = "dmabuf", .name_len = 6,
};

static int dmabuf_mmap(struct file *f, struct vma *vma) { return bo_mmap(f->priv, 0, vma); }
static void dmabuf_release(struct file *f) { bo_put(f->priv); }

static const struct file_ops dmabuf_ops = { .mmap = dmabuf_mmap, .release = dmabuf_release };

static int prime_export(struct drm_file *df, struct drm_prime_handle *p)
{
    struct drm_bo *bo = handle_bo(df, p->handle);
    if (!bo)
        return -ENOENT;
    if (p->flags & ~(DRM_CLOEXEC | DRM_RDWR))
        return -EINVAL;
    struct file *f = file_new(&dmabuf_ops, &dmabuf_inode, O_RDWR);
    if (!f)
        return -ENOMEM;
    f->priv = bo;
    bo->refs++;
    int fd = fd_install(f);
    if (fd < 0) {
        file_put(f);
        return fd;
    }
    if ((p->flags & DRM_CLOEXEC) && proc_current())
        fd_set_cloexec(proc_current(), fd, 1);
    p->fd = fd;
    return 0;
}

static int prime_import(struct drm_file *df, struct drm_prime_handle *p)
{
    struct file *f = fd_get(p->fd);
    if (!f || f->ops != &dmabuf_ops)
        return -EINVAL;
    struct drm_bo *bo = f->priv;
    for (uint32_t i = 0; i < MAX_HANDLES; i++)
        if (df->handles[i] == bo) {     /* one handle per buffer per file, as in Linux */
            p->handle = i + 1;
            return 0;
        }
    int h = handle_new(df, bo);
    if (h < 0)
        return h;
    p->handle = (uint32_t)h;
    return 0;
}

/* --- the mode-setting ioctls ------------------------------------------------- */

static int get_resources(struct drm_mode_card_res *r)
{
    static const uint32_t crtc = ID_CRTC, conn = ID_CONNECTOR, enc = ID_ENCODER;
    uint32_t ids[MAX_FBS], n = 0;
    for (uint32_t k = 0; k < MAX_FBS; k++)
        if (fbs[k].bo)
            ids[n++] = FB_ID_BASE + k;
    int err = put_array(r->fb_id_ptr, r->count_fbs, ids, n, 4) |
              put_array(r->crtc_id_ptr, r->count_crtcs, &crtc, 1, 4) |
              put_array(r->connector_id_ptr, r->count_connectors, &conn, 1, 4) |
              put_array(r->encoder_id_ptr, r->count_encoders, &enc, 1, 4);
    r->count_fbs = n;
    r->count_crtcs = r->count_connectors = r->count_encoders = 1;
    r->min_width = r->min_height = 1;
    r->max_width = r->max_height = 4096;
    return err ? -EFAULT : 0;
}

static int get_connector(struct drm_mode_get_connector *c)
{
    if (c->connector_id != ID_CONNECTOR)
        return -ENOENT;
    int n;
    const struct drm_display_mode *m = mode_list(&n);
    static struct drm_mode_modeinfo info[DRM_CONNECTOR_MAX_MODES];  /* too big for the stack */
    for (int i = 0; i < n; i++)
        modeinfo_from(&info[i], &m[i]);
    static const uint32_t enc = ID_ENCODER, prop = PROP_DPMS;
    uint64_t dpms = kms.dpms;
    int err = put_array(c->modes_ptr, c->count_modes, info, (uint32_t)n, sizeof info[0]) |
              put_array(c->encoders_ptr, c->count_encoders, &enc, 1, 4) |
              put_array(c->props_ptr, c->count_props, &prop, 1, 4) |
              put_array(c->prop_values_ptr, c->count_props, &dpms, 1, 8);
    const struct drm_display_mode *pref = &m[0];
    for (int i = 0; i < n; i++)
        if (m[i].type & DRM_MODE_TYPE_PREFERRED)
            pref = &m[i];
    c->count_modes = (uint32_t)n;
    c->count_encoders = c->count_props = 1;
    c->encoder_id = ID_ENCODER;
    c->connector_type = kms.dev ? DRM_MODE_CONNECTOR_VIRTUAL : DRM_MODE_CONNECTOR_Unknown;
    c->connector_type_id = 1;
    c->connection = DRM_MODE_CONNECTED;
    c->mm_width = pref->hdisplay * 254u / 960;     /* 96 dots per inch */
    c->mm_height = pref->vdisplay * 254u / 960;
    c->subpixel = DRM_MODE_SUBPIXEL_UNKNOWN;
    return err ? -EFAULT : 0;
}

static int get_crtc(struct drm_mode_crtc *c)
{
    if (c->crtc_id != ID_CRTC)
        return -ENOENT;
    c->gamma_size = 0;
    c->x = c->y = 0;
    c->fb_id = kms.active ? kms.fb_id : 0;
    c->mode_valid = 1;
    if (kms.active) {
        modeinfo_from(&c->mode, &kms.mode);
    } else if (kms.dev) {               /* the console's mode */
        const struct drm_display_mode *cm = &kms.dev->pipe->crtc_state.mode;
        modeinfo_from(&c->mode, cm);
    } else {
        modeinfo_from(&c->mode, &kms.fixed);
    }
    return 0;
}

static int set_crtc(struct drm_mode_crtc *c)
{
    if (c->crtc_id != ID_CRTC)
        return -ENOENT;
    if (!c->mode_valid) {               /* off */
        if (c->fb_id)
            return -EINVAL;
        if (kms.dev && kms.active)
            drm_blank(kms.dev, 1);
        kms.active = 0;
        kms.fb_id = 0;
        return 0;
    }
    const struct drm_display_mode *m = mode_find(&c->mode);
    if (!m || c->x || c->y)
        return -EINVAL;
    if (c->count_connectors != 1)
        return -EINVAL;
    uint32_t conn;
    if (copy_from_user(&conn, uptr(c->set_connectors_ptr), 4) < 0)
        return -EFAULT;
    if (conn != ID_CONNECTOR)
        return -ENOENT;
    uint32_t id = c->fb_id == ~0u ? kms.fb_id : c->fb_id;  /* -1: keep the one shown */
    struct drm_fbo *fb = fb_lookup(id);
    if (!fb)
        return -ENOENT;
    if (fb->width < m->hdisplay || fb->height < m->vdisplay)
        return -ENOSPC;
    take_screen();
    int err = show(fb, m);
    if (err)
        return err;
    kms.active = 1;
    kms.mode = *m;
    kms.fb_id = id;
    if (kms.dpms != DRM_MODE_DPMS_ON)
        set_dpms(DRM_MODE_DPMS_ON);
    return 0;
}

static int page_flip(struct drm_file *df, const struct drm_mode_crtc_page_flip *p)
{
    if (p->crtc_id != ID_CRTC)
        return -ENOENT;
    if (p->flags & ~DRM_MODE_PAGE_FLIP_EVENT)
        return -EINVAL;                 /* no async flips, no target vblank */
    struct drm_fbo *fb = fb_lookup(p->fb_id);
    if (!fb)
        return -ENOENT;
    if (!kms.active)
        return -EINVAL;
    if (fb->width < kms.mode.hdisplay || fb->height < kms.mode.vdisplay)
        return -ENOSPC;
    if ((p->flags & DRM_MODE_PAGE_FLIP_EVENT) && df->ev_state != EV_NONE)
        return -EBUSY;                  /* the last one hasn't been read yet */
    int err = show(fb, &kms.mode);
    if (err)
        return err;
    kms.fb_id = p->fb_id;
    return p->flags & DRM_MODE_PAGE_FLIP_EVENT ? queue_event(df, p->user_data) : 0;
}

static int get_property(struct drm_mode_get_property *p)
{
    static const struct drm_mode_property_enum dpms[4] = {
        { 0, "On" }, { 1, "Standby" }, { 2, "Suspend" }, { 3, "Off" },
    };
    static const struct drm_mode_property_enum type[3] = {
        { 0, "Overlay" }, { 1, "Primary" }, { 2, "Cursor" },
    };
    const struct drm_mode_property_enum *e;
    uint32_t n;
    memset(p->name, 0, sizeof p->name);
    if (p->prop_id == PROP_DPMS) {
        e = dpms; n = 4;
        p->flags = DRM_MODE_PROP_ENUM;
        memcpy(p->name, "DPMS", 4);
    } else if (p->prop_id == PROP_TYPE) {
        e = type; n = 3;
        p->flags = DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE;
        memcpy(p->name, "type", 4);
    } else {
        return -ENOENT;
    }
    uint64_t values[4];
    for (uint32_t i = 0; i < n; i++)
        values[i] = e[i].value;
    int err = put_array(p->values_ptr, p->count_values, values, n, 8) |
              put_array(p->enum_blob_ptr, p->count_enum_blobs, e, n, sizeof *e);
    p->count_values = p->count_enum_blobs = n;
    return err ? -EFAULT : 0;
}

static int obj_properties(struct drm_file *df, struct drm_mode_obj_get_properties *o)
{
    uint32_t id = 0;
    uint64_t value = 0;
    uint32_t t = o->obj_type;
    if (o->obj_id == ID_CONNECTOR && (t == DRM_MODE_OBJECT_CONNECTOR || t == DRM_MODE_OBJECT_ANY)) {
        id = PROP_DPMS;
        value = kms.dpms;
    } else if (o->obj_id == ID_PLANE && (t == DRM_MODE_OBJECT_PLANE || t == DRM_MODE_OBJECT_ANY)) {
        if (df->universal_planes) {
            id = PROP_TYPE;
            value = DRM_PLANE_TYPE_PRIMARY;
        }
    } else if (!((o->obj_id == ID_CRTC && (t == DRM_MODE_OBJECT_CRTC || t == DRM_MODE_OBJECT_ANY)) ||
                 (o->obj_id == ID_ENCODER && (t == DRM_MODE_OBJECT_ENCODER || t == DRM_MODE_OBJECT_ANY)))) {
        return -ENOENT;
    }
    uint32_t n = id ? 1 : 0;
    int err = put_array(o->props_ptr, o->count_props, &id, n, 4) |
              put_array(o->prop_values_ptr, o->count_props, &value, n, 8);
    o->count_props = n;
    return err ? -EFAULT : 0;
}

static int set_property(uint32_t obj, uint32_t prop, uint64_t value)
{
    if (obj != ID_CONNECTOR || prop != PROP_DPMS)
        return obj == ID_CONNECTOR || obj == ID_PLANE || obj == ID_CRTC ? -EINVAL : -ENOENT;
    if (value > DRM_MODE_DPMS_OFF)
        return -EINVAL;
    set_dpms(value);
    return 0;
}

static int get_plane(struct drm_mode_get_plane *p)
{
    if (p->plane_id != ID_PLANE)
        return -ENOENT;
    uint32_t formats[2] = { kms.format, kms.format == DRM_FORMAT_XRGB8888 ?
                                        DRM_FORMAT_ARGB8888 : DRM_FORMAT_ABGR8888 };
    int err = put_array(p->format_type_ptr, p->count_format_types, formats, 2, 4);
    p->crtc_id = kms.active ? ID_CRTC : 0;
    p->fb_id = kms.active ? kms.fb_id : 0;
    p->possible_crtcs = 1;
    p->gamma_size = 0;
    p->count_format_types = 2;
    return err ? -EFAULT : 0;
}

static int get_cap(struct drm_get_cap *c)
{
    switch (c->capability) {
    case DRM_CAP_DUMB_BUFFER:          c->value = 1; break;
    case DRM_CAP_DUMB_PREFERRED_DEPTH: c->value = 24; break;
    case DRM_CAP_DUMB_PREFER_SHADOW:   c->value = 0; break;
    case DRM_CAP_PRIME:                c->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; break;
    case DRM_CAP_TIMESTAMP_MONOTONIC:  c->value = 1; break;
    case DRM_CAP_CRTC_IN_VBLANK_EVENT: c->value = 1; break;
    case DRM_CAP_VBLANK_HIGH_CRTC:
    case DRM_CAP_ASYNC_PAGE_FLIP:
    case DRM_CAP_ADDFB2_MODIFIERS:
    case DRM_CAP_PAGE_FLIP_TARGET:
    case DRM_CAP_SYNCOBJ:
    case DRM_CAP_SYNCOBJ_TIMELINE:     c->value = 0; break;
    case DRM_CAP_CURSOR_WIDTH:
    case DRM_CAP_CURSOR_HEIGHT:        c->value = 64; break;
    default: return -EINVAL;
    }
    return 0;
}

static int version(struct drm_version *v)
{
    static const char date[] = "20261021", desc[] = "MyOS DRM";
    const char *strs[3] = { kms.name, date, desc };
    size_t *lens[3] = { &v->name_len, &v->date_len, &v->desc_len };
    char *bufs[3] = { v->name, v->date, v->desc };
    for (int k = 0; k < 3; k++) {
        size_t n = strlen(strs[k]);
        size_t room = *lens[k];
        if (room && copy_to_user(bufs[k], strs[k], room < n ? (uint32_t)room : (uint32_t)n) < 0)
            return -EFAULT;
        *lens[k] = n;
    }
    v->version_major = 1;
    v->version_minor = 0;
    v->version_patchlevel = 0;
    return 0;
}

/* --- the ioctl switch ------------------------------------------------------- */

static int is_master(struct drm_file *df) { return kms.master == df; }

static void release_master(struct drm_file *df)
{
    if (kms.master != df)
        return;
    kms.master = NULL;
    give_back_screen();
}

/* The size of each ioctl's argument, by number (0: not one of ours). */
static uint32_t arg_size(uint32_t nr)
{
    switch (nr) {
    case DRM_NR_VERSION:                return sizeof(struct drm_version);
    case DRM_NR_GET_UNIQUE:             return 2 * sizeof(size_t);  /* size_t, char * */
    case DRM_NR_GET_MAGIC:
    case DRM_NR_AUTH_MAGIC:             return sizeof(struct drm_auth);
    case DRM_NR_SET_VERSION:            return sizeof(struct drm_set_version);
    case DRM_NR_GEM_CLOSE:              return sizeof(struct drm_gem_close);
    case DRM_NR_GET_CAP:                return sizeof(struct drm_get_cap);
    case DRM_NR_SET_CLIENT_CAP:         return sizeof(struct drm_set_client_cap);
    case DRM_NR_SET_MASTER:
    case DRM_NR_DROP_MASTER:            return 1;   /* no argument */
    case DRM_NR_PRIME_HANDLE_TO_FD:
    case DRM_NR_PRIME_FD_TO_HANDLE:     return sizeof(struct drm_prime_handle);
    case DRM_NR_MODE_GETRESOURCES:      return sizeof(struct drm_mode_card_res);
    case DRM_NR_MODE_GETCRTC:
    case DRM_NR_MODE_SETCRTC:           return sizeof(struct drm_mode_crtc);
    case DRM_NR_MODE_CURSOR:
    case DRM_NR_MODE_CURSOR2:           return sizeof(struct drm_mode_cursor);
    case DRM_NR_MODE_GETENCODER:        return sizeof(struct drm_mode_get_encoder);
    case DRM_NR_MODE_GETCONNECTOR:      return sizeof(struct drm_mode_get_connector);
    case DRM_NR_MODE_GETPROPERTY:       return sizeof(struct drm_mode_get_property);
    case DRM_NR_MODE_SETPROPERTY:       return sizeof(struct drm_mode_connector_set_property);
    case DRM_NR_MODE_GETPROPBLOB:       return sizeof(struct drm_mode_get_blob);
    case DRM_NR_MODE_GETFB:
    case DRM_NR_MODE_ADDFB:             return sizeof(struct drm_mode_fb_cmd);
    case DRM_NR_MODE_RMFB:              return 4;
    case DRM_NR_MODE_PAGE_FLIP:         return sizeof(struct drm_mode_crtc_page_flip);
    case DRM_NR_MODE_DIRTYFB:           return sizeof(struct drm_mode_fb_dirty_cmd);
    case DRM_NR_MODE_CREATE_DUMB:       return sizeof(struct drm_mode_create_dumb);
    case DRM_NR_MODE_MAP_DUMB:          return sizeof(struct drm_mode_map_dumb);
    case DRM_NR_MODE_DESTROY_DUMB:      return sizeof(struct drm_mode_destroy_dumb);
    case DRM_NR_MODE_GETPLANERESOURCES: return sizeof(struct drm_mode_get_plane_res);
    case DRM_NR_MODE_GETPLANE:          return sizeof(struct drm_mode_get_plane);
    case DRM_NR_MODE_ADDFB2:            return sizeof(struct drm_mode_fb_cmd2);
    case DRM_NR_MODE_OBJ_GETPROPERTIES: return sizeof(struct drm_mode_obj_get_properties);
    case DRM_NR_MODE_OBJ_SETPROPERTY:   return sizeof(struct drm_mode_obj_set_property);
    case DRM_NR_MODE_LIST_LESSEES:      return sizeof(struct drm_mode_list_lessees);
    default:                            return 0;
    }
}

/* Every ioctl copies its argument in, works on the copy, and copies it
   back out: a copy can fault and sleep, so no state is held across one. */
union drm_arg {
    struct drm_version version;
    struct drm_auth auth;
    struct drm_set_version set_version;
    struct drm_gem_close gem_close;
    struct drm_get_cap cap;
    struct drm_set_client_cap client_cap;
    struct drm_prime_handle prime;
    struct drm_mode_card_res res;
    struct drm_mode_crtc crtc;
    struct drm_mode_cursor cursor;
    struct drm_mode_get_encoder encoder;
    struct drm_mode_get_connector connector;
    struct drm_mode_get_property property;
    struct drm_mode_connector_set_property set_property;
    struct drm_mode_get_blob blob;
    struct drm_mode_fb_cmd fb;
    struct drm_mode_fb_cmd2 fb2;
    struct drm_mode_crtc_page_flip flip;
    struct drm_mode_fb_dirty_cmd dirty;
    struct drm_mode_create_dumb create;
    struct drm_mode_map_dumb map;
    struct drm_mode_destroy_dumb destroy;
    struct drm_mode_get_plane_res plane_res;
    struct drm_mode_get_plane plane;
    struct drm_mode_obj_get_properties obj_props;
    struct drm_mode_obj_set_property obj_set;
    struct drm_mode_list_lessees lessees;
    uint32_t u32;
    size_t unique[2];
};

static int dirty_fb(struct drm_fbo *fb, const struct drm_mode_fb_dirty_cmd *d)
{
    /* RAM buffers on the screen: copy again (all of it; clips are a
       hint). A buffer in Bochs video memory is the screen already. */
    if (kms.active && kms.fb_id == d->fb_id && !fb->bo->vram)
        return show(fb, &kms.mode);
    return 0;
}

static int do_ioctl(struct drm_file *df, uint32_t nr, union drm_arg *a)
{
    int h;
    struct drm_bo *bo;
    struct drm_fbo *fb;
    switch (nr) {
    case DRM_NR_VERSION:
        return version(&a->version);
    case DRM_NR_GET_UNIQUE:
        a->unique[0] = 0;               /* no bus id */
        return 0;
    case DRM_NR_GET_MAGIC:
        if (!df->magic)
            df->magic = ++kms.magic_next;
        a->auth.magic = df->magic;
        return 0;
    case DRM_NR_AUTH_MAGIC:
        /* libdrm's drmIsMaster asks to authenticate magic 0: -EINVAL
           means "you are master", -EACCES "you are not". */
        if (!is_master(df))
            return -EACCES;
        for (struct drm_file *o = kms.files; o; o = o->next)
            if (a->auth.magic && o->magic == a->auth.magic) {
                o->authenticated = 1;
                return 0;
            }
        return -EINVAL;
    case DRM_NR_SET_VERSION:
        if (a->set_version.di_major != -1 &&
            (a->set_version.di_major != 1 || a->set_version.di_minor > 4))
            return -EINVAL;
        a->set_version = (struct drm_set_version){ 1, 4, 1, 0 };
        return 0;
    case DRM_NR_GET_CAP:
        return get_cap(&a->cap);
    case DRM_NR_SET_CLIENT_CAP:
        if (a->client_cap.capability == DRM_CLIENT_CAP_UNIVERSAL_PLANES) {
            df->universal_planes = a->client_cap.value != 0;
            return 0;
        }
        if (a->client_cap.capability == DRM_CLIENT_CAP_ASPECT_RATIO)
            return 0;
        return a->client_cap.capability == DRM_CLIENT_CAP_ATOMIC ? -EOPNOTSUPP : -EINVAL;
    case DRM_NR_SET_MASTER:
        if (kms.master && kms.master != df)
            return -EBUSY;
        kms.master = df;
        return 0;
    case DRM_NR_DROP_MASTER:
        if (!is_master(df))
            return -EINVAL;
        release_master(df);
        return 0;
    case DRM_NR_GEM_CLOSE:
        return handle_close(df, a->gem_close.handle);
    case DRM_NR_PRIME_HANDLE_TO_FD:
        return prime_export(df, &a->prime);
    case DRM_NR_PRIME_FD_TO_HANDLE:
        return prime_import(df, &a->prime);
    case DRM_NR_MODE_GETRESOURCES:
        return get_resources(&a->res);
    case DRM_NR_MODE_GETCRTC:
        return get_crtc(&a->crtc);
    case DRM_NR_MODE_SETCRTC:
        return is_master(df) ? set_crtc(&a->crtc) : -EACCES;
    case DRM_NR_MODE_CURSOR:
    case DRM_NR_MODE_CURSOR2:
        if (!is_master(df))
            return -EACCES;
        /* No cursor plane: "hide it" works, a cursor image doesn't, and
           wlroots then draws the pointer itself. */
        return (a->cursor.flags & 1) && a->cursor.handle ? -ENXIO : 0;
    case DRM_NR_MODE_GETENCODER:
        if (a->encoder.encoder_id != ID_ENCODER)
            return -ENOENT;
        a->encoder.encoder_type = DRM_MODE_ENCODER_NONE;
        a->encoder.crtc_id = ID_CRTC;
        a->encoder.possible_crtcs = 1;
        a->encoder.possible_clones = 0;
        return 0;
    case DRM_NR_MODE_GETCONNECTOR:
        return get_connector(&a->connector);
    case DRM_NR_MODE_GETPROPERTY:
        return get_property(&a->property);
    case DRM_NR_MODE_SETPROPERTY:
        if (!is_master(df))
            return -EACCES;
        return set_property(a->set_property.connector_id, a->set_property.prop_id,
                            a->set_property.value);
    case DRM_NR_MODE_OBJ_SETPROPERTY:
        if (!is_master(df))
            return -EACCES;
        return set_property(a->obj_set.obj_id, a->obj_set.prop_id, a->obj_set.value);
    case DRM_NR_MODE_OBJ_GETPROPERTIES:
        return obj_properties(df, &a->obj_props);
    case DRM_NR_MODE_GETPROPBLOB:
        return -ENOENT;                 /* no blobs */
    case DRM_NR_MODE_GETPLANERESOURCES: {
        static const uint32_t plane = ID_PLANE;
        int err = put_array(a->plane_res.plane_id_ptr, a->plane_res.count_planes, &plane, 1, 4);
        a->plane_res.count_planes = 1;
        return err;
    }
    case DRM_NR_MODE_GETPLANE:
        return get_plane(&a->plane);
    case DRM_NR_MODE_CREATE_DUMB: {
        struct drm_mode_create_dumb *c = &a->create;
        if (c->bpp != 32 || c->flags || !c->width || !c->height ||
            c->width > 4096 || c->height > 4096 ||
            ((c->width * 4 + 63) & ~63u) * c->height > BO_MAX_BYTES)
            return -EINVAL;
        bo = bo_new(c->width, c->height);
        if (!bo)
            return -ENOMEM;
        h = handle_new(df, bo);
        bo_put(bo);                     /* the handle holds it now */
        if (h < 0)
            return h;
        c->handle = (uint32_t)h;
        c->pitch = bo->pitch;
        c->size = bo->size;
        return 0;
    }
    case DRM_NR_MODE_MAP_DUMB:
        bo = handle_bo(df, a->map.handle);
        if (!bo)
            return -ENOENT;
        a->map.offset = (uint64_t)(bo_index(bo) + 1) * MAP_STRIDE;
        return 0;
    case DRM_NR_MODE_DESTROY_DUMB:
        return handle_close(df, a->destroy.handle);
    case DRM_NR_MODE_ADDFB: {
        struct drm_mode_fb_cmd *c = &a->fb;
        uint32_t format = c->depth == 32 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
        if (c->bpp != 32 || (c->depth != 24 && c->depth != 32))
            return -EINVAL;
        return fb_add(df, c->handle, c->width, c->height, c->pitch, 0, format, &c->fb_id);
    }
    case DRM_NR_MODE_ADDFB2: {
        struct drm_mode_fb_cmd2 *c = &a->fb2;
        if (c->flags & ~DRM_MODE_FB_MODIFIERS)
            return -EINVAL;
        if ((c->flags & DRM_MODE_FB_MODIFIERS) && c->modifier[0])
            return -EINVAL;             /* linear only */
        if (c->handles[1] || c->handles[2] || c->handles[3])
            return -EINVAL;             /* one plane of pixels */
        return fb_add(df, c->handles[0], c->width, c->height, c->pitches[0], c->offsets[0],
                      c->pixel_format, &c->fb_id);
    }
    case DRM_NR_MODE_GETFB:
        fb = fb_lookup(a->fb.fb_id);
        if (!fb)
            return -ENOENT;
        a->fb.width = fb->width;
        a->fb.height = fb->height;
        a->fb.pitch = fb->pitch;
        a->fb.bpp = 32;
        a->fb.depth = fb->format == DRM_FORMAT_ARGB8888 || fb->format == DRM_FORMAT_ABGR8888 ? 32 : 24;
        a->fb.handle = 0;
        if (is_master(df)) {            /* Linux: only master or root see the pixels */
            h = handle_new(df, fb->bo);
            a->fb.handle = h > 0 ? (uint32_t)h : 0;
        }
        return 0;
    case DRM_NR_MODE_RMFB:
        fb = fb_lookup(a->u32);
        if (!fb || fb->owner != df)
            return -ENOENT;
        fb_remove(fb);
        return 0;
    case DRM_NR_MODE_PAGE_FLIP:
        return is_master(df) ? page_flip(df, &a->flip) : -EACCES;
    case DRM_NR_MODE_DIRTYFB:
        fb = fb_lookup(a->dirty.fb_id);
        return fb ? dirty_fb(fb, &a->dirty) : -ENOENT;
    case DRM_NR_MODE_LIST_LESSEES:
        a->lessees.count_lessees = 0;   /* no leases */
        return 0;
    }
    return -EINVAL;
}

/* Which ioctls write their argument back. */
static int writes_back(uint32_t nr)
{
    switch (nr) {
    case DRM_NR_GEM_CLOSE: case DRM_NR_SET_CLIENT_CAP: case DRM_NR_AUTH_MAGIC:
    case DRM_NR_SET_MASTER: case DRM_NR_DROP_MASTER: case DRM_NR_MODE_SETCRTC:
    case DRM_NR_MODE_CURSOR: case DRM_NR_MODE_CURSOR2: case DRM_NR_MODE_SETPROPERTY:
    case DRM_NR_MODE_OBJ_SETPROPERTY: case DRM_NR_MODE_RMFB: case DRM_NR_MODE_PAGE_FLIP:
    case DRM_NR_MODE_DIRTYFB: case DRM_NR_MODE_DESTROY_DUMB:
        return 0;
    default:
        return 1;
    }
}

static int32_t drm_ioctl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    if (((cmd >> 8) & 0xFF) != DRM_IOCTL_BASE)
        return -ENOTTY;
    uint32_t nr = cmd & 0xFF, size = arg_size(nr);
    if (!size)
        return -EINVAL;                 /* atomic, leases, vblank waits, gamma... */
    union drm_arg a;
    memset(&a, 0, sizeof a);
    if (size > 1 && copy_from_user(&a, (const void *)arg, size) < 0)
        return -EFAULT;
    file_get(f);
    int r = do_ioctl(f->priv, nr, &a);
    file_put(f);
    if (r == 0 && size > 1 && writes_back(nr) && copy_to_user((void *)arg, &a, size) < 0)
        return -EFAULT;
    return r;
}

/* --- open, close, mmap --------------------------------------------------------- */

static int drm_open(struct file *f)
{
    if (!kms.ready)
        return -ENODEV;
    struct drm_file *df = kmalloc(sizeof *df);
    if (!df)
        return -ENOMEM;
    memset(df, 0, sizeof *df);
    if (!kms.master)
        kms.master = df;                /* the first opener, as in Linux */
    df->next = kms.files;
    kms.files = df;
    f->priv = df;
    return 0;
}

static void drm_release(struct file *f)
{
    struct drm_file *df = f->priv;
    for (struct drm_file **p = &kms.files; *p; p = &(*p)->next)
        if (*p == df) {
            *p = df->next;
            break;
        }
    for (uint32_t k = 0; k < MAX_FBS; k++)
        if (fbs[k].bo && fbs[k].owner == df)
            fb_remove(&fbs[k]);
    for (uint32_t h = 1; h <= MAX_HANDLES; h++)
        handle_close(df, h);
    release_master(df);
    kfree(df);
}

/* mmap at MAP_DUMB's offset: the buffer, if this file has a handle to it. */
static int drm_mmap(struct file *f, struct vma *vma)
{
    struct drm_file *df = f->priv;
    uint32_t k = vma->file_off / MAP_STRIDE;
    if (k == 0 || k > MAX_BOS || !bos[k - 1].refs)
        return -EINVAL;
    struct drm_bo *bo = &bos[k - 1];
    for (uint32_t i = 0; i < MAX_HANDLES; i++)
        if (df->handles[i] == bo)
            return bo_mmap(bo, k * MAP_STRIDE, vma);
    return -EACCES;
}

static const struct file_ops drm_ops = {
    .read = drm_read, .ioctl = drm_ioctl, .poll = drm_poll,
    .mmap = drm_mmap, .open = drm_open, .release = drm_release,
};

const struct file_ops *drm_file_ops(void)
{
    return &drm_ops;
}

uint32_t drm_pool_pages(void)
{
    return ram_pages;
}

int drm_card0(struct pci_dev **pdev)
{
    *pdev = kms.dev ? kms.dev->pdev : NULL;
    return kms.ready;
}

/* After drm_init: a screen this can drive? Then /dev/dri/card0. */
void drm_uapi_init(void)
{
    if (drm_primary && drm_primary->pipe) {
        kms.dev = drm_primary;
        kms.format = DRM_FORMAT_XRGB8888;
        kms.name = drm_primary->driver->name;
    } else {
        struct fb_info *info = fb_get(0);
        if (!info || info->var.bits_per_pixel != 32)
            return;
        kms.info = info;
        kms.format = info->var.red.offset == 0 ? DRM_FORMAT_XBGR8888 : DRM_FORMAT_XRGB8888;
        kms.name = "simpledrm";
        struct drm_display_mode *m = &kms.fixed;
        m->hdisplay = m->hsync_start = m->hsync_end = m->htotal = (uint16_t)info->var.xres;
        m->vdisplay = m->vsync_start = m->vsync_end = m->vtotal = (uint16_t)info->var.yres;
        m->clock = (int)(info->var.xres * info->var.yres * 60 / 1000);
        m->type = DRM_MODE_TYPE_PREFERRED;
        drm_mode_set_name(m);
    }
    kms.dpms = DRM_MODE_DPMS_ON;
    kms.ready = 1;
    dev_make_node("dri/card0", (226 << 8) | 0);
}
