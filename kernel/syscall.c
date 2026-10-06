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
#include <bcache.h>
#include <futex.h>
#include <kprintf.h>
#include <memlayout.h>
#include <mm.h>
#include <pcache.h>
#include <pmm.h>
#include <power.h>
#include <proc.h>
#include <serial.h>
#include <signal.h>
#include <socket.h>
#include <syscalls.h>
#include <string.h>
#include <syscall.h>
#include <syscall64.h>
#include <thread.h>
#include <tty.h>
#include <uaccess.h>
#include <vfs.h>

/* Every handler gets the six argument registers; most use fewer. They are
   register-sized (syscalls.h): on i386 uintptr_t is uint32_t. */
#define U __attribute__((unused))
#define SYSCALL(name) \
    static intptr_t name(uintptr_t a1 U, uintptr_t a2 U, uintptr_t a3 U, \
                         uintptr_t a4 U, uintptr_t a5 U, uintptr_t a6 U)

typedef intptr_t (*syscall_fn)(uintptr_t, uintptr_t, uintptr_t,
                               uintptr_t, uintptr_t, uintptr_t);

/* --- files ------------------------------------------------------------------ */

/*
 * The one door read() and write() go through, for every kind of file.
 *
 * Three things happen here so that no filesystem has to remember them:
 *   - the range is checked, so nothing can name kernel memory;
 *   - the length is clamped to RW_MAX, because a syscall cannot be preempted
 *     and the caller must not be able to choose how long it runs;
 *   - the pages are faulted in, which is the part that is not obvious.
 *
 * A copy_to_user can sleep: an untouched page of an mmap'd file sends the
 * fault to the page cache, which waits for the disk. Any implementation that
 * reads a shared counter, copies, and then stores the counter has a window
 * there where a second process sees the same value — that was four real bugs
 * in the pipe, socket and TCP rings. Doing the faulting here, before the
 * implementation has looked at anything, leaves the copy unable to sleep, and
 * makes "a syscall runs to completion" true rather than nearly true.
 *
 * `at` is pread/pwrite's offset, or -1 for the file's own position.
 */
/* always_inline: each caller gets its own copy with `at` and `writing`
   known, so read() and write() don't pay for pread's checks or a call with
   five arguments — 40 cycles a read in make bench (docs/performance.md). */
static inline __attribute__((always_inline))
int32_t rw_at(uint32_t fd, uintptr_t buf, uintptr_t len, int writing, int64_t at)
{
    struct file *f = fd_get(fd);
    if (!f)
        return -EBADF;
    if (at >= 0) {
        uint32_t m = f->inode->mode;
        if (S_ISCHR(m) || S_ISFIFO(m) || (m & S_IFMT) == S_IFSOCK)
            return -ESPIPE;
        if (S_ISDIR(m))
            return -EISDIR;
        if (at > 0xFFFFFFFFll)          /* past the end of any file we can have */
            return writing ? -EFBIG : 0;
    }
    if (!user_range_ok(buf, len))
        return -EFAULT;
    if (len > RW_MAX)
        len = RW_MAX;
    /* `!writing` because the flag is the kernel's direction: a read() is the
       kernel writing into the user's buffer. */
    if (fault_in_user((const void *)buf, len, !writing) < 0)
        return -EFAULT;
    /*
     * Held across the call, because a blocking read sleeps inside it and the
     * fd table is shared with this process's other threads: without this, a
     * sibling's close() frees the file under the sleeper. See file_get.
     */
    file_get(f);
    /*
     * pread/pwrite leave the file's position alone, even while a sibling
     * thread reads the same open file. Every file system reads and writes
     * at f->pos, so they get a copy of the open file that starts at `at`,
     * and the copy's position is thrown away.
     */
    struct file copy;
    struct file *use = f;
    if (at >= 0) {
        copy = *f;
        copy.pos = (uint32_t)at;
        use = &copy;
    }
    int32_t rc = writing ? use->ops->write(use, (const void *)buf, len)
                         : use->ops->read(use, (void *)buf, len);
    if (rc > 0)
        notify(f->inode, writing ? IN_MODIFY : IN_ACCESS);
    file_put(f);
    return rc;
}

#define rw(fd, buf, len, writing) rw_at(fd, buf, len, writing, -1)

/* ssize_t read(int fd, void *buf, size_t count) */
SYSCALL(sys_read) { return rw(a1, a2, a3, 0); }

/* ssize_t write(int fd, const void *buf, size_t count) */
SYSCALL(sys_write) { return rw(a1, a2, a3, 1); }

/* ssize_t pread64 / pwrite64(int fd, void *buf, size_t count, u32 off_low,
   u32 off_high): at an offset, without moving the file's position. Poppler
   (Evince's PDF reader) reads every file this way. */
static int64_t off64(uint32_t lo, uint32_t hi) { return (int64_t)(((uint64_t)hi << 32) | lo); }
SYSCALL(sys_pread64)
{
    int64_t at = off64(a4, a5);
    return at < 0 ? -EINVAL : rw_at(a1, a2, a3, 0, at);
}
SYSCALL(sys_pwrite64)
{
    int64_t at = off64(a4, a5);
    return at < 0 ? -EINVAL : rw_at(a1, a2, a3, 1, at);
}

/*
 * ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
 * (sendfile64 on i386; the offset is 64-bit on both): copy from a file to
 * another file, pipe, terminal or socket without the bytes passing through
 * the program. They go page cache -> one kernel page -> the destination,
 * which takes them through file_copy_in (vfs.h): one copy fewer than
 * read() + write(), and one syscall instead of two per piece. BusyBox's cat
 * and cp use it.
 *
 * Like every syscall here it does at most RW_MAX bytes per call; callers
 * loop, as they must anyway (a short count is allowed). A destination that
 * can't take kernel memory (kbuf_ok) is -EINVAL, which makes the C library
 * or the program fall back to read + write — the same as on Linux for a
 * file that doesn't support it.
 */
/* The copy loop sendfile and copy_file_range share: up to `count` bytes of
   regular file `src` from *pos into `out` — at *out_pos if out_pos is given
   (out's own position stays), else at out's position, which moves. Page
   cache -> one kernel page -> the destination's write, on a private copy of
   its open file that says "kernel memory". Returns the bytes done (and moves
   *pos and *out_pos), or -errno if nothing was done. */
static int32_t copy_file_data(struct file *in, int64_t *pos, struct file *out,
                              int64_t *out_pos, uint32_t count)
{
    struct inode *src = in->inode;
    uint32_t page = pmm_alloc();
    if (!page)
        return -ENOMEM;
    uint8_t *buf = P2V(page);
    file_get(in);                       /* both can sleep below: hold them */
    file_get(out);
    int32_t done = 0, err = 0;
    while ((uint32_t)done < count) {
        if (*pos >= src->size)
            break;                      /* end of the file */
        uint32_t k = count - done;
        if (k > PAGE_SIZE)
            k = PAGE_SIZE;
        if (k > src->size - *pos)
            k = (uint32_t)(src->size - *pos);
        err = file_copy_out(src, (uint32_t)*pos, buf, k);
        if (err < 0)
            break;
        struct file w = *out;
        w.kbuf = 1;
        if (out_pos)
            w.pos = (uint32_t)*out_pos;
        int32_t n = out->ops->write(&w, buf, k);
        if (out_pos) {
            if (n > 0)
                *out_pos += n;
        } else {
            out->pos = w.pos;           /* a regular file's position moves; a pipe,
                                           terminal or socket has none that matters */
        }
        if (n <= 0) {
            err = n;
            break;
        }
        done += n;
        *pos += n;
        if ((uint32_t)n < k || signal_pending())
            break;                      /* full, or someone wants us: return */
    }
    if (done > 0) {
        notify(src, IN_ACCESS);
        notify(out->inode, IN_MODIFY);
    }
    file_put(out);
    file_put(in);
    pmm_free(page);
    return done == 0 && err < 0 ? err : done;
}

/* The source must be file data the kernel can copy out: a regular file in
   RAM or on a disk (not a /proc file, which is made up on reading). */
static int copyable_source(const struct inode *src)
{
    return S_ISREG(src->mode) && (!src->ops || src->fs);
}

SYSCALL(sys_sendfile)
{
    struct file *out = fd_get(a1), *in = fd_get(a2);
    if (!out || !in)
        return -EBADF;
    if ((in->flags & O_ACCMODE) == O_WRONLY || (out->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (!copyable_source(in->inode))
        return -EINVAL;
    if (!out->ops->write || !out->ops->kbuf_ok || (out->flags & O_APPEND))
        return -EINVAL;                 /* (O_APPEND: as on Linux) */
    int64_t pos = in->pos;
    if (a3 && copy_from_user(&pos, (const void *)a3, sizeof pos) < 0)
        return -EFAULT;
    if (pos < 0)
        return -EINVAL;
    int32_t done = copy_file_data(in, &pos, out, NULL, a4 > RW_MAX ? RW_MAX : (uint32_t)a4);
    if (done < 0)
        return done;
    if (a3) {
        if (copy_to_user((void *)a3, &pos, sizeof pos) < 0)
            return -EFAULT;
    } else {
        in->pos = (uint32_t)pos;
    }
    return done;
}

/*
 * ssize_t copy_file_range(int fd_in, off64_t *off_in, int fd_out,
 *                         off64_t *off_out, size_t len, unsigned flags)
 * Copy between two regular files inside the kernel (coreutils' cp and
 * install use it first). An offset pointer that is NULL means "the file's
 * own position, which moves". The same loop as sendfile, so the same rules:
 * at most RW_MAX a call, and a destination that can't take kernel memory is
 * -EXDEV — which tells the program to copy with read and write instead.
 */
SYSCALL(sys_copy_file_range)
{
    if (a6)
        return -EINVAL;                 /* no flags are defined */
    struct file *in = fd_get(a1), *out = fd_get(a3);
    if (!in || !out)
        return -EBADF;
    if ((in->flags & O_ACCMODE) == O_WRONLY || (out->flags & O_ACCMODE) == O_RDONLY ||
        (out->flags & O_APPEND))
        return -EBADF;
    if (!copyable_source(in->inode) || !S_ISREG(out->inode->mode))
        return -EINVAL;
    if (!out->ops->write || !out->ops->kbuf_ok)
        return -EXDEV;
    int64_t pos = in->pos, opos = out->pos;
    if ((a2 && copy_from_user(&pos, (const void *)a2, sizeof pos) < 0) ||
        (a4 && copy_from_user(&opos, (const void *)a4, sizeof opos) < 0))
        return -EFAULT;
    uint32_t count = a5 > RW_MAX ? RW_MAX : (uint32_t)a5;
    if (pos < 0 || opos < 0)
        return -EINVAL;
    if (in->inode == out->inode && pos < opos + count && opos < pos + count)
        return -EINVAL;                 /* the same file, overlapping */
    int32_t done = copy_file_data(in, &pos, out, a4 ? &opos : NULL, count);
    if (done < 0)
        return done;
    if (a2) {
        if (copy_to_user((void *)a2, &pos, sizeof pos) < 0)
            return -EFAULT;
    } else {
        in->pos = (uint32_t)pos;
    }
    if (a4 && copy_to_user((void *)a4, &opos, sizeof opos) < 0)
        return -EFAULT;
    return done;
}

/* The directory a *at() call's dirfd means: NULL for AT_FDCWD (the
   current directory), else the open directory. 0, or -errno. */
int dir_of(uint32_t dirfd, struct inode **dir)
{
    if ((int32_t)dirfd == AT_FDCWD) {
        *dir = NULL;
        return 0;
    }
    struct file *f = fd_get(dirfd);
    if (!f)
        return -EBADF;
    if (!S_ISDIR(f->inode->mode))
        return -ENOTDIR;
    *dir = f->inode;
    return 0;
}

/* Copy a path from user memory. 0, or -errno. */
int get_path(char *dst, uintptr_t user_path)
{
    long n = strncpy_from_user(dst, (const char *)user_path, PATH_MAX);
    if (n < 0)
        return n;
    return n == 0 ? -ENOENT : 0;    /* "" is not a file name */
}

int32_t do_open(struct inode *dir, uintptr_t user_path, uint32_t flags, uint32_t mode)
{
    char path[PATH_MAX];
    int err = get_path(path, user_path);
    if (err)
        return err;
    struct file *f;
    if (flags & O_CREAT)
        mode &= ~proc_current()->umask;
    err = vfs_open_at(dir, path, flags, mode, &f);
    if (err)
        return err;
    int fd = fd_install(f);
    if (fd < 0)
        file_put(f);
    else if (flags & O_CLOEXEC)
        proc_current()->cloexec |= 1ull << fd;    /* closed by execve */
    return fd;
}

/* int open(const char *path, int flags, mode_t mode) */
SYSCALL(sys_open)
{
    return do_open(NULL, a1, a2, a3);
}

/* int creat(const char *path, mode_t mode): an old spelling of open */
SYSCALL(sys_creat)
{
    return do_open(NULL, a1, O_CREAT | O_WRONLY | O_TRUNC, a2);
}

/* int openat(int dirfd, const char *path, int flags, mode_t mode) */
SYSCALL(sys_openat)
{
    struct inode *dir;
    int err = dir_of(a1, &dir);
    return err ? err : do_open(dir, a2, a3, a4);
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
    if (S_ISCHR(f->inode->mode) || S_ISFIFO(f->inode->mode))
        return -ESPIPE;             /* the console and pipes have no position */
    if (S_ISDIR(f->inode->mode)) {
        if (off != 0 || whence != SEEK_SET)
            return -EINVAL;         /* directories: only rewinddir() */
        vfs_dir_rewind(f);
        return 0;
    }
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

/* The file a stat call is about: the open file dirfd itself (fstat, with
   AT_EMPTY_PATH), or a path relative to dirfd. 0, or -errno. */
static int stat_target(uintptr_t dirfd, uintptr_t user_path, uint32_t flags,
                       struct inode **out)
{
    if (flags & AT_EMPTY_PATH) {
        struct file *f = fd_get(dirfd);
        if (!f)
            return -EBADF;
        *out = f->inode;
        return 0;
    }
    char path[PATH_MAX];
    struct inode *dir;
    int err = get_path(path, user_path);
    if (!err)
        err = dir_of(dirfd, &dir);
    if (!err)                       /* lstat doesn't follow a final symlink */
        err = vfs_lookup_at(dir, path, !(flags & AT_SYMLINK_NOFOLLOW), out);
    return err;
}

/* int statx(int dirfd, const char *path, int flags, unsigned mask,
             struct statx *buf) — what musl's stat(), fstat() and lstat() use
   on i386. */
SYSCALL(sys_statx)
{
    struct inode *i;
    int err = stat_target(a1, a2, a3, &i);
    if (err)
        return err;

    struct statx st;
    memset(&st, 0, sizeof st);
    st.stx_mask = STATX_BASIC_STATS;
    st.stx_blksize = 4096;
    st.stx_nlink = i->nlink;
    st.stx_uid = i->uid;
    st.stx_gid = i->gid;
    st.stx_mode = (uint16_t)i->mode;
    st.stx_ino = i->fs ? i->fs_ino : i->ino;   /* a disk's own number: stable */
    st.stx_size = i->size;
    st.stx_blocks = (i->size + 511) / 512;
    st.stx_atime.tv_sec = i->atime ? i->atime : i->mtime;
    st.stx_mtime.tv_sec = i->mtime;
    st.stx_ctime.tv_sec = i->ctime ? i->ctime : i->mtime;
    st.stx_dev_minor = i->mnt ? i->mnt->dev_minor : 1;  /* RAM: "device 0:1" */
    st.stx_rdev_major = i->rdev >> 8;
    st.stx_rdev_minor = i->rdev & 0xFF;
    return copy_to_user((void *)a5, &st, sizeof st);
}

/* int pipe(int fds[2]) / pipe2(int fds[2], int flags) */
static int32_t do_pipe(uintptr_t user_fds, uint32_t flags)
{
    struct file *rd, *wr;
    int err = pipe_create(&rd, &wr);
    if (err)
        return err;
    rd->flags |= flags & O_NONBLOCK;
    wr->flags |= flags & O_NONBLOCK;
    int fds[2] = { fd_install(rd), -1 };
    if (fds[0] >= 0)
        fds[1] = fd_install(wr);
    if (fds[1] < 0 || copy_to_user((void *)user_fds, fds, sizeof fds) < 0) {
        if (fds[0] >= 0)
            fd_close(fds[0]);
        else
            file_put(rd);
        if (fds[1] >= 0)
            fd_close(fds[1]);
        else
            file_put(wr);
        return fds[1] < 0 ? -EMFILE : -EFAULT;
    }
    if (flags & O_CLOEXEC)
        proc_current()->cloexec |= (1ull << fds[0]) | (1ull << fds[1]);
    return 0;
}

SYSCALL(sys_pipe)  { return do_pipe(a1, 0); }
SYSCALL(sys_pipe2) { return do_pipe(a1, a2); }

/* int dup(int fd): the same open file under the lowest free number. */
SYSCALL(sys_dup)
{
    struct file *f = fd_get(a1);
    if (!f)
        return -EBADF;
    int fd = fd_install(f);
    if (fd >= 0)
        f->refs++;
    return fd;
}

/* int dup2(int old, int new) / dup3(old, new, flags): exactly fd `new`. */
static int32_t do_dup2(uint32_t oldfd, uint32_t newfd, uint32_t flags)
{
    struct file *f = fd_get(oldfd);
    if (!f || newfd >= FD_MAX)
        return -EBADF;
    if (oldfd == newfd)
        return newfd;
    f->refs++;
    fd_install_at(newfd, f);            /* closes what was at newfd */
    if (flags & O_CLOEXEC)
        proc_current()->cloexec |= 1ull << newfd;
    return newfd;
}

SYSCALL(sys_dup2) { return do_dup2(a1, a2, 0); }
SYSCALL(sys_dup3) { return a1 == a2 ? -EINVAL : do_dup2(a1, a2, a3); }

/* int fcntl(int fd, int cmd, ...) — the common commands. */
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030
#define F_ADD_SEALS     1033            /* memfd: add F_SEAL_* bits */
#define F_GET_SEALS     1034
#define F_SEAL_SEAL     0x0001          /* no further seals */
#define FD_CLOEXEC      1

SYSCALL(sys_fcntl)
{
    struct process *p = proc_current();
    struct file *f = fd_get(a1);
    if (!f)
        return -EBADF;
    switch (a2) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        for (uint32_t fd = a3; fd < FD_MAX; fd++) {     /* lowest free >= arg */
            if (!p->files[fd]) {
                p->files[fd] = f;
                f->refs++;
                if (a2 == F_DUPFD_CLOEXEC)
                    p->cloexec |= 1ull << fd;
                return fd;
            }
        }
        return -EMFILE;
    case F_GETFD:
        return (p->cloexec >> a1) & 1 ? FD_CLOEXEC : 0;
    case F_SETFD:
        if (a3 & FD_CLOEXEC)
            p->cloexec |= 1ull << a1;
        else
            p->cloexec &= ~(1ull << a1);
        return 0;
    case F_GETFL:
        return f->flags;
    case F_SETFL:                       /* only these two may change */
        f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | (a3 & (O_APPEND | O_NONBLOCK));
        return 0;
    case F_ADD_SEALS:
        if (!(f->inode->flags & I_MEMFD))
            return -EINVAL;
        if (f->inode->fs_ino & F_SEAL_SEAL)
            return -EPERM;
        f->inode->fs_ino |= a3;
        return 0;
    case F_GET_SEALS:
        if (!(f->inode->flags & I_MEMFD))
            return -EINVAL;
        return (int32_t)f->inode->fs_ino;
    default:                            /* record locks: fs/lock.c */
        return lock_fcntl_cmd(a2) ? lock_fcntl(f, a2, a3) : -EINVAL;
    }
}

/* ssize_t getdents64(int fd, struct linux_dirent64 *buf, size_t count)
   — reading a directory (what `ls` uses). Each entry: inode number,
   offset of the next, its own length (8-aligned), type, name + NUL.
   The open directory keeps its place (a cursor, vfs.c), so a big
   directory read in many calls costs O(entries), not O(entries squared). */
#define DT_FIFO 1
#define DT_CHR  2
#define DT_DIR  4
#define DT_REG  8
#define DT_LNK 10

static uint8_t dirent_type(uint32_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFDIR: return DT_DIR;
    case S_IFREG: return DT_REG;
    case S_IFLNK: return DT_LNK;
    case S_IFCHR: return DT_CHR;
    case S_IFIFO: return DT_FIFO;
    default:      return 0;
    }
}

SYSCALL(sys_getdents64)
{
    struct file *f = fd_get(a1);
    if (!f)
        return -EBADF;
    struct inode *dir = f->inode;
    if (!S_ISDIR(dir->mode))
        return -ENOTDIR;

    /* Clamp before the pre-fault, so the caller cannot choose how many pages
       get touched, and pre-fault so the copies in the loop below do not
       ordinarily sleep. */
    if (a3 > RW_MAX)
        a3 = RW_MAX;
    if (fault_in_user((void *)a2, a3, 1) < 0)
        return -EFAULT;

    /*
     * `next` and `last` are inode pointers held across a copy_to_user, and
     * that copy can sleep, so both are pinned for as long as they are held.
     *
     * Unpinned, a sibling's unlink during the sleep calls inode_free on them
     * -- detach() frees an unlinked inode the moment its refs reaches 0 --
     * and a later iteration reads a name pointer and a length out of the
     * freed node and copies that to user space. `next` is the one that is
     * easy to miss, and was missed: it is fetched here, before the loop, and
     * the copies that emit "." and ".." sleep before it is ever used. The
     * pin on the current entry alone looks sufficient and is not.
     * user/musl/racetest.c arms a sleep and deletes the directory underneath
     * this loop, which is how that was found rather than argued about.
     */
    uint8_t rec[19 + NAME_MAX + 1 + 8] __attribute__((aligned(8)));
    uint32_t written = 0;
    int32_t rc;
    struct inode *next = vfs_dir_first(f), *last = NULL;
    int ended = 0;
    if (next)
        vfs_get(next);
    for (;;) {
        const char *name;
        uint32_t len;
        struct inode *i;
        if (f->pos == 0) {              /* "." and ".." come first */
            name = ".", len = 1, i = dir;
        } else if (f->pos == 1) {
            name = "..", len = 2;
            /* dir is pinned by the open file; its parent is pinned by dir. */
            i = (dir->flags & I_UNLINKED) ? dir : dir->parent;
        } else {
            i = next;
            if (!i) {
                ended = 1;              /* that was all */
                break;
            }
            name = i->name, len = i->name_len;
        }
        uint32_t reclen = (19 + len + 1 + 7) & ~7u;
        if (written + reclen > a3) {
            if (written == 0) {
                rc = -EINVAL;           /* buffer too small for one entry */
                goto out;
            }
            break;
        }
        uint64_t ino = i->fs ? i->fs_ino : i->ino, off = f->pos + 1;
        memcpy(rec, &ino, 8);
        memcpy(rec + 8, &off, 8);
        memcpy(rec + 16, &reclen, 2);
        rec[18] = dirent_type(i->mode);
        memcpy(rec + 19, name, len);
        memset(rec + 19 + len, 0, reclen - 19 - len);
        if (copy_to_user((uint8_t *)a2 + written, rec, reclen) < 0) {
            rc = -EFAULT;
            goto out;
        }
        written += reclen;
        if (f->pos >= 2) {
            /* Unlinked while we slept: sibling_remove has already taken it
               out of the list, so a cursor cannot be anchored on it, and its
               next_sibling leads back into a list it is no longer part of.
               Stop, keep the cursor we had, and let the caller ask again. */
            if (i->flags & I_UNLINKED)
                break;
            struct inode *after = vfs_dir_after(i);
            if (after)
                vfs_get(after);
            if (last)
                vfs_put(last);
            last = i;                   /* next's reference moves to last */
            next = after;               /* and after's to next */
        }
        f->pos++;
    }
    /* The anchor must still be in the list: it may have been deleted during a
       later entry's copy rather than its own. */
    if (last && (last->flags & I_UNLINKED)) {
        vfs_put(last);
        last = NULL;
    }
    /* Remember where we stopped. */
    rc = (int32_t)written;
    if (ended || last) {
        int err = vfs_dir_stop(f, ended ? NULL : last);
        if (err)
            rc = err;
    }
out:
    if (last)
        vfs_put(last);                  /* only now: the cursor needed it */
    if (next)
        vfs_put(next);
    return rc;
}

/* int chdir(const char *path) / fchdir(int fd) */
SYSCALL(sys_chdir)
{
    char path[PATH_MAX];
    struct inode *i;
    int err = get_path(path, a1);
    if (!err)
        err = vfs_lookup(path, &i);
    if (err)
        return err;
    if (!S_ISDIR(i->mode))
        return -ENOTDIR;
    proc_set_cwd(proc_current(), i);
    return 0;
}

/* int chroot(const char *path): path becomes this process's "/" — and its
   children's (fs/vfs.c stops ".." there). Root only. The current directory
   stays where it is, as on Linux; `chroot dir prog` (BusyBox) does
   chdir("/") itself. */
SYSCALL(sys_chroot)
{
    struct process *p = proc_current();
    if (p->cred.euid != 0)
        return -EPERM;
    char path[PATH_MAX];
    struct inode *i;
    int err = get_path(path, a1);
    if (!err)
        err = vfs_lookup(path, &i);
    if (err)
        return err;
    if (!S_ISDIR(i->mode))
        return -ENOTDIR;
    vfs_get(i);
    if (p->root)
        vfs_put(p->root);
    p->root = i;
    return 0;
}

SYSCALL(sys_fchdir)
{
    struct file *f = fd_get(a1);
    if (!f)
        return -EBADF;
    if (!S_ISDIR(f->inode->mode))
        return -ENOTDIR;
    proc_set_cwd(proc_current(), f->inode);
    return 0;
}

/* long getcwd(char *buf, size_t size): the current directory's full path,
   built by walking up to the root. Returns its length with the NUL. */
SYSCALL(sys_getcwd)
{
    char path[PATH_MAX];
    struct inode *i = proc_current()->cwd;
    if (i->flags & I_UNLINKED)
        return -ENOENT;                 /* our directory was deleted */
    int32_t len = vfs_path(i, path, sizeof path);
    if (len < 0)
        return len;
    if ((uint32_t)len + 1 > a2)
        return -ERANGE;
    if (copy_to_user((void *)a1, path, len + 1) < 0)
        return -EFAULT;
    return len + 1;
}

#ifdef SELFTEST
/* Arm the n'th copy_to_user/copy_from_user from this thread to yield first,
   so a test can produce the interleaving the ring-buffer fixes are about.
   `make test` only — docs/security.md explains why it has to exist. */
SYSCALL(sys_uaccess_test)
{
    uaccess_test_arm((int)a1);
    return 0;
}
#endif

/* ssize_t writev / readv(int fd, const struct iovec *iov, int count):
   several buffers in one call (musl's printf writes this way). Stops at
   the first short transfer, like Linux. */
static int32_t do_iov(uint32_t fd, uintptr_t user_iov, uint32_t count, int writing)
{
    if (!fd_get(fd))
        return -EBADF;
    if (count > 1024)
        return -EINVAL;
    int32_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        user_addr_t base, len;          /* struct iovec: two pointer-sized words */
        uintptr_t at = user_iov + i * 2 * sizeof base;
        if (get_user_word(&base, at) < 0 || get_user_word(&len, at + sizeof base) < 0)
            return total ? total : -EFAULT;
        if (len > 0x7FFFF000)
            len = 0x7FFFF000;           /* Linux's limit for one transfer */
        if (len == 0)
            continue;
        /* One buffer at a time through rw(), so each gets the clamp and the
           pre-fault. Faulting them all in first would be wrong anyway: a
           short transfer stops the loop and the rest is never touched. */
        int32_t n = rw(fd, base, len, writing);
        if (n < 0)
            return total ? total : n;
        total += n;
        if ((uint32_t)n < len)
            break;
    }
    return total;
}

SYSCALL(sys_writev) { return do_iov(a1, a2, a3, 1); }
SYSCALL(sys_readv)  { return do_iov(a1, a2, a3, 0); }

/* ssize_t readlink(const char *path, char *buf, size_t size) — and
   readlinkat with AT_FDCWD. No NUL is added (that's how Linux does it). */
static int32_t do_readlink(struct inode *dir, uintptr_t user_path, uintptr_t buf, uint32_t size)
{
    char path[PATH_MAX];
    struct inode *i;
    int err = get_path(path, user_path);
    if (!err)
        err = vfs_lookup_at(dir, path, 0, &i);
    if (err)
        return err;
    if (!S_ISLNK(i->mode))
        return -EINVAL;
    const void *target = i->data;
    uint32_t len = i->size;
    if (i->flags & (I_PROC_FD | I_PROC_EXE)) {
        int32_t r = procfs_readlink(i, path, sizeof path);  /* path is free now */
        if (r < 0)
            return r;
        target = path, len = r;
    }
    uint32_t n = len < size ? len : size;
    if (copy_to_user((void *)buf, target, n) < 0)
        return -EFAULT;
    return n;
}

SYSCALL(sys_readlink)   { return do_readlink(NULL, a1, a2, a3); }
SYSCALL(sys_readlinkat)
{
    struct inode *dir;
    int err = dir_of(a1, &dir);
    return err ? err : do_readlink(dir, a2, a3, a4);
}

/* int access(const char *path, int mode) / faccessat(dirfd, path, mode, flags).
   We're all root: everything exists-and-allowed, except executing a file
   with no x bit. */
#define X_OK 1

static int32_t do_access(struct inode *dir, uintptr_t user_path, uint32_t mode)
{
    char path[PATH_MAX];
    struct inode *i;
    int err = get_path(path, user_path);
    if (!err)
        err = vfs_lookup_at(dir, path, 1, &i);
    if (err)
        return err;
    if ((mode & X_OK) && !S_ISDIR(i->mode) && !(i->mode & 0111))
        return -EACCES;
    return 0;
}

SYSCALL(sys_access) { return do_access(NULL, a1, a2); }
SYSCALL(sys_faccessat)
{
    struct inode *dir;
    int err = dir_of(a1, &dir);
    return err ? err : do_access(dir, a2, a3);
}

/* madvise, set_robust_list...: nothing to do, success. */
SYSCALL(sys_nop) { return 0; }

/* int mincore(void *addr, size_t length, unsigned char *vec): one byte per
   page of the range, 1 if it is in RAM. Programs also use it to find out
   where memory is mapped at all — GNU tools' stack-overflow handler (gnulib)
   probes the stack's extent page by page — so a page in no area must be
   ENOMEM. When this was a stub answering "all there", each `grep` walked the
   address space for most of a minute. */
SYSCALL(sys_mincore)
{
    if (a1 & (PAGE_SIZE - 1))
        return -EINVAL;
    if (a1 + a2 < a1 || a1 + a2 > USER_ADDR_END)
        return -ENOMEM;
    uintptr_t pages = (a2 + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t vec[256];
    for (uintptr_t done = 0; done < pages; ) {
        uint32_t n = pages - done < sizeof vec ? (uint32_t)(pages - done) : sizeof vec;
        int err = mm_mincore(a1 + done * PAGE_SIZE, n, vec);
        if (err)
            return err;
        if (copy_to_user((void *)(a3 + done), vec, n) < 0)
            return -EFAULT;
        done += n;
    }
    return 0;
}

/* sync(): write every changed disk block now. fsync(fd) / fdatasync(fd):
   the same (the cache doesn't know which blocks belong to which file). */
/* File data first (it is in the page cache), then the metadata. */
SYSCALL(sys_sync)  { pcache_sync(); bsync(); return 0; }

/* syncfs(fd): the file system fd is on — here, all of them (the caches
   don't keep them apart). */
SYSCALL(sys_syncfs)
{
    if (!fd_get(a1))
        return -EBADF;
    pcache_sync();
    return bsync();
}
SYSCALL(sys_fsync)
{
    struct file *f = fd_get(a1);
    if (!f)
        return -EBADF;
    int err = f->inode ? pcache_flush(f->inode) : 0;
    int err2 = bsync();
    return err ? err : err2;
}

/* reboot(magic, magic2, cmd, arg). The two magic numbers guard against a
   stray call (the second is Linus Torvalds' birthday, in a few spellings).
   Linux leaves syncing to the caller; we sync anyway, because there is no
   device shutdown step after it to flush what's in the cache. */
#define REBOOT_MAGIC1   0xFEE1DEADu
#define CMD_RESTART     0x01234567u
#define CMD_RESTART2    0xA1B2C3D4u     /* with a command string: ignored */
#define CMD_HALT        0xCDEF0123u
#define CMD_POWER_OFF   0x4321FEDCu
#define CMD_CAD_ON      0x89ABCDEFu
#define CMD_CAD_OFF     0u
SYSCALL(sys_reboot)
{
    if (a1 != REBOOT_MAGIC1 ||
        (a2 != 672274793 && a2 != 85072278 && a2 != 369367448 && a2 != 537993216))
        return -EINVAL;
    switch (a3) {
    case CMD_CAD_ON:  power_set_ctrl_alt_del(1); return 0;
    case CMD_CAD_OFF: power_set_ctrl_alt_del(0); return 0;
    case CMD_RESTART:
    case CMD_RESTART2:
    case CMD_HALT:
    case CMD_POWER_OFF:
        break;
    default:
        return -EINVAL;
    }
    pcache_sync();
    bsync();
    if (a3 == CMD_HALT)
        power_halt();
    if (a3 == CMD_POWER_OFF)
        power_off();
    power_restart();
}

/* vhangup(): agetty calls it before it asks for a login name. */
SYSCALL(sys_vhangup) { tty_hangup(); return 0; }

/* sched_getaffinity(pid, size, mask): one CPU. (musl's sysconf asks.) */
SYSCALL(sys_getaffinity)
{
    uint32_t mask = 1;
    if (a2 < 4)
        return -EINVAL;
    return copy_to_user((void *)a3, &mask, 4) < 0 ? -EFAULT : 4;
}

/* mode_t umask(mode_t mask) */
SYSCALL(sys_umask)
{
    struct process *p = proc_current();
    uint32_t old = p->umask;
    p->umask = a1 & 0777;
    return old;
}

/* int ioctl(int fd, unsigned long request, ...) — device-specific
   requests; only the terminal has any. Others: "not a terminal" (that's
   how isatty() finds out). */
SYSCALL(sys_ioctl)
{
    struct file *f = fd_get(a1);
    if (!f)
        return -EBADF;
    return f->ops->ioctl ? f->ops->ioctl(f, a2, a3) : -ENOTTY;
}

/* --- signals ------------------------------------------------------------------ */

SYSCALL(sys_rt_sigaction)   { return sys_rt_sigaction_impl(a1, a2, a3, a4); }
SYSCALL(sys_rt_sigprocmask) { return sys_rt_sigprocmask_impl(a1, a2, a3, a4); }
SYSCALL(sys_alarm) { return sys_alarm_impl(a1); }
SYSCALL(sys_setitimer) { return sys_setitimer_impl(a1, a2, a3); }
SYSCALL(sys_getitimer) { return sys_getitimer_impl(a1, a2); }
SYSCALL(sys_sigreturn)      { return sys_sigreturn_impl(0); }
SYSCALL(sys_rt_sigreturn)   { return sys_sigreturn_impl(1); }
SYSCALL(sys_kill)           { return sys_kill_impl((int32_t)a1, a2); }
SYSCALL(sys_rt_sigsuspend)  { return sys_rt_sigsuspend_impl(a1, a2); }
SYSCALL(sys_pause)          { return sys_pause_impl(); }
SYSCALL(sys_rt_sigpending)  { return sys_rt_sigpending_impl(a1, a2); }
SYSCALL(sys_rt_sigtimedwait)   { return sys_rt_sigtimedwait_impl(a1, a2, a3, a4, 0); }
SYSCALL(sys_rt_sigtimedwait64) { return sys_rt_sigtimedwait_impl(a1, a2, a3, a4, 1); }

/* tkill(tid, sig) / tgkill(tgid, tid, sig): a signal for one thread
   (pthread_kill, raise). */
SYSCALL(sys_tkill)
{
    struct thread *t = thread_by_tid(a1);
    return t ? signal_send_thread(t, a2) : -ESRCH;
}

SYSCALL(sys_tgkill)
{
    struct thread *t = thread_by_tid(a2);
    if (!t || t->proc->pid != (int)a1)
        return -ESRCH;
    return signal_send_thread(t, a3);
}

/* gettid(): this thread's id (= getpid() in a process's first thread). */
SYSCALL(sys_gettid)
{
    return thread_current()->tid;
}

/* futex(uaddr, op, val, timeout, uaddr2, val3): see futex.c. */
SYSCALL(sys_futex)         { return sys_futex_impl(a1, a2, a3, a4, a5, a6, 0); }
SYSCALL(sys_futex_time64)  { return sys_futex_impl(a1, a2, a3, a4, a5, a6, 1); }

/* int sched_yield(void): let the next thread run. */
SYSCALL(sys_sched_yield)
{
    thread_yield();
    return 0;
}

/* --- process groups and sessions --------------------------------------------- */

SYSCALL(sys_setpgid) { return proc_setpgid(a1, a2); }
SYSCALL(sys_setsid)  { return proc_setsid(); }
SYSCALL(sys_getpgrp) { return proc_current()->pgid; }

SYSCALL(sys_getpgid)
{
    struct process *p = a1 ? proc_find(a1) : proc_current();
    return p ? p->pgid : -ESRCH;
}

SYSCALL(sys_getsid)
{
    struct process *p = a1 ? proc_find(a1) : proc_current();
    return p ? p->sid : -ESRCH;
}

/* --- processes --------------------------------------------------------------- */

/* void exit(int status): ends the calling thread only (musl calls it from
   pthread_exit); the last thread's exit ends the process. */
SYSCALL(sys_exit)
{
    proc_thread_exit(W_EXITED(a1));             /* never returns */
}

/* void exit_group(int status): ends every thread (C's exit()). */
SYSCALL(sys_exit_group)
{
    proc_exit(W_EXITED(a1));                    /* never returns */
}

/* pid_t getpid(void) — also the classic "null" syscall for measuring
   syscall cost. */
SYSCALL(sys_getpid)
{
    return proc_current()->pid;
}

/* pid_t getppid(void) — 0 if the kernel started us. */
SYSCALL(sys_getppid)
{
    struct process *p = proc_current();
    return p->parent ? p->parent->pid : 0;
}

/* pid_t fork(void), and vfork(): with copy-on-write, fork is already as
   cheap as vfork was meant to be, so vfork is simply fork. */
SYSCALL(sys_fork)
{
    return proc_fork();
}

/* int execve(const char *path, char *const argv[], char *const envp[]) */
SYSCALL(sys_execve)
{
    return proc_execve(a1, a2, a3);
}

/* pid_t wait4(pid_t pid, int *status, int options, struct rusage *ru),
   and the older waitpid(pid, status, options). (No rusage yet.) */
SYSCALL(sys_wait4)
{
    return proc_wait4((int32_t)a1, a2, (int32_t)a3, a4);
}

/* waitpid(pid, status, options): wait4 without the rusage pointer (its
   fourth register is whatever the program left there). i386 only. */
SYSCALL(sys_waitpid)
{
    return proc_wait4((int32_t)a1, a2, (int32_t)a3, 0);
}

/* void *brk(void *addr) — the raw syscall: returns the (new) break. */
SYSCALL(sys_brk)
{
    return (intptr_t)mm_brk(a1);
}

/* void *mmap2(addr, len, prot, flags, fd, pgoffset): anonymous memory, or
   a file (the dynamic linker maps shared libraries this way). pgoffset
   counts 4 KiB pages. */
SYSCALL(sys_mmap2)
{
    struct inode *file = NULL;
    uint32_t fmode = 0;
    if (!(a4 & 0x20)) {                 /* not MAP_ANONYMOUS */
        struct file *f = fd_get(a5);
        if (!f)
            return -EBADF;
        if (f->ops && f->ops->mmap)     /* device memory (/dev/fb0) */
            return mm_mmap_device(a1, a2, a3, a4, f, a6 * 4096);
        file = f->inode;
        fmode = f->flags;
    }
    return mm_mmap(a1, a2, a3, a4, file, a6 * 4096, fmode);
}

/* int munmap(void *addr, size_t len) */
SYSCALL(sys_munmap)
{
    return mm_munmap(a1, a2);
}

/* mremap(old, old_len, new_len, flags, new_addr) */
SYSCALL(sys_mremap)
{
    return mm_mremap(a1, a2, a3, a4, a5);
}

/* membarrier: on one CPU a syscall is already a barrier. QUERY names
   the commands we accept so musl and Firefox stop retrying ENOSYS. */
#define MEMBARRIER_CMD_QUERY                      0
#define MEMBARRIER_CMD_GLOBAL                     (1u << 0)
#define MEMBARRIER_CMD_GLOBAL_EXPEDITED           (1u << 1)
#define MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED  (1u << 2)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED          (1u << 3)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED (1u << 4)
#define MEMBARRIER_CMDS (MEMBARRIER_CMD_GLOBAL | MEMBARRIER_CMD_GLOBAL_EXPEDITED | \
                         MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED | \
                         MEMBARRIER_CMD_PRIVATE_EXPEDITED | \
                         MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED)

SYSCALL(sys_membarrier)
{
    if (a1 == MEMBARRIER_CMD_QUERY)
        return (int32_t)MEMBARRIER_CMDS;
    if (a1 & ~MEMBARRIER_CMDS)
        return -EINVAL;
    return 0;
}

/* prctl: Firefox names threads and asks a few process flags. The ones
   that need real machinery (seccomp, capability bounding) stay EINVAL. */
#define PR_SET_PDEATHSIG     1
#define PR_GET_PDEATHSIG     2
#define PR_GET_DUMPABLE      3
#define PR_SET_DUMPABLE      4
#define PR_SET_NAME         15
#define PR_GET_NAME         16
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39

SYSCALL(sys_prctl)
{
    struct thread *t = thread_current();
    switch (a1) {
    case PR_SET_NAME: {
        char name[16];
        if (copy_from_user(name, (void *)a2, sizeof name) < 0)
            return -EFAULT;
        name[sizeof name - 1] = '\0';
        thread_set_name(t, name);
        return 0;
    }
    case PR_GET_NAME:
        if (copy_to_user((void *)a2, t->name, sizeof t->name) < 0)
            return -EFAULT;
        return 0;
    case PR_SET_PDEATHSIG:
    case PR_GET_PDEATHSIG:
    case PR_GET_DUMPABLE:
    case PR_SET_DUMPABLE:
    case PR_SET_NO_NEW_PRIVS:
    case PR_GET_NO_NEW_PRIVS:
        if (a1 == PR_GET_DUMPABLE || a1 == PR_GET_NO_NEW_PRIVS)
            return 0;                   /* "not dumpable" / "no new privs" */
        if (a1 == PR_GET_PDEATHSIG) {
            uint32_t zero = 0;
            if (copy_to_user((void *)a2, &zero, 4) < 0)
                return -EFAULT;
        }
        return 0;
    default:
        return -EINVAL;
    }
}

/* int mprotect(void *addr, size_t len, int prot) */
SYSCALL(sys_mprotect)
{
    return mm_mprotect(a1, a2, a3);
}

/* clone(flags, new_stack, parent_tid *, tls, child_tid *): a new process
   like fork, on a new stack if one is given (musl's posix_spawn), or with
   CLONE_THREAD a new thread in this process (pthread_create). */
SYSCALL(sys_clone)
{
    return proc_clone(a1, a2, a3, a4, a5);
}

#ifdef ARCH_X86_64
/* --- x86_64: where its ABI differs from i386 -------------------------------
   Most x86_64 system calls are the i386 ones with wider registers, and the
   table below points straight at the same handlers. These adapters cover the
   rest: an offset in bytes instead of pages, one 64-bit register instead of
   two 32-bit halves, a different argument order, a different struct. */

/* struct stat as x86_64 Linux lays it out (144 bytes; asm/stat.h). */
struct stat64_x86_64 {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, pad0;
    uint64_t st_rdev;
    int64_t  st_size, st_blksize, st_blocks;
    uint64_t st_atime, st_atime_nsec, st_mtime, st_mtime_nsec, st_ctime, st_ctime_nsec;
    int64_t  unused[3];
};

/* int newfstatat(int dirfd, const char *path, struct stat *buf, int flags):
   musl's stat(), fstat(), lstat() and fstatat() on x86_64. */
SYSCALL(sys_newfstatat)
{
    struct inode *i;
    int err = stat_target(a1, a2, a4, &i);
    if (err)
        return err;
    struct stat64_x86_64 st;
    memset(&st, 0, sizeof st);
    st.st_dev = i->mnt ? i->mnt->dev_minor : 1;     /* major 0 (as statx) */
    st.st_ino = i->fs ? i->fs_ino : i->ino;
    st.st_nlink = i->nlink;
    st.st_mode = i->mode;
    st.st_uid = i->uid;
    st.st_gid = i->gid;
    st.st_rdev = i->rdev;           /* (major << 8) | minor: Linux's encoding too */
    st.st_size = i->size;
    st.st_blksize = 4096;
    st.st_blocks = (i->size + 511) / 512;
    st.st_atime = i->atime ? i->atime : i->mtime;
    st.st_mtime = i->mtime;
    st.st_ctime = i->ctime ? i->ctime : i->mtime;
    return copy_to_user((void *)a3, &st, sizeof st);
}
SYSCALL(sys_stat_x64)  { return sys_newfstatat(AT_FDCWD, a1, a2, 0, 0, 0); }
SYSCALL(sys_lstat_x64) { return sys_newfstatat(AT_FDCWD, a1, a2, AT_SYMLINK_NOFOLLOW, 0, 0); }
SYSCALL(sys_fstat_x64) { return sys_newfstatat(a1, 0, a2, AT_EMPTY_PATH, 0, 0); }

/* mmap(addr, len, prot, flags, fd, offset): the offset is in bytes (mmap2's
   is in pages), and must be page aligned. Lengths and offsets are 32-bit
   inside (mm.c, files): bigger ones are refused, not cut short. */
SYSCALL(sys_mmap_x64)
{
    if (a6 & (PAGE_SIZE - 1))
        return -EINVAL;
    if (a2 > 0xFFFFF000)
        return -ENOMEM;
    if (a6 > 0xFFFFFFFF)
        return -EOVERFLOW;
    return sys_mmap2(a1, a2, a3, a4, a5, a6 / PAGE_SIZE);
}

/* 64-bit file offsets come in one register, not as two halves. */
SYSCALL(sys_lseek_x64)  { return do_seek(a1, (int64_t)a2, a3); }
SYSCALL(sys_pread_x64)  { return (int64_t)a4 < 0 ? -EINVAL : rw_at(a1, a2, a3, 0, (int64_t)a4); }
SYSCALL(sys_pwrite_x64) { return (int64_t)a4 < 0 ? -EINVAL : rw_at(a1, a2, a3, 1, (int64_t)a4); }

/* clone(flags, stack, parent_tid, child_tid, tls): i386 has tls before
   child_tid. */
SYSCALL(sys_clone_x64) { return sys_clone(a1, a2, a3, a5, a4, 0); }

/* nanosleep(req, rem): the timespecs are 64-bit, like i386's time64 ones.
   Linux measures it on CLOCK_MONOTONIC. */
SYSCALL(sys_nanosleep_x64) { return sys_clock_nanosleep64(1, 0, a1, a2, 0, 0); }

/* struct rlimit is two 64-bit words here: the same as prlimit64's. */
SYSCALL(sys_getrlimit_x64) { return sys_prlimit64(0, a1, 0, a2, 0, 0); }
SYSCALL(sys_setrlimit_x64) { return sys_prlimit64(0, a1, a2, 0, 0, 0); }

SYSCALL(sys_accept_x64) { return sys_accept4(a1, a2, a3, 0, 0, 0); }

/* arch_prctl(code, addr): the thread's FS base, which is thread-local
   storage on x86_64 (musl sets it once, before main). It lives in
   thread->tls[0] and is written to the MSR at every switch (gdt.c). */
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
SYSCALL(sys_arch_prctl)
{
    struct thread *t = thread_current();
    switch (a1) {
    case ARCH_SET_FS:
        if (a2 >= USER_ADDR_END)
            return -EPERM;          /* (a non-canonical one would fault in wrmsr) */
        t->tls[0] = a2;
        thread_load_tls(t);
        return 0;
    case ARCH_GET_FS:
        return copy_to_user((void *)a2, &t->tls[0], sizeof t->tls[0]);
    default:
        return -EINVAL;
    }
}
#endif

/* --- the table ---------------------------------------------------------------- */

static const syscall_fn syscall_table[] U = {     /* (x86_64: unused, dropped) */
    [NR_exit]       = sys_exit,
    [NR_fork]       = sys_fork,
    [NR_creat]      = sys_creat,
    [NR_link]       = sys_link,
    [NR_unlink]     = sys_unlink,
    [NR_time]       = sys_time,
    [NR_mknod]      = sys_mknod,
    [NR_chmod]      = sys_chmod,
    [NR_getuid]     = sys_getuid,
    [NR_getgid]     = sys_getgid,
    [NR_geteuid]    = sys_geteuid,
    [NR_getegid]    = sys_getegid,
    [NR_sync]       = sys_sync,
    [NR_mount]      = sys_mount,
    [NR_umount2]    = sys_umount2,
    [NR_sethostname] = sys_sethostname,
    [NR_reboot]     = sys_reboot,
    [NR_vhangup]    = sys_vhangup,
    [NR_flock]      = sys_flock,
    [NR_getpriority] = sys_getpriority,
    [NR_setpriority] = sys_setpriority,
    [NR_rename]     = sys_rename,
    [NR_mkdir]      = sys_mkdir,
    [NR_rmdir]      = sys_rmdir,
    [NR_setrlimit]  = sys_setrlimit,
    [NR_getrlimit]  = sys_getrlimit,
    [NR_gettimeofday] = sys_gettimeofday,
    [NR_symlink]    = sys_symlink,
    [NR_truncate]   = sys_truncate,
    [NR_ftruncate]  = sys_ftruncate,
    [NR_fchmod]     = sys_fchmod,
    [NR_sysinfo]    = sys_sysinfo,
    [NR_ipc]        = sys_ipc,
    [NR_fsync]      = sys_fsync,
    [NR_clone]      = sys_clone,
    [NR_uname]      = sys_uname,
    [NR_times]      = sys_times,
    [NR_syncfs]     = sys_syncfs,
    [NR_chroot]     = sys_chroot,
    [NR_setxattr] = sys_setxattr,
    [NR_lsetxattr] = sys_lsetxattr,
    [NR_fsetxattr] = sys_fsetxattr,
    [NR_getxattr] = sys_getxattr,
    [NR_lgetxattr] = sys_lgetxattr,
    [NR_fgetxattr] = sys_fgetxattr,
    [NR_listxattr] = sys_listxattr,
    [NR_llistxattr] = sys_llistxattr,
    [NR_flistxattr] = sys_flistxattr,
    [NR_removexattr] = sys_removexattr,
    [NR_lremovexattr] = sys_lremovexattr,
    [NR_fremovexattr] = sys_fremovexattr,
    [NR_inotify_init] = sys_inotify_init,
    [NR_inotify_init1] = sys_inotify_init1,
    [NR_inotify_add_watch] = sys_inotify_add_watch,
    [NR_inotify_rm_watch] = sys_inotify_rm_watch,
    [NR_shmget]     = sys_shmget,
    [NR_shmctl]     = sys_shmctl,
    [NR_shmat]      = sys_shmat,
    [NR_shmdt]      = sys_shmdt,
    [NR_semget]     = sys_semget,
    [NR_semctl]     = sys_semctl,
    [NR_semtimedop_time64] = sys_semtimedop,
    [NR_msgget]     = sys_msgget,
    [NR_msgsnd]     = sys_msgsnd,
    [NR_msgrcv]     = sys_msgrcv,
    [NR_msgctl]     = sys_msgctl,
    [NR_fadvise64_64] = sys_fadvise64_64,
    [NR_pivot_root] = sys_pivot_root,
    [NR_personality] = sys_personality,
    [NR_capget]     = sys_capget,
    [NR_capset]     = sys_capset,
    [NR_unshare]    = sys_unshare,
    [NR_setns]      = sys_setns,
    [NR_swapon]     = sys_swapon,
    [NR_syslog]     = sys_syslog,
    [NR_swapoff]    = sys_swapon,
    [NR_adjtimex]   = sys_adjtimex,
    [NR_clock_adjtime] = sys_clock_adjtime,
    [NR_clock_adjtime64] = sys_clock_adjtime64,
    [NR_clock_settime64] = sys_clock_settime64,
    [NR_getrusage]  = sys_getrusage,
    [NR_mprotect]   = sys_mprotect,
    [NR__newselect] = sys_select,
    [NR_fdatasync]  = sys_fsync,
    [NR_nanosleep]  = sys_nanosleep,
    [NR_poll]       = sys_poll,
    [NR_epoll_create] = sys_epoll_create,
    [NR_epoll_create1] = sys_epoll_create1,
    [NR_epoll_ctl]  = sys_epoll_ctl,
    [NR_epoll_wait] = sys_epoll_wait,
    [NR_epoll_pwait] = sys_epoll_pwait,
    [NR_ugetrlimit] = sys_getrlimit,
    [NR_truncate64] = sys_truncate64,
    [NR_ftruncate64] = sys_ftruncate64,
    [NR_fallocate]  = sys_fallocate,
    [NR_lchown32]   = sys_lchown32,
    [NR_getuid32]   = sys_getuid,
    [NR_getgid32]   = sys_getgid,
    [NR_geteuid32]  = sys_geteuid,
    [NR_getegid32]  = sys_getegid,
    [NR_setreuid32] = sys_setreuid,
    [NR_setregid32] = sys_setregid,
    [NR_getgroups32] = sys_getgroups,
    [NR_setgroups32] = sys_setgroups,
    [NR_fchown32]   = sys_fchown32,
    [NR_setresuid32] = sys_setresuid,
    [NR_getresuid32] = sys_getresuid,
    [NR_setresgid32] = sys_setresgid,
    [NR_getresgid32] = sys_getresgid,
    [NR_chown32]    = sys_chown32,
    [NR_setuid32]   = sys_setuid,
    [NR_setgid32]   = sys_setgid,
    [NR_setfsuid32] = sys_setfsuid,
    [NR_setfsgid32] = sys_setfsgid,
    [NR_mincore]    = sys_mincore,
    [NR_readahead]  = sys_nop,
    [NR_madvise]    = sys_nop,
    [NR_sched_getaffinity] = sys_getaffinity,
    [NR_set_thread_area] = sys_set_thread_area,
    [NR_get_thread_area] = sys_get_thread_area,
    [NR_set_tid_address] = sys_set_tid_address,
    [NR_timer_create]    = sys_timer_create,
    [NR_timer_settime]   = sys_timer_settime,
    [NR_timer_gettime]   = sys_timer_gettime,
    [NR_timer_getoverrun] = sys_timer_getoverrun,
    [NR_timer_delete]    = sys_timer_delete,
    [NR_statfs64]   = sys_statfs64,
    [NR_fstatfs64]  = sys_fstatfs64,
    [NR_clock_gettime]   = sys_clock_gettime,
    [NR_clock_getres]    = sys_clock_getres,
    [NR_clock_nanosleep] = sys_clock_nanosleep,
    [NR_mkdirat]    = sys_mkdirat,
    [NR_mknodat]    = sys_mknodat,
    [NR_fchownat]   = sys_fchownat,
    [NR_unlinkat]   = sys_unlinkat,
    [NR_renameat]   = sys_renameat,
    [NR_linkat]     = sys_link,
    [NR_symlinkat]  = sys_symlinkat,
    [NR_fchmodat]   = sys_fchmodat,
    [NR_set_robust_list] = sys_nop,
    [NR_pselect6]   = sys_pselect32,
    [NR_ppoll]      = sys_ppoll32,
    [NR_utimensat]  = sys_utimensat,
    [NR_prlimit64]  = sys_prlimit64,
    [NR_renameat2]  = sys_renameat2,
    [NR_getrandom]  = sys_getrandom,
    [NR_memfd_create] = sys_memfd_create,
    [NR_socketcall] = sys_socketcall,
    [NR_socket]     = sys_socket,
    [NR_socketpair] = sys_socketpair,
    [NR_bind]       = sys_bind,
    [NR_connect]    = sys_connect,
    [NR_listen]     = sys_listen,
    [NR_accept4]    = sys_accept4,
    [NR_getsockopt] = sys_getsockopt,
    [NR_setsockopt] = sys_setsockopt,
    [NR_getsockname] = sys_getsockname,
    [NR_getpeername] = sys_getpeername,
    [NR_sendto]     = sys_sendto,
    [NR_sendmsg]    = sys_sendmsg,
    [NR_recvfrom]   = sys_recvfrom,
    [NR_recvmsg]    = sys_recvmsg,
    [NR_shutdown]   = sys_shutdown,
    [NR_clock_gettime64]  = sys_clock_gettime64,
    [NR_clock_getres_time64] = sys_clock_getres64,
    [NR_clock_nanosleep_time64] = sys_clock_nanosleep64,
    [NR_timer_gettime64] = sys_timer_gettime64,
    [NR_timer_settime64] = sys_timer_settime64,
    [NR_utimensat_time64] = sys_utimensat64,
    [NR_pselect6_time64] = sys_pselect64,
    [NR_ppoll_time64]    = sys_ppoll64,
    [NR_read]       = sys_read,
    [NR_write]      = sys_write,
    [NR_open]       = sys_open,
    [NR_close]      = sys_close,
    [NR_waitpid]    = sys_waitpid,
    [NR_execve]     = sys_execve,
    [NR_chdir]      = sys_chdir,
    [NR_access]     = sys_access,
    [NR_kill]       = sys_kill,
    [NR_dup]        = sys_dup,
    [NR_pipe]       = sys_pipe,
    [NR_ioctl]      = sys_ioctl,
    [NR_fcntl]      = sys_fcntl,
    [NR_setpgid]    = sys_setpgid,
    [NR_umask]      = sys_umask,
    [NR_dup2]       = sys_dup2,
    [NR_readlink]   = sys_readlink,
    [NR_fchdir]     = sys_fchdir,
    [NR_readv]      = sys_readv,
    [NR_writev]     = sys_writev,
    [NR_pread64]    = sys_pread64,
    [NR_pwrite64]   = sys_pwrite64,
    [NR_sendfile64] = sys_sendfile,
    [NR_copy_file_range] = sys_copy_file_range,
    [NR_getcwd]     = sys_getcwd,
    [NR_getdents64] = sys_getdents64,
    [NR_fcntl64]    = sys_fcntl,
    [NR_readlinkat] = sys_readlinkat,
    [NR_faccessat]  = sys_faccessat,
    [NR_dup3]       = sys_dup3,
    [NR_pipe2]      = sys_pipe2,
    [NR_faccessat2] = sys_faccessat,
    [NR_lseek]      = sys_lseek,
    [NR_getpid]     = sys_getpid,
    [NR_brk]        = sys_brk,
    [NR_getppid]    = sys_getppid,
    [NR_getpgrp]    = sys_getpgrp,
    [NR_setsid]     = sys_setsid,
    [NR_munmap]     = sys_munmap,
    [NR_wait4]      = sys_wait4,
    [NR_sigreturn]  = sys_sigreturn,
    [NR_getpgid]    = sys_getpgid,
    [NR_getsid]     = sys_getsid,
    [NR_sched_yield] = sys_sched_yield,
    [NR_rt_sigreturn]   = sys_rt_sigreturn,
    [NR_rt_sigaction]   = sys_rt_sigaction,
    [NR_rt_sigprocmask] = sys_rt_sigprocmask,
    [NR_alarm]      = sys_alarm,
    [NR_pause]      = sys_pause,
    [NR_setitimer]  = sys_setitimer,
    [NR_getitimer]  = sys_getitimer,
    [NR_rt_sigpending]  = sys_rt_sigpending,
    [NR_rt_sigsuspend]  = sys_rt_sigsuspend,
    [NR_rt_sigtimedwait] = sys_rt_sigtimedwait,
    [NR_rt_sigtimedwait_time64] = sys_rt_sigtimedwait64,
    [NR_tkill]      = sys_tkill,
    [NR_tgkill]     = sys_tgkill,
    [NR__llseek]    = sys_llseek,
    [NR_vfork]      = sys_fork,
    [NR_mmap2]      = sys_mmap2,
    [NR_mremap]     = sys_mremap,
    [NR_prctl]      = sys_prctl,
    [NR_eventfd2]   = sys_eventfd2,
    [NR_membarrier] = sys_membarrier,
    [NR_exit_group] = sys_exit_group,
    [NR_gettid]     = sys_gettid,
    [NR_futex]      = sys_futex,
    [NR_futex_time64] = sys_futex_time64,
    [NR_openat]     = sys_openat,
    [NR_statx]      = sys_statx,
#ifdef SELFTEST
    [NR_uaccess_test] = sys_uaccess_test,
#endif
};

#ifdef ARCH_X86_64
/* Linux x86_64 numbers (arch/x86/entry/syscalls/syscall_64.tbl, our
   include/syscall64.h). A gap means "not here yet": -ENOSYS, and a line in
   the serial log. Calls whose structs differ from i386 (statfs, sigevent,
   itimerval, timeval, ...) stay gaps until they get an adapter: -ENOSYS is
   better than a struct of the wrong shape. */
static const syscall_fn syscall_table64[] = {
    [NR64_read]         = sys_read,
    [NR64_write]        = sys_write,
    [NR64_open]         = sys_open,
    [NR64_close]        = sys_close,
    [NR64_stat]         = sys_stat_x64,
    [NR64_fstat]        = sys_fstat_x64,
    [NR64_lstat]        = sys_lstat_x64,
    [NR64_poll]         = sys_poll,
    [NR64_lseek]        = sys_lseek_x64,
    [NR64_mmap]         = sys_mmap_x64,
    [NR64_mprotect]     = sys_mprotect,
    [NR64_munmap]       = sys_munmap,
    [NR64_brk]          = sys_brk,
    [NR64_rt_sigaction] = sys_rt_sigaction,
    [NR64_rt_sigprocmask] = sys_rt_sigprocmask,
    [NR64_rt_sigreturn] = sys_rt_sigreturn,
    [NR64_ioctl]        = sys_ioctl,
    [NR64_pread64]      = sys_pread_x64,
    [NR64_pwrite64]     = sys_pwrite_x64,
    [NR64_readv]        = sys_readv,
    [NR64_writev]       = sys_writev,
    [NR64_access]       = sys_access,
    [NR64_pipe]         = sys_pipe,
    [NR64_sched_yield]  = sys_sched_yield,
    [NR64_mremap]       = sys_mremap,
    [NR64_mincore]      = sys_mincore,
    [NR64_madvise]      = sys_nop,
    [NR64_dup]          = sys_dup,
    [NR64_dup2]         = sys_dup2,
    [NR64_nanosleep]    = sys_nanosleep_x64,
    [NR64_pause]        = sys_pause,
    [NR64_getitimer]    = sys_getitimer,
    [NR64_alarm]        = sys_alarm,
    [NR64_setitimer]    = sys_setitimer,
    [NR64_getpid]       = sys_getpid,
    [NR64_sendfile]     = sys_sendfile,
    [NR64_copy_file_range] = sys_copy_file_range,
    [NR64_socket]       = sys_socket,
    [NR64_connect]      = sys_connect,
    [NR64_accept]       = sys_accept_x64,
    [NR64_sendto]       = sys_sendto,
    [NR64_recvfrom]     = sys_recvfrom,
    [NR64_sendmsg]      = sys_sendmsg,
    [NR64_recvmsg]      = sys_recvmsg,
    [NR64_shutdown]     = sys_shutdown,
    [NR64_bind]         = sys_bind,
    [NR64_listen]       = sys_listen,
    [NR64_getsockname]  = sys_getsockname,
    [NR64_getpeername]  = sys_getpeername,
    [NR64_socketpair]   = sys_socketpair,
    [NR64_setsockopt]   = sys_setsockopt,
    [NR64_getsockopt]   = sys_getsockopt,
    [NR64_clone]        = sys_clone_x64,
    [NR64_fork]         = sys_fork,
    [NR64_vfork]        = sys_fork,
    [NR64_execve]       = sys_execve,
    [NR64_exit]         = sys_exit,
    [NR64_wait4]        = sys_wait4,
    [NR64_kill]         = sys_kill,
    [NR64_uname]        = sys_uname,
    [NR64_fcntl]        = sys_fcntl,
    [NR64_flock]        = sys_flock,
    [NR64_fsync]        = sys_fsync,
    [NR64_fdatasync]    = sys_fsync,
    [NR64_truncate]     = sys_truncate,
    [NR64_ftruncate]    = sys_ftruncate,
    [NR64_getcwd]       = sys_getcwd,
    [NR64_chdir]        = sys_chdir,
    [NR64_fchdir]       = sys_fchdir,
    [NR64_rename]       = sys_rename,
    [NR64_mkdir]        = sys_mkdir,
    [NR64_rmdir]        = sys_rmdir,
    [NR64_creat]        = sys_creat,
    [NR64_link]         = sys_link,
    [NR64_unlink]       = sys_unlink,
    [NR64_symlink]      = sys_symlink,
    [NR64_readlink]     = sys_readlink,
    [NR64_chmod]        = sys_chmod,
    [NR64_fchmod]       = sys_fchmod,
    [NR64_chown]        = sys_chown32,
    [NR64_fchown]       = sys_fchown32,
    [NR64_lchown]       = sys_lchown32,
    [NR64_umask]        = sys_umask,
    [NR64_getrlimit]    = sys_getrlimit_x64,
    [NR64_getrusage]    = sys_getrusage,
    [NR64_times]        = sys_times,
    [NR64_gettimeofday] = sys_gettimeofday,
    [NR64_time]         = sys_time,
    [NR64_clock_settime] = sys_clock_settime64,
    [NR64_adjtimex]     = sys_adjtimex,
    [NR64_clock_adjtime] = sys_clock_adjtime,
    [NR64_syncfs]       = sys_syncfs,
    [NR64_chroot]       = sys_chroot,
    [NR64_setxattr] = sys_setxattr,
    [NR64_lsetxattr] = sys_lsetxattr,
    [NR64_fsetxattr] = sys_fsetxattr,
    [NR64_getxattr] = sys_getxattr,
    [NR64_lgetxattr] = sys_lgetxattr,
    [NR64_fgetxattr] = sys_fgetxattr,
    [NR64_listxattr] = sys_listxattr,
    [NR64_llistxattr] = sys_llistxattr,
    [NR64_flistxattr] = sys_flistxattr,
    [NR64_removexattr] = sys_removexattr,
    [NR64_lremovexattr] = sys_lremovexattr,
    [NR64_fremovexattr] = sys_fremovexattr,
    [NR64_inotify_init] = sys_inotify_init,
    [NR64_inotify_init1] = sys_inotify_init1,
    [NR64_inotify_add_watch] = sys_inotify_add_watch,
    [NR64_inotify_rm_watch] = sys_inotify_rm_watch,
    [NR64_shmget]       = sys_shmget,
    [NR64_shmat]        = sys_shmat,
    [NR64_shmctl]       = sys_shmctl,
    [NR64_shmdt]        = sys_shmdt,
    [NR64_semget]       = sys_semget,
    [NR64_semop]        = sys_semop,
    [NR64_semctl]       = sys_semctl,
    [NR64_semtimedop]   = sys_semtimedop,
    [NR64_msgget]       = sys_msgget,
    [NR64_msgsnd]       = sys_msgsnd,
    [NR64_msgrcv]       = sys_msgrcv,
    [NR64_msgctl]       = sys_msgctl,
    [NR64_fadvise64]    = sys_fadvise64,
    [NR64_fallocate]    = sys_fallocate64,
    [NR64_statfs]       = sys_statfs,
    [NR64_fstatfs]      = sys_fstatfs,
    [NR64_select]       = sys_select,
    [NR64_pivot_root]   = sys_pivot_root,
    [NR64_personality]  = sys_personality,
    [NR64_capget]       = sys_capget,
    [NR64_capset]       = sys_capset,
    [NR64_unshare]      = sys_unshare,
    [NR64_setns]        = sys_setns,
    [NR64_swapon]       = sys_swapon,
    [NR64_syslog]       = sys_syslog,
    [NR64_swapoff]      = sys_swapon,
    [NR64_sysinfo]      = sys_sysinfo,
    [NR64_getuid]       = sys_getuid,
    [NR64_getgid]       = sys_getgid,
    [NR64_setuid]       = sys_setuid,
    [NR64_setgid]       = sys_setgid,
    [NR64_geteuid]      = sys_geteuid,
    [NR64_getegid]      = sys_getegid,
    [NR64_setpgid]      = sys_setpgid,
    [NR64_getppid]      = sys_getppid,
    [NR64_getpgrp]      = sys_getpgrp,
    [NR64_setsid]       = sys_setsid,
    [NR64_setreuid]     = sys_setreuid,
    [NR64_setregid]     = sys_setregid,
    [NR64_getgroups]    = sys_getgroups,
    [NR64_setgroups]    = sys_setgroups,
    [NR64_setresuid]    = sys_setresuid,
    [NR64_getresuid]    = sys_getresuid,
    [NR64_setresgid]    = sys_setresgid,
    [NR64_getresgid]    = sys_getresgid,
    [NR64_getpgid]      = sys_getpgid,
    [NR64_setfsuid]     = sys_setfsuid,
    [NR64_setfsgid]     = sys_setfsgid,
    [NR64_getsid]       = sys_getsid,
    [NR64_rt_sigpending] = sys_rt_sigpending,
    [NR64_rt_sigtimedwait] = sys_rt_sigtimedwait64,
    [NR64_rt_sigsuspend] = sys_rt_sigsuspend,
    [NR64_mknod]        = sys_mknod,
    [NR64_getpriority]  = sys_getpriority,
    [NR64_setpriority]  = sys_setpriority,
    [NR64_prctl]        = sys_prctl,
    [NR64_arch_prctl]   = sys_arch_prctl,
    [NR64_setrlimit]    = sys_setrlimit_x64,
    [NR64_sync]         = sys_sync,
    [NR64_mount]        = sys_mount,
    [NR64_umount2]      = sys_umount2,
    [NR64_reboot]       = sys_reboot,
    [NR64_sethostname]  = sys_sethostname,
    [NR64_gettid]       = sys_gettid,
    [NR64_readahead]    = sys_nop,
    [NR64_tkill]        = sys_tkill,
    [NR64_futex]        = sys_futex_time64,
    [NR64_sched_getaffinity] = sys_getaffinity,
    [NR64_epoll_create] = sys_epoll_create,
    [NR64_getdents64]   = sys_getdents64,
    [NR64_set_tid_address] = sys_set_tid_address,
    [NR64_timer_create] = sys_timer_create,
    [NR64_timer_settime] = sys_timer_settime64,
    [NR64_timer_gettime] = sys_timer_gettime64,
    [NR64_timer_getoverrun] = sys_timer_getoverrun,
    [NR64_timer_delete] = sys_timer_delete,
    [NR64_clock_gettime] = sys_clock_gettime64,
    [NR64_clock_getres] = sys_clock_getres64,
    [NR64_clock_nanosleep] = sys_clock_nanosleep64,
    [NR64_exit_group]   = sys_exit_group,
    [NR64_epoll_wait]   = sys_epoll_wait,
    [NR64_epoll_ctl]    = sys_epoll_ctl,
    [NR64_tgkill]       = sys_tgkill,
    [NR64_openat]       = sys_openat,
    [NR64_mkdirat]      = sys_mkdirat,
    [NR64_mknodat]      = sys_mknodat,
    [NR64_fchownat]     = sys_fchownat,
    [NR64_newfstatat]   = sys_newfstatat,
    [NR64_unlinkat]     = sys_unlinkat,
    [NR64_renameat]     = sys_renameat,
    [NR64_linkat]       = sys_link,
    [NR64_symlinkat]    = sys_symlinkat,
    [NR64_readlinkat]   = sys_readlinkat,
    [NR64_fchmodat]     = sys_fchmodat,
    [NR64_faccessat]    = sys_faccessat,
    [NR64_pselect6]     = sys_pselect64,
    [NR64_ppoll]        = sys_ppoll64,
    [NR64_set_robust_list] = sys_nop,
    [NR64_utimensat]    = sys_utimensat64,
    [NR64_epoll_pwait]  = sys_epoll_pwait,
    [NR64_eventfd2]     = sys_eventfd2,
    [NR64_epoll_create1] = sys_epoll_create1,
    [NR64_dup3]         = sys_dup3,
    [NR64_pipe2]        = sys_pipe2,
    [NR64_accept4]      = sys_accept4,
    [NR64_prlimit64]    = sys_prlimit64,
    [NR64_renameat2]    = sys_renameat2,
    [NR64_getrandom]    = sys_getrandom,
    [NR64_memfd_create] = sys_memfd_create,
    [NR64_membarrier]   = sys_membarrier,
    [NR64_statx]        = sys_statx,
    [NR64_faccessat2]   = sys_faccessat,
};
#define TABLE syscall_table64
#else
#define TABLE syscall_table
#endif

#define NSYSCALLS (sizeof TABLE / sizeof TABLE[0])

intptr_t syscall_dispatch(uint32_t nr, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                          uintptr_t a4, uintptr_t a5, uintptr_t a6)
{
    if (nr < NSYSCALLS && TABLE[nr])
        return TABLE[nr](a1, a2, a3, a4, a5, a6);

    /* Not implemented. Log it to serial — once per number — so running
       real programs shows us which syscalls to write next (docs/DESIGN.md).
       Many programs try a new call first and fall back to an old one, so
       -ENOSYS is often fine. */
    static uint32_t logged[512 / 32];
    const char *who = thread_current()->name;
    int again = nr < 512 && (logged[nr / 32] & (1u << (nr % 32)));
    int watch = who[0] == 'f' && who[1] == 'i' && who[2] == 'r'; /* firefox */
    if (nr < 512 && !again)
        logged[nr / 32] |= 1u << (nr % 32);
    if (!again || watch)
        klog("syscall %lu not implemented (%s)\n", nr, who);
    return -ENOSYS;
}
