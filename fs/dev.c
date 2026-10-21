/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * dev.c — device files: /dev/null, /dev/zero, /dev/console, /dev/tty
 *
 * In Unix, devices are files: a program opens /dev/null and writes to it
 * like to any file. Each device node has a number, (major, minor), that says
 * which driver handles it — Linux's numbers, so an initramfs built for Linux
 * works here too:
 *
 *   1,3  null      reads: end of file; writes: thrown away
 *   1,5  zero      reads: zero bytes; writes: thrown away
 *   1,7  full      reads: zero bytes; writes: ENOSPC (a full disk)
 *   1,8  random    } reads: random bytes (kernel/random.c's ChaCha20, the
 *   1,9  urandom   } same as getrandom()); writes: stirred into the pool
 *   5,0  tty       the terminal of the process  } both the console here
 *   5,1  console   the system console           }
 *   5,2  ptmx      a new pseudo-terminal pair (pty.c); its slave is
 * 136,n  pts/n     ...this, made and removed by pty.c
 *   4,n  ttyN      virtual terminals: the console too, for now
 *  13,64 input/event0   the keyboard as input events (evdev.c)
 *  13,65 input/event1   the mouse
 *  13,66 input/event2   touch screens and tablets
 *  29,0  fb0       the screen's pixels (drivers/video/fbmem.c), once a
 *                  display driver has registered one
 * 226,0  dri/card0 the same screen through DRM/KMS, for Wayland
 *                  (drivers/gpu/drm/drm_ioctl.c)
 * 116,n  snd/...   sound cards (drivers/sound/core.c): controlC0, pcmC0D0p...
 */
#include <bcache.h>
#include <blockdev.h>
#include <drm/drm_drv.h>
#include <errno.h>
#include <evdev.h>
#include <interrupts.h>
#include <kprintf.h>
#include <kheap.h>
#include <linux/fb.h>
#include <partition.h>
#include <proc.h>
#include <pty.h>
#include <random.h>
#include <serial.h>
#include <sound/core.h>
#include <string.h>
#include <tty.h>
#include <uaccess.h>
#include <vfs.h>


#define DEV(major, minor) (((major) << 8) | (minor))

/* --- the console -------------------------------------------------------------- */

/* Writing to the console. Never waits for the slow serial port: if its
   queue is nearly full we write what fits and return a smaller count (a
   "short write" — POSIX allows it, and C libraries simply call write()
   again). So the syscall always finishes quickly.
   Screen and serial port both, except /dev/ttyS0 (the serial port alone)
   and /dev/console once TIOCCONS pointed it at ttyS0: then the screen
   shows only the boot's steps and what is written to tty0-7. */
static int32_t console_write_user(struct file *f, const void *user_buf, uint32_t count)
{
    char chunk[256];                /* copy in pieces via the stack */
    uint32_t done = 0;
    int32_t bg = tty_write_check(NULL);     /* a background job, `stty tostop` */
    if (bg)
        return bg;
    uint32_t rdev = f->inode->rdev;
    int screen = rdev != TTYS0_RDEV && !(rdev == DEV(5, 1) && tty_console_on_serial());
    while (done < count) {
        uint32_t n = count - done < sizeof chunk ? count - done : sizeof chunk;
        uint32_t room = serial_space() / 2;     /* worst case: all '\n' */
        if (room < n)
            n = room;
        if (n == 0)
            break;                              /* queue full: short write */
        if (file_copy_in(f, chunk, (const char *)user_buf + done, n) < 0)
            return done ? (int32_t)done : -EFAULT;
        if (screen)
            console_write(chunk, n);
        else
            serial_write_n(chunk, n);
        done += n;
    }
    return done;
}

/* Reading goes through the terminal's line editing (tty.c). */
const struct file_ops console_ops = {
    .read = tty_read, .write = console_write_user, .ioctl = tty_ioctl, .poll = tty_poll,
    .kbuf_ok = 1,
};

/* /dev/tty: "my terminal". In a terminal window that's the window's pty
   (the session's controlling terminal); otherwise the console. */
static int devtty_open(struct file *f)
{
    pty_open_ctty(f);
    return 0;
}

static const struct file_ops devtty_ops = {
    .read = tty_read, .write = console_write_user, .ioctl = tty_ioctl, .poll = tty_poll,
    .kbuf_ok = 1,
    .open = devtty_open,
};

static struct inode console_inode = {
    .ino = 0, .mode = S_IFCHR | 0600, .nlink = 1, .rdev = DEV(5, 1),
    .name = "console", .name_len = 7, .ops = &console_ops,
};

/* One shared open console for every program's fds 0-2; never freed. */
struct file console_file = {
    .ops = &console_ops, .inode = &console_inode, .flags = O_RDWR, .refs = 1,
};

/* --- null and zero ------------------------------------------------------------ */

static int32_t null_read(struct file *f, void *user_buf, uint32_t n)
{
    (void)f; (void)user_buf; (void)n;
    return 0;
}

static int32_t sink_write(struct file *f, const void *user_buf, uint32_t n)
{
    (void)f; (void)user_buf;
    return n;                       /* swallowed */
}

static int32_t zero_read(struct file *f, void *user_buf, uint32_t n)
{
    (void)f;
    static const uint8_t zeros[256];
    /* The same cap the real filesystems use (ramfs.c): a syscall runs with
       interrupts off, so without it read(fd, buf, 0x7fffffff) holds the CPU
       for two million copies and the 1 ms slot is long gone. A short count is
       what read() is allowed to return, and C libraries loop on it. */
    if (n > RW_MAX)
        n = RW_MAX;
    uint32_t done = 0;
    while (done < n) {
        uint32_t k = n - done < sizeof zeros ? n - done : sizeof zeros;
        if (copy_to_user((uint8_t *)user_buf + done, zeros, k) < 0)
            return done ? (int32_t)done : -EFAULT;
        done += k;
    }
    return done;
}

/* /dev/random and /dev/urandom: one source, always ready, never blocks
   (as Linux since 5.6 once it is seeded). Capped like zero_read, and the
   bytes are wiped from the stack after each copy. */
static int32_t random_read(struct file *f, void *user_buf, uint32_t n)
{
    (void)f;
    if (n > RW_MAX)
        n = RW_MAX;
    uint8_t buf[64];
    uint32_t done = 0;
    while (done < n) {
        uint32_t k = n - done < sizeof buf ? n - done : sizeof buf;
        random_bytes(buf, k);
        int rc = copy_to_user((uint8_t *)user_buf + done, buf, k);
        memset(buf, 0, k);
        if (rc < 0)
            return done ? (int32_t)done : -EFAULT;
        done += k;
    }
    return done;
}

/* Writing stirs the bytes in (no entropy credited — there is no count). */
static int32_t random_write(struct file *f, const void *user_buf, uint32_t n)
{
    (void)f;
    if (n > RW_MAX)
        n = RW_MAX;
    uint8_t buf[64];
    uint32_t done = 0;
    while (done < n) {
        uint32_t k = n - done < sizeof buf ? n - done : sizeof buf;
        if (copy_from_user(buf, (const uint8_t *)user_buf + done, k) < 0)
            return done ? (int32_t)done : -EFAULT;
        random_add(buf, k);
        done += k;
    }
    return done;
}

static const struct file_ops random_ops = { .read = random_read, .write = random_write };
static const struct file_ops null_ops = { .read = null_read, .write = sink_write, .kbuf_ok = 1 };
static const struct file_ops zero_ops = { .read = zero_read, .write = sink_write, .kbuf_ok = 1 };

/* /dev/full: reads like zero, every write fails "no space left" — for
   testing what a program does with a full disk. */
static int32_t full_write(struct file *f, const void *user_buf, uint32_t n)
{
    (void)f, (void)user_buf;
    return n ? -ENOSPC : 0;
}

static const struct file_ops full_ops = { .read = zero_read, .write = full_write, .kbuf_ok = 1 };

/* --- disks: /dev/sda, /dev/sda2, ... -------------------------------------------- */
/*
 * A disk or a partition as one big file: what sfdisk, mke2fs and the
 * installer read and write. Linux's numbers (blockdev_devnum), owner root,
 * group disk (6), mode 0660 — the live user is not in that group.
 *
 * Positions go past 4 GiB and struct file's pos is 32 bits, so each open
 * keeps its own 64-bit position (struct blkfile in f->priv); lseek and
 * pread/pwrite come here for it (syscall.c). Bytes go through a bounce
 * buffer in whole sectors — a partial sector is read, changed, written
 * back — BLK_CHUNK at a time, but a call does the whole request: disk
 * tools (mke2fs) treat a short write as an error.
 *
 * The disk under a mounted file system can't be opened for writing
 * (EBUSY), and one open for writing can't be mounted: the file system's
 * cache and the raw writes would undo each other — and the installer can
 * never format the system it runs on. The block cache forgets the disk
 * when a writer opens it and when it closes, so a file system mounted
 * afterwards reads what was written, not an old cached copy.
 */
#define BLK_CHUNK   (64 * 1024)
#define GID_DISK    6

struct blkfile {
    struct blockdev *d;
    uint64_t pos;
    uint8_t writer;                     /* opened for writing */
};

static uint8_t writers[BLOCKDEV_MAX];  /* opens for writing, per whole disk */
static struct inode *dev_dir;          /* the RAM /dev (still /dev after root moves) */

static struct blockdev *disk_of(struct blockdev *d) { return d->parent ? d->parent : d; }

static int disk_index(struct blockdev *disk)
{
    for (int k = 0; k < blockdev_count(); k++)
        if (blockdev_get(k) == disk)
            return k;
    return 0;
}

/* Is d, its whole disk, or (for a whole disk) any of its partitions mounted? */
static int blk_mounted(struct blockdev *d)
{
    for (int k = 0; k < vfs_mount_count(); k++) {
        struct mount *m = vfs_mount_at(k);
        if (!m || strncmp(m->source, "/dev/", 5))
            continue;
        for (int j = 0; j < blockdev_count(); j++) {
            struct blockdev *x = blockdev_get(j);
            if (strcmp(x->name, m->source + 5) || disk_of(x) != disk_of(d))
                continue;
            if (x == d || !x->parent || !d->parent)
                return 1;
        }
    }
    return 0;
}

int blkdev_busy(struct blockdev *d)    /* sys_mount: a raw writer has it */
{
    return writers[disk_index(disk_of(d))] != 0;
}

/* The node's own mode bits, checked here because the VFS doesn't check
   any yet (WORKLIST B30): a raw disk is every file on it, so only root
   and the disk group may open one. */
static int blk_allowed(const struct inode *i, uint32_t accmode)
{
    struct process *p = proc_current();
    if (!p || p->cred.euid == 0)
        return 1;
    uint32_t need = (accmode != O_WRONLY ? 4 : 0) | (accmode != O_RDONLY ? 2 : 0);
    uint32_t bits = i->mode & 7;                                    /* others */
    if (p->cred.euid == i->uid)
        bits = i->mode >> 6 & 7;
    else {
        int member = p->cred.egid == i->gid;
        for (uint32_t k = 0; k < p->cred.ngroups && !member; k++)
            member = p->cred.groups[k] == i->gid;
        if (member)
            bits = i->mode >> 3 & 7;
    }
    return (bits & need) == need;
}

static int blk_open(struct file *f)
{
    struct blockdev *d = blockdev_by_devnum(f->inode->rdev >> 8, f->inode->rdev & 0xFF);
    if (!d)
        return -ENXIO;
    int writer = (f->flags & O_ACCMODE) != O_RDONLY;
    if (!blk_allowed(f->inode, f->flags & O_ACCMODE))
        return -EACCES;
    struct blkfile *b = kmalloc(sizeof *b);
    if (!b)
        return -ENOMEM;
    if (writer) {
        if (blk_mounted(d)) {
            kfree(b);
            return -EBUSY;
        }
        writers[disk_index(disk_of(d))]++;
        bcache_drop(disk_of(d));
    }
    *b = (struct blkfile){ d, 0, (uint8_t)writer };
    f->priv = b;
    return 0;
}

static void blk_release(struct file *f)
{
    struct blkfile *b = f->priv;
    if (!b)
        return;
    if (b->writer) {
        blockdev_flush(b->d);           /* the drive's own write cache */
        bcache_drop(disk_of(b->d));
        writers[disk_index(disk_of(b->d))]--;
    }
    kfree(b);
}

/* One piece of a disk read or write: at most BLK_CHUNK, through the bounce
   buffer. At `at`, or at (and moving) the open's own position if -1. */
static int32_t blk_chunk(struct file *f, uintptr_t ubuf, uint32_t n, int writing, int64_t at)
{
    struct blkfile *b = f->priv;
    if (!b || !b->d->sectors)
        return -ENXIO;                  /* the partition went away */
    uint64_t pos = at >= 0 ? (uint64_t)at : b->pos, size = (uint64_t)b->d->sectors * 512;
    if (pos >= size)
        return writing && n ? -ENOSPC : 0;
    if (n > size - pos)
        n = (uint32_t)(size - pos);
    if (n > BLK_CHUNK)
        n = BLK_CHUNK;
    if (!n)
        return 0;
    uint32_t off = (uint32_t)(pos % 512), lba = (uint32_t)(pos / 512);
    uint32_t count = (off + n + 511) / 512;
    uint8_t *buf = kmalloc_contig(count * 512);    /* drivers DMA into it */
    if (!buf)
        return -ENOMEM;
    int32_t r = (int32_t)n;
    if (!writing) {
        if (blockdev_read(b->d, lba, count, buf) < 0)
            r = -EIO;
        else if (copy_to_user((void *)ubuf, buf + off, n) < 0)
            r = -EFAULT;
    } else {
        /* The sectors only partly written keep the rest of their bytes. */
        uint32_t tail = (off + n) % 512;
        if ((off && blockdev_read(b->d, lba, 1, buf) < 0) ||
            (tail && (count > 1 || !off) &&
             blockdev_read(b->d, lba + count - 1, 1, buf + (count - 1) * 512) < 0))
            r = -EIO;
        else if (copy_from_user(buf + off, (const void *)ubuf, n) < 0)
            r = -EFAULT;
        else if (blockdev_write(b->d, lba, count, buf) < 0)
            r = -EIO;
    }
    kfree(buf);
    if (r > 0 && at < 0)
        b->pos = pos + (uint32_t)r;
    return r;
}

/* The whole request, piece by piece: a disk's read or write is never short
   unless it reaches the end or fails (Linux's rule, which disk tools count
   on). */
int32_t blkfile_rw(struct file *f, uintptr_t ubuf, uint32_t n, int writing, int64_t at)
{
    uint32_t done = 0;
    while (done < n) {
        int32_t r = blk_chunk(f, ubuf + done, n - done, writing, at >= 0 ? at + done : -1);
        if (r <= 0)
            return done ? (int32_t)done : r;
        done += (uint32_t)r;
    }
    return (int32_t)done;
}

static int32_t blk_read(struct file *f, void *ubuf, uint32_t n)
{
    return blkfile_rw(f, (uintptr_t)ubuf, n, 0, -1);
}

static int32_t blk_write(struct file *f, const void *ubuf, uint32_t n)
{
    return blkfile_rw(f, (uintptr_t)ubuf, n, 1, -1);
}

int64_t blkfile_seek(struct file *f, int64_t off, uint32_t whence)
{
    struct blkfile *b = f->priv;
    if (!b)
        return -EINVAL;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)b->pos       /* SEEK_SET, CUR */
                 : whence == 2 ? (int64_t)b->d->sectors * 512 : -1;     /* SEEK_END */
    if (base < 0 || base + off < 0)
        return -EINVAL;
    b->pos = (uint64_t)(base + off);
    return base + off;
}

/* The block ioctls disk tools use (linux/fs.h, linux/hdreg.h). */
#define BLKROGET         0x125E
#define BLKRRPART        0x125F
#define BLKGETSIZE       0x1260
#define BLKFLSBUF        0x1261
#define BLKSSZGET        0x1268
#define BLKIOMIN         0x1278
#define BLKIOOPT         0x1279
#define BLKALIGNOFF      0x127A
#define BLKPBSZGET       0x127B
#define BLKDISCARDZEROES 0x127C
#define BLKGETSIZE64     0x80001272u    /* _IOR(0x12, 114, size_t): its size bits vary */
#define HDIO_GETGEO      0x0301

static int32_t put_int(uintptr_t arg, int32_t v)
{
    return copy_to_user((void *)arg, &v, sizeof v) < 0 ? -EFAULT : 0;
}

static int32_t blk_ioctl(struct file *f, uint32_t cmd, uintptr_t arg)
{
    struct blkfile *b = f->priv;
    struct blockdev *d = b ? b->d : NULL;
    if (!d || !d->sectors)
        return -ENXIO;
    if ((cmd & ~0x3FFF0000u) == BLKGETSIZE64) {
        uint64_t bytes = (uint64_t)d->sectors * 512;
        return copy_to_user((void *)arg, &bytes, 8) < 0 ? -EFAULT : 0;
    }
    switch (cmd) {
    case BLKGETSIZE: {
        unsigned long s = d->sectors;
        return copy_to_user((void *)arg, &s, sizeof s) < 0 ? -EFAULT : 0;
    }
    case BLKSSZGET: case BLKPBSZGET: case BLKIOMIN:
        return put_int(arg, 512);
    case BLKIOOPT: case BLKALIGNOFF: case BLKROGET: case BLKDISCARDZEROES:
        return put_int(arg, 0);
    case BLKFLSBUF:
        if (proc_current() && proc_current()->cred.euid != 0)
            return -EACCES;
        blockdev_flush(d);
        bcache_drop(disk_of(d));
        return 0;
    case BLKRRPART:
        if (proc_current() && proc_current()->cred.euid != 0)
            return -EACCES;
        if (d->parent)
            return -EINVAL;
        if (blk_mounted(d))
            return -EBUSY;              /* Linux too: not under a mounted file system */
        return partition_rescan(d);
    case HDIO_GETGEO: {                 /* the made-up geometry every disk has now */
        struct {
            uint8_t heads, sectors;
            uint16_t cylinders;
            unsigned long start;
        } g = { 255, 63, 0, d->parent ? d->offset : 0 };
        uint32_t cyl = d->sectors / (255 * 63);
        g.cylinders = (uint16_t)(cyl > 65535 ? 65535 : cyl);
        return copy_to_user((void *)arg, &g, sizeof g) < 0 ? -EFAULT : 0;
    }
    }
    return -ENOTTY;
}

static const struct file_ops blk_ops = {
    .open = blk_open, .release = blk_release, .read = blk_read, .write = blk_write,
    .ioctl = blk_ioctl,
};

void dev_block_add(struct blockdev *d)
{
    if (!dev_dir || !d->sectors)
        return;
    unsigned ma, mi;
    blockdev_devnum(d, &ma, &mi);
    uint32_t len = (uint32_t)strlen(d->name), flags = irq_save();
    struct inode *old = vfs_child(dev_dir, d->name, len);
    if (old && S_ISBLK(old->mode)) {   /* the initramfs or mknod made it */
        old->rdev = DEV(ma, mi);
        old->ops = &blk_ops;
    }
    char *name = old ? NULL : kmalloc(len);
    struct inode *i = name ? vfs_add(dev_dir, (memcpy(name, d->name, len), name), len,
                                     S_IFBLK | 0660) : NULL;
    if (i) {
        i->flags |= I_NAME_OWNED;
        i->rdev = DEV(ma, mi);
        i->gid = GID_DISK;
        i->ops = &blk_ops;
    } else
        kfree(name);
    irq_restore(flags);
}

void dev_block_remove(struct blockdev *d)
{
    if (!dev_dir)
        return;
    uint32_t flags = irq_save();
    struct inode *i = vfs_child(dev_dir, d->name, (uint32_t)strlen(d->name));
    if (i && S_ISBLK(i->mode))
        vfs_remove(i);
    irq_restore(flags);
}

/* --- device nodes ---------------------------------------------------------------- */

void dev_attach(struct inode *i)
{
    if (S_ISBLK(i->mode)) {
        i->ops = &blk_ops;              /* open() finds the disk by number */
        return;
    }
    switch (i->rdev) {
    case DEV(1, 3): i->ops = &null_ops;    break;
    case DEV(1, 5): i->ops = &zero_ops;    break;
    case DEV(1, 7): i->ops = &full_ops;    break;
    case DEV(1, 8):
    case DEV(1, 9): i->ops = &random_ops;  break;
    case DEV(5, 0): i->ops = &devtty_ops;  break;
    case DEV(5, 1): i->ops = &console_ops; break;
    case DEV(5, 2): i->ops = pty_master_ops(); break;
    case DEV(29, 0): i->ops = fb_file_ops(); break;     /* fb0 (NULL: no screen) */
    default:
        if ((i->rdev >> 8) == 4)
            i->ops = &console_ops;          /* tty0, tty1, ... */
        else if ((i->rdev >> 8) == PTY_SLAVE_MAJOR)
            i->ops = pty_slave_ops();       /* pts/N: open() checks N */
        else if ((i->rdev >> 8) == 13)
            i->ops = evdev_ops(i->rdev & 0xFF);     /* input/event0-2 */
        else if ((i->rdev >> 8) == 116)
            i->ops = snd_file_ops(i->rdev & 0xFF);  /* snd/controlC0, pcmC0D0p... */
        else if (i->rdev == DEV(226, 0))
            i->ops = drm_file_ops();                /* dri/card0 (open: -ENODEV if no screen) */
        break;                              /* unknown: open() fails */
    }
}

/* Who may open a node the kernel makes. Linux's devtmpfs makes nodes
   0600 root and udev opens them up; with no udev here, the kernel gives
   the final modes, Alpine's (mdev.conf, and its /etc/group numbers):
   the keyboard, mouse and screen only to their groups (the X server is
   root; the desktop's user is in them), the virtual terminals to tty,
   the console to root alone. Everything else here (null, zero, random,
   tty, ptmx) is everyone's. Sound sets its own (drivers/sound/core.c). */
#define GID_TTY     5
#define GID_INPUT   23
#define GID_VIDEO   27

static uint32_t node_mode(uint32_t rdev, uint32_t *gid)
{
    *gid = 0;
    switch (rdev >> 8) {
    case 4:  *gid = GID_TTY;   return 0620;     /* tty0..tty7 */
    case 5:  return (rdev & 0xFF) == 1 ? 0600 : 0666;  /* console; tty, ptmx */
    case 13: *gid = GID_INPUT; return 0660;     /* input/event* */
    case 29:                                    /* fb0 */
    case 226: *gid = GID_VIDEO; return 0660;    /* dri/card0 */
    default: return 0666;
    }
}

static void make_node(struct inode *dev, const char *name, uint32_t rdev)
{
    uint32_t len = strlen(name), gid;
    if (vfs_child(dev, name, len))
        return;                             /* the initramfs has it already */
    struct inode *i = vfs_add(dev, name, len, S_IFCHR | node_mode(rdev, &gid));
    if (i) {
        i->rdev = rdev;
        i->gid = gid;
        dev_attach(i);
    }
}

void dev_make_node(const char *name, uint32_t rdev)
{
    struct inode *dev = vfs_child(vfs_root(), "dev", 3);
    const char *slash = strchr(name, '/');
    if (dev && slash) {                     /* "dri/card0": one directory deep */
        uint32_t len = (uint32_t)(slash - name);
        struct inode *sub = vfs_child(dev, name, len);
        dev = sub ? sub : vfs_add(dev, name, len, S_IFDIR | 0755);
        name = slash + 1;
    }
    if (!dev)
        return;
    struct inode *i = vfs_child(dev, name, strlen(name));
    if (i && i->rdev == rdev)
        dev_attach(i);                      /* the initramfs has it: give it ops */
    else
        make_node(dev, name, rdev);
}

void dev_init(void)
{
    struct inode *dev = vfs_child(vfs_root(), "dev", 3);
    if (!dev)
        dev = vfs_add(vfs_root(), "dev", 3, S_IFDIR | 0755);
    if (!dev)
        return;
    dev_dir = dev;
    make_node(dev, "null", DEV(1, 3));
    make_node(dev, "zero", DEV(1, 5));
    make_node(dev, "full", DEV(1, 7));
    make_node(dev, "random", DEV(1, 8));
    make_node(dev, "urandom", DEV(1, 9));
    make_node(dev, "tty", DEV(5, 0));
    make_node(dev, "console", DEV(5, 1));
    make_node(dev, "ptmx", DEV(5, 2));
    pty_init(dev);
    /* Linux's links into /proc/self/fd: scripts read /dev/stdin, and bash's
       <(command) hands out /dev/fd/N. They resolve when used, so /proc can
       be mounted later. */
    static const char *const links[][2] = {
        { "fd", "/proc/self/fd" },        { "stdin", "/proc/self/fd/0" },
        { "stdout", "/proc/self/fd/1" },  { "stderr", "/proc/self/fd/2" },
    };
    for (uint32_t k = 0; k < sizeof links / sizeof links[0]; k++) {
        uint32_t len = strlen(links[k][0]);
        struct inode *l = vfs_child(dev, links[k][0], len) ? NULL
                        : vfs_add(dev, links[k][0], len, S_IFLNK | 0777);
        if (l) {                        /* (before any thread: vfs_symlink can't run yet) */
            l->data = (const uint8_t *)links[k][1];     /* a constant: not copied */
            l->size = strlen(links[k][1]);
        }
    }
    /* Virtual terminals: one console behind them all, for now. An X
       server opens tty0 to ask for a free one, then ttyN. */
    static const char tty_names[8][5] = {
        "tty0", "tty1", "tty2", "tty3", "tty4", "tty5", "tty6", "tty7" };
    for (int n = 0; n < 8; n++)
        make_node(dev, tty_names[n], DEV(4, n));
    make_node(dev, "ttyS0", TTYS0_RDEV);
    struct inode *input = vfs_child(dev, "input", 5);
    if (!input)
        input = vfs_add(dev, "input", 5, S_IFDIR | 0755);
    if (input) {
        make_node(input, "event0", DEV(13, 64));    /* keyboard (evdev.c) */
        make_node(input, "event1", DEV(13, 65));    /* mouse */
        make_node(input, "event2", DEV(13, 66));    /* touch screens, tablets */
    }
    for (int k = 0; k < blockdev_count(); k++)     /* disks found before /dev */
        dev_block_add(blockdev_get(k));
}
