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
 *   5,0  tty       the terminal of the process  } both the console here
 *   5,1  console   the system console           }
 *   4,n  ttyN      virtual terminals: the console too, for now
 *  29,0  fb0       the screen's pixels (drivers/video/fbmem.c), once a
 *                  display driver has registered one
 */
#include <errno.h>
#include <kprintf.h>
#include <linux/fb.h>
#include <serial.h>
#include <string.h>
#include <tty.h>
#include <uaccess.h>
#include <vfs.h>


#define DEV(major, minor) (((major) << 8) | (minor))

/* --- the console -------------------------------------------------------------- */

/* Writing to the console. Never waits for the slow serial port: if its
   queue is nearly full we write what fits and return a smaller count (a
   "short write" — POSIX allows it, and C libraries simply call write()
   again). So the syscall always finishes quickly. */
static int32_t console_write_user(struct file *f, const void *user_buf, uint32_t count)
{
    (void)f;
    char chunk[256];                /* copy in pieces via the stack */
    uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done < sizeof chunk ? count - done : sizeof chunk;
        uint32_t room = serial_space() / 2;     /* worst case: all '\n' */
        if (room < n)
            n = room;
        if (n == 0)
            break;                              /* queue full: short write */
        if (copy_from_user(chunk, (const char *)user_buf + done, n) < 0)
            return done ? (int32_t)done : -EFAULT;
        console_write(chunk, n);
        done += n;
    }
    return done;
}

/* Reading goes through the terminal's line editing (tty.c). */
const struct file_ops console_ops = {
    .read = tty_read, .write = console_write_user, .ioctl = tty_ioctl, .poll = tty_poll,
};

static struct inode console_inode = {
    .ino = 0, .mode = S_IFCHR | 0620, .nlink = 1, .rdev = DEV(5, 1),
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

static const struct file_ops null_ops = { .read = null_read, .write = sink_write };
static const struct file_ops zero_ops = { .read = zero_read, .write = sink_write };

/* --- device nodes ---------------------------------------------------------------- */

void dev_attach(struct inode *i)
{
    switch (i->rdev) {
    case DEV(1, 3): i->ops = &null_ops;    break;
    case DEV(1, 5): i->ops = &zero_ops;    break;
    case DEV(5, 0):
    case DEV(5, 1): i->ops = &console_ops; break;
    case DEV(29, 0): i->ops = fb_file_ops(); break;     /* fb0 (NULL: no screen) */
    default:
        if ((i->rdev >> 8) == 4)
            i->ops = &console_ops;          /* tty0, tty1, ... */
        break;                              /* unknown: open() fails */
    }
}

static void make_node(struct inode *dev, const char *name, uint32_t rdev)
{
    uint32_t len = strlen(name);
    if (vfs_child(dev, name, len))
        return;                             /* the initramfs has it already */
    struct inode *i = vfs_add(dev, name, len, S_IFCHR | 0666);
    if (i) {
        i->rdev = rdev;
        dev_attach(i);
    }
}

void dev_make_node(const char *name, uint32_t rdev)
{
    struct inode *dev = vfs_child(vfs_root(), "dev", 3);
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
    make_node(dev, "null", DEV(1, 3));
    make_node(dev, "zero", DEV(1, 5));
    make_node(dev, "tty", DEV(5, 0));
    make_node(dev, "console", DEV(5, 1));
}
