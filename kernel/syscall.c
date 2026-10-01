/*
 * syscall.c — the Linux system calls, and the table that finds them
 *
 * A user program can't call kernel functions directly (it's in ring 3 and
 * kernel pages are off-limits). Instead it puts a number in EAX and enters
 * the kernel (`int 0x80` or `sysenter`). We look the number up in
 * syscall_table[] — one array index, no searching.
 *
 * Rule #1 of syscalls: never trust a pointer from user space. A program can
 * pass any number as a "buffer" — including a kernel address, hoping we'll
 * read or write kernel memory for it. All user memory is touched only through
 * copy_from_user() & co. (uaccess.h): a range check, then a copy that turns
 * a fault into -EFAULT.
 */
#include <kprintf.h>
#include <serial.h>
#include <string.h>
#include <syscall.h>
#include <thread.h>
#include <uaccess.h>
#include <vfs.h>

/* Every handler gets the six argument registers; most use fewer. */
#define U __attribute__((unused))
#define SYSCALL(name) \
    static int32_t name(uint32_t a1 U, uint32_t a2 U, uint32_t a3 U, \
                        uint32_t a4 U, uint32_t a5 U, uint32_t a6 U)

typedef int32_t (*syscall_fn)(uint32_t, uint32_t, uint32_t,
                              uint32_t, uint32_t, uint32_t);

/* --- the console ------------------------------------------------------------ */

/* Writing to the console (fds 0-2). Never waits for the slow serial port:
   if its queue is nearly full we write what fits and return a smaller count
   (a "short write" — POSIX allows it, and C libraries simply call write()
   again). So the syscall always finishes quickly. */
int32_t console_write_user(struct file *f, const void *user_buf, uint32_t count)
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

/* --- files ------------------------------------------------------------------ */

/* ssize_t read(int fd, void *buf, size_t count) */
SYSCALL(sys_read)
{
    struct file *f = fd_get(a1);
    if (!f)
        return -EBADF;
    if (!user_range_ok(a2, a3))
        return -EFAULT;
    return f->ops->read(f, (void *)a2, a3);
}

/* ssize_t write(int fd, const void *buf, size_t count) */
SYSCALL(sys_write)
{
    struct file *f = fd_get(a1);
    if (!f)
        return -EBADF;
    if (!user_range_ok(a2, a3))
        return -EFAULT;
    return f->ops->write(f, (const void *)a2, a3);
}

/* Copy a path from user memory. 0, or -errno. */
static int get_path(char *dst, uint32_t user_path)
{
    long n = strncpy_from_user(dst, (const char *)user_path, PATH_MAX);
    if (n < 0)
        return n;
    return n == 0 ? -ENOENT : 0;    /* "" is not a file name */
}

static int32_t do_open(uint32_t user_path, uint32_t flags)
{
    char path[PATH_MAX];
    int err = get_path(path, user_path);
    if (err)
        return err;
    struct file *f;
    err = vfs_open(path, flags, &f);
    if (err)
        return err;
    int fd = fd_install(f);
    if (fd < 0)
        file_put(f);
    return fd;
}

/* int open(const char *path, int flags, mode_t mode) */
SYSCALL(sys_open)
{
    return do_open(a1, a2);
}

/* int openat(int dirfd, const char *path, int flags, mode_t mode)
   Only AT_FDCWD or absolute paths for now (there's no chdir yet). */
SYSCALL(sys_openat)
{
    if ((int32_t)a1 != AT_FDCWD) {
        char c;
        if (strncpy_from_user(&c, (const char *)a2, 1) < 0 || c != '/')
            return -ENOSYS;         /* relative to a directory fd: not yet */
    }
    return do_open(a2, a3);
}

/* int close(int fd) */
SYSCALL(sys_close)
{
    return fd_close(a1);
}

/* Shared by lseek and _llseek: the new position, or -errno. */
static int64_t do_seek(uint32_t fd, int64_t off, uint32_t whence)
{
    struct file *f = fd_get(fd);
    if (!f)
        return -EBADF;
    if (S_ISCHR(f->inode->mode))
        return -ESPIPE;             /* the console has no position */
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->pos
                 : whence == SEEK_END ? (int64_t)f->inode->size : -1;
    if (base < 0)
        return -EINVAL;
    int64_t pos = base + off;
    if (pos < 0 || pos > 0xFFFFFFFFll)
        return -EINVAL;
    f->pos = (uint32_t)pos;
    return pos;
}

/* off_t lseek(int fd, off_t offset, int whence) — 32-bit */
SYSCALL(sys_lseek)
{
    int64_t pos = do_seek(a1, (int32_t)a2, a3);
    if (pos > 0x7FFFFFFF)
        return -EOVERFLOW;
    return (int32_t)pos;
}

/* int _llseek(int fd, u32 off_high, u32 off_low, loff_t *result, int whence)
   The 64-bit version, which musl uses on i386. */
SYSCALL(sys_llseek)
{
    int64_t pos = do_seek(a1, (int64_t)(((uint64_t)a2 << 32) | a3), a5);
    if (pos < 0)
        return (int32_t)pos;
    return copy_to_user((void *)a4, &pos, sizeof pos);
}

/* struct statx, exactly as Linux lays it out (256 bytes). */
struct statx_ts { int64_t tv_sec; uint32_t tv_nsec; int32_t reserved; };
struct statx {
    uint32_t stx_mask, stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink, stx_uid, stx_gid;
    uint16_t stx_mode, spare0;
    uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
    struct statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align, stx_dio_offset_align;
    uint64_t spare3[12];
};
_Static_assert(sizeof(struct statx) == 256, "struct statx must match Linux");

#define STATX_BASIC_STATS 0x7FF     /* type, mode, nlink, uid, gid, times, ino, size, blocks */

/* int statx(int dirfd, const char *path, int flags, unsigned mask,
             struct statx *buf) — what musl's stat(), fstat() and lstat() use. */
SYSCALL(sys_statx)
{
    struct inode *i;
    if (a3 & AT_EMPTY_PATH) {       /* fstat: the open file dirfd itself */
        struct file *f = fd_get(a1);
        if (!f)
            return -EBADF;
        i = f->inode;
    } else {
        char path[PATH_MAX];
        int err = get_path(path, a2);
        if (err)
            return err;
        if ((int32_t)a1 != AT_FDCWD && path[0] != '/')
            return -ENOSYS;         /* relative to a directory fd: not yet */
        err = vfs_lookup(path, &i);
        if (err)
            return err;
    }

    struct statx st;
    memset(&st, 0, sizeof st);
    st.stx_mask = STATX_BASIC_STATS;
    st.stx_blksize = 4096;
    st.stx_nlink = i->nlink;
    st.stx_uid = i->uid;
    st.stx_gid = i->gid;
    st.stx_mode = (uint16_t)i->mode;
    st.stx_ino = i->ino;
    st.stx_size = i->size;
    st.stx_blocks = (i->size + 511) / 512;
    st.stx_atime.tv_sec = st.stx_ctime.tv_sec = st.stx_mtime.tv_sec = i->mtime;
    st.stx_dev_minor = 1;           /* the initramfs is "device 0:1" */
    return copy_to_user((void *)a5, &st, sizeof st);
}

/* --- processes --------------------------------------------------------------- */

/* void exit(int status) — and exit_group, which ends all threads of a
   process. With one thread per program, they're the same. */
SYSCALL(sys_exit)
{
    thread_current()->exit_code = a1 & 0xFF;    /* Linux keeps 8 bits */
    thread_exit();                              /* never returns */
    return 0;
}

/* pid_t getpid(void) — no processes yet (step 8): the thread's number.
   Also the classic "null" syscall for measuring syscall cost. */
SYSCALL(sys_getpid)
{
    return thread_id(thread_current());
}

/* --- the table ---------------------------------------------------------------- */

static const syscall_fn syscall_table[] = {
    [NR_exit]       = sys_exit,
    [NR_read]       = sys_read,
    [NR_write]      = sys_write,
    [NR_open]       = sys_open,
    [NR_close]      = sys_close,
    [NR_lseek]      = sys_lseek,
    [NR_getpid]     = sys_getpid,
    [NR__llseek]    = sys_llseek,
    [NR_exit_group] = sys_exit,
    [NR_openat]     = sys_openat,
    [NR_statx]      = sys_statx,
};

#define NSYSCALLS (sizeof syscall_table / sizeof syscall_table[0])

int32_t syscall_dispatch(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3,
                         uint32_t a4, uint32_t a5, uint32_t a6)
{
    if (nr < NSYSCALLS && syscall_table[nr])
        return syscall_table[nr](a1, a2, a3, a4, a5, a6);

    /* Not implemented. Log it to serial, so running real programs shows
       us which syscalls to write next (docs/DESIGN.md). */
    klog("syscall %lu not implemented (%s)\n", nr, thread_current()->name);
    return -ENOSYS;
}
