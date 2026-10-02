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
#include <mm.h>
#include <proc.h>
#include <serial.h>
#include <signal.h>
#include <syscalls.h>
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

/* The directory a *at() call's dirfd means: NULL for AT_FDCWD (the
   current directory), else the open directory. 0, or -errno. */
static int dir_of(uint32_t dirfd, struct inode **dir)
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
static int get_path(char *dst, uint32_t user_path)
{
    long n = strncpy_from_user(dst, (const char *)user_path, PATH_MAX);
    if (n < 0)
        return n;
    return n == 0 ? -ENOENT : 0;    /* "" is not a file name */
}

static int32_t do_open(struct inode *dir, uint32_t user_path, uint32_t flags)
{
    char path[PATH_MAX];
    int err = get_path(path, user_path);
    if (err)
        return err;
    struct file *f;
    err = vfs_open_at(dir, path, flags, &f);
    if (err)
        return err;
    int fd = fd_install(f);
    if (fd < 0)
        file_put(f);
    else if (flags & O_CLOEXEC)
        proc_current()->cloexec |= 1u << fd;    /* closed by execve */
    return fd;
}

/* int open(const char *path, int flags, mode_t mode) */
SYSCALL(sys_open)
{
    return do_open(NULL, a1, a2);
}

/* int openat(int dirfd, const char *path, int flags, mode_t mode) */
SYSCALL(sys_openat)
{
    struct inode *dir;
    int err = dir_of(a1, &dir);
    return err ? err : do_open(dir, a2, a3);
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
        struct inode *dir;
        int err = get_path(path, a2);
        if (!err)
            err = dir_of(a1, &dir);
        if (!err)                   /* lstat doesn't follow a final symlink */
            err = vfs_lookup_at(dir, path, !(a3 & AT_SYMLINK_NOFOLLOW), &i);
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
    st.stx_rdev_major = i->rdev >> 8;
    st.stx_rdev_minor = i->rdev & 0xFF;
    return copy_to_user((void *)a5, &st, sizeof st);
}

/* int pipe(int fds[2]) / pipe2(int fds[2], int flags) */
static int32_t do_pipe(uint32_t user_fds, uint32_t flags)
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
        proc_current()->cloexec |= (1u << fds[0]) | (1u << fds[1]);
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
        proc_current()->cloexec |= 1u << newfd;
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
                    p->cloexec |= 1u << fd;
                return fd;
            }
        }
        return -EMFILE;
    case F_GETFD:
        return (p->cloexec >> a1) & 1 ? FD_CLOEXEC : 0;
    case F_SETFD:
        if (a3 & FD_CLOEXEC)
            p->cloexec |= 1u << a1;
        else
            p->cloexec &= ~(1u << a1);
        return 0;
    case F_GETFL:
        return f->flags;
    case F_SETFL:                       /* only these two may change */
        f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | (a3 & (O_APPEND | O_NONBLOCK));
        return 0;
    default:
        return -EINVAL;
    }
}

/* ssize_t getdents64(int fd, struct linux_dirent64 *buf, size_t count)
   — reading a directory (what `ls` uses). Each entry: inode number,
   offset of the next, its own length (8-aligned), type, name + NUL.
   The file remembers the next entry, so a big directory read in many
   calls costs O(entries), not O(entries squared). */
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

    uint8_t rec[19 + NAME_MAX + 1 + 8] __attribute__((aligned(8)));
    uint32_t written = 0;
    for (;;) {
        const char *name;
        uint32_t len;
        struct inode *i;
        if (f->pos == 0) {              /* "." and ".." come first */
            name = ".", len = 1, i = dir;
        } else if (f->pos == 1) {
            name = "..", len = 2, i = dir->parent;
        } else {
            i = f->priv;
            if (!i)
                break;                  /* that was all */
            name = i->name, len = i->name_len;
        }
        uint32_t reclen = (19 + len + 1 + 7) & ~7u;
        if (written + reclen > a3) {
            if (written == 0)
                return -EINVAL;         /* buffer too small for one entry */
            break;
        }
        uint64_t ino = i->ino, off = f->pos + 1;
        memcpy(rec, &ino, 8);
        memcpy(rec + 8, &off, 8);
        memcpy(rec + 16, &reclen, 2);
        rec[18] = dirent_type(i->mode);
        memcpy(rec + 19, name, len);
        memset(rec + 19 + len, 0, reclen - 19 - len);
        if (copy_to_user((uint8_t *)a2 + written, rec, reclen) < 0)
            return -EFAULT;
        written += reclen;
        if (f->pos >= 2)
            f->priv = i->next_sibling;
        f->pos++;
    }
    return written;
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
    proc_current()->cwd = i;
    return 0;
}

SYSCALL(sys_fchdir)
{
    struct file *f = fd_get(a1);
    if (!f)
        return -EBADF;
    if (!S_ISDIR(f->inode->mode))
        return -ENOTDIR;
    proc_current()->cwd = f->inode;
    return 0;
}

/* long getcwd(char *buf, size_t size): the current directory's full path,
   built by walking up to the root. Returns its length with the NUL. */
SYSCALL(sys_getcwd)
{
    char path[PATH_MAX];
    uint32_t at = PATH_MAX;
    path[--at] = '\0';
    struct inode *i = proc_current()->cwd;
    if (i == vfs_root())
        path[--at] = '/';
    for (; i != vfs_root(); i = i->parent) {
        if (i->name_len + 1 > at)
            return -ENAMETOOLONG;
        at -= i->name_len;
        memcpy(path + at, i->name, i->name_len);
        path[--at] = '/';
    }
    uint32_t len = PATH_MAX - at;
    if (len > a2)
        return -ERANGE;
    if (copy_to_user((void *)a1, path + at, len) < 0)
        return -EFAULT;
    return len;
}

/* ssize_t writev / readv(int fd, const struct iovec *iov, int count):
   several buffers in one call (musl's printf writes this way). Stops at
   the first short transfer, like Linux. */
static int32_t do_iov(uint32_t fd, uint32_t user_iov, uint32_t count, int writing)
{
    struct file *f = fd_get(fd);
    if (!f)
        return -EBADF;
    if (count > 1024)
        return -EINVAL;
    int32_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t base, len;
        if (get_user32(&base, user_iov + i * 8) < 0 ||
            get_user32(&len, user_iov + i * 8 + 4) < 0)
            return total ? total : -EFAULT;
        if (len == 0)
            continue;
        if (!user_range_ok(base, len))
            return total ? total : -EFAULT;
        int32_t n = writing ? f->ops->write(f, (const void *)base, len)
                            : f->ops->read(f, (void *)base, len);
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
static int32_t do_readlink(struct inode *dir, uint32_t user_path, uint32_t buf, uint32_t size)
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
    uint32_t n = i->size < size ? i->size : size;
    if (copy_to_user((void *)buf, i->data, n) < 0)
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
   with no x bit, and writing the read-only initramfs. */
#define X_OK 1
#define W_OK 2

static int32_t do_access(struct inode *dir, uint32_t user_path, uint32_t mode)
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
    if ((mode & W_OK) && !i->ops && !S_ISDIR(i->mode))
        return -EROFS;
    return 0;
}

SYSCALL(sys_access) { return do_access(NULL, a1, a2); }
SYSCALL(sys_faccessat)
{
    struct inode *dir;
    int err = dir_of(a1, &dir);
    return err ? err : do_access(dir, a2, a3);
}

/* Anything that would change the file system: the initramfs is read-only
   (a writable tmpfs comes later). A clear error beats "not implemented". */
SYSCALL(sys_rofs) { return -EROFS; }

/* fsync, sync, madvise, set_robust_list...: nothing to do, success. */
SYSCALL(sys_nop) { return 0; }

/* No networking yet (step 10), so no fd is a socket. Programs ask anyway:
   bash checks whether its input is a network connection (getpeername). */
SYSCALL(sys_notsock) { return fd_get(a1) ? -ENOTSOCK : -EBADF; }

/* socketcall(call, args): the old all-in-one socket syscall. */
SYSCALL(sys_socketcall) { return a1 == 7 ? -ENOTSOCK : -ENOSYS; }  /* 7: getpeername */

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
SYSCALL(sys_sigreturn)      { return sys_sigreturn_impl(0); }
SYSCALL(sys_rt_sigreturn)   { return sys_sigreturn_impl(1); }
SYSCALL(sys_kill)           { return sys_kill_impl((int32_t)a1, a2); }
SYSCALL(sys_rt_sigsuspend)  { return sys_rt_sigsuspend_impl(a1, a2); }
SYSCALL(sys_rt_sigpending)  { return sys_rt_sigpending_impl(a1, a2); }

/* tkill(tid, sig) / tgkill(tgid, tid, sig): one process per thread here. */
SYSCALL(sys_tkill)
{
    struct process *p = proc_find(a1);
    return p ? signal_send(p, a2) : -ESRCH;
}

SYSCALL(sys_tgkill)
{
    struct process *p = proc_find(a2);
    return p ? signal_send(p, a3) : -ESRCH;
}

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

/* void exit(int status) — and exit_group, which ends all threads of a
   process. With one thread per program, they're the same. */
SYSCALL(sys_exit)
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
    return proc_wait4((int32_t)a1, a2, (int32_t)a3);
}

/* void *brk(void *addr) — the raw syscall: returns the (new) break. */
SYSCALL(sys_brk)
{
    return (int32_t)mm_brk(a1);
}

/* void *mmap2(addr, len, prot, flags, fd, pgoffset): anonymous memory, or
   a file (the dynamic linker maps shared libraries this way). pgoffset
   counts 4 KiB pages. */
SYSCALL(sys_mmap2)
{
    struct inode *file = NULL;
    if (!(a4 & 0x20)) {                 /* not MAP_ANONYMOUS */
        struct file *f = fd_get(a5);
        if (!f)
            return -EBADF;
        file = f->inode;
    }
    return mm_mmap(a1, a2, a3, a4, file, a6 * 4096);
}

/* int munmap(void *addr, size_t len) */
SYSCALL(sys_munmap)
{
    return mm_munmap(a1, a2);
}

/* int mprotect(void *addr, size_t len, int prot) */
SYSCALL(sys_mprotect)
{
    return mm_mprotect(a1, a2, a3);
}

/* clone(flags, new_stack, parent_tid *, tls, child_tid *): like fork, on a
   new stack if one is given (musl's posix_spawn). Threads (CLONE_THREAD)
   come later. */
SYSCALL(sys_clone)
{
    return proc_clone(a1, a2, a3, a4);
}

/* --- the table ---------------------------------------------------------------- */

static const syscall_fn syscall_table[] = {
    [NR_exit]       = sys_exit,
    [NR_fork]       = sys_fork,
    [NR_creat]      = sys_rofs,
    [NR_link]       = sys_rofs,
    [NR_unlink]     = sys_rofs,
    [NR_time]       = sys_time,
    [NR_mknod]      = sys_rofs,
    [NR_chmod]      = sys_rofs,
    [NR_getuid]     = sys_getid,
    [NR_getgid]     = sys_getid,
    [NR_geteuid]    = sys_getid,
    [NR_getegid]    = sys_getid,
    [NR_sync]       = sys_nop,
    [NR_rename]     = sys_rofs,
    [NR_mkdir]      = sys_rofs,
    [NR_rmdir]      = sys_rofs,
    [NR_setrlimit]  = sys_setrlimit,
    [NR_getrlimit]  = sys_getrlimit,
    [NR_gettimeofday] = sys_gettimeofday,
    [NR_symlink]    = sys_rofs,
    [NR_truncate]   = sys_rofs,
    [NR_ftruncate]  = sys_rofs,
    [NR_fchmod]     = sys_rofs,
    [NR_sysinfo]    = sys_sysinfo,
    [NR_fsync]      = sys_nop,
    [NR_clone]      = sys_clone,
    [NR_uname]      = sys_uname,
    [NR_mprotect]   = sys_mprotect,
    [NR__newselect] = sys_select,
    [NR_fdatasync]  = sys_nop,
    [NR_nanosleep]  = sys_nanosleep,
    [NR_poll]       = sys_poll,
    [NR_ugetrlimit] = sys_getrlimit,
    [NR_lchown32]   = sys_rofs,
    [NR_getuid32]   = sys_getid,
    [NR_getgid32]   = sys_getid,
    [NR_geteuid32]  = sys_getid,
    [NR_getegid32]  = sys_getid,
    [NR_setreuid32] = sys_setid,
    [NR_setregid32] = sys_setid,
    [NR_getgroups32] = sys_getgroups,
    [NR_setgroups32] = sys_nop,
    [NR_fchown32]   = sys_rofs,
    [NR_setresuid32] = sys_setid,
    [NR_getresuid32] = sys_getresid,
    [NR_setresgid32] = sys_setid,
    [NR_getresgid32] = sys_getresid,
    [NR_chown32]    = sys_rofs,
    [NR_setuid32]   = sys_setid,
    [NR_setgid32]   = sys_setid,
    [NR_setfsuid32] = sys_getid,        /* returns the old fs uid: 0 */
    [NR_setfsgid32] = sys_getid,
    [NR_madvise]    = sys_nop,
    [NR_sched_getaffinity] = sys_getaffinity,
    [NR_set_thread_area] = sys_set_thread_area,
    [NR_get_thread_area] = sys_get_thread_area,
    [NR_set_tid_address] = sys_set_tid_address,
    [NR_clock_gettime]   = sys_clock_gettime,
    [NR_clock_getres]    = sys_clock_getres,
    [NR_clock_nanosleep] = sys_clock_nanosleep,
    [NR_mkdirat]    = sys_rofs,
    [NR_fchownat]   = sys_rofs,
    [NR_unlinkat]   = sys_rofs,
    [NR_renameat]   = sys_rofs,
    [NR_linkat]     = sys_rofs,
    [NR_symlinkat]  = sys_rofs,
    [NR_fchmodat]   = sys_rofs,
    [NR_set_robust_list] = sys_nop,
    [NR_pselect6]   = sys_pselect32,
    [NR_ppoll]      = sys_ppoll32,
    [NR_utimensat]  = sys_rofs,
    [NR_prlimit64]  = sys_prlimit64,
    [NR_getrandom]  = sys_getrandom,
    [NR_socketcall] = sys_socketcall,
    [NR_getsockname] = sys_notsock,
    [NR_getpeername] = sys_notsock,
    [NR_clock_gettime64]  = sys_clock_gettime64,
    [NR_clock_getres_time64] = sys_clock_getres64,
    [NR_clock_nanosleep_time64] = sys_clock_nanosleep64,
    [NR_pselect6_time64] = sys_pselect64,
    [NR_ppoll_time64]    = sys_ppoll64,
    [NR_read]       = sys_read,
    [NR_write]      = sys_write,
    [NR_open]       = sys_open,
    [NR_close]      = sys_close,
    [NR_waitpid]    = sys_wait4,
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
    [NR_rt_sigpending]  = sys_rt_sigpending,
    [NR_rt_sigsuspend]  = sys_rt_sigsuspend,
    [NR_tkill]      = sys_tkill,
    [NR_tgkill]     = sys_tgkill,
    [NR__llseek]    = sys_llseek,
    [NR_vfork]      = sys_fork,
    [NR_mmap2]      = sys_mmap2,
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

    /* Not implemented. Log it to serial — once per number — so running
       real programs shows us which syscalls to write next (docs/DESIGN.md).
       Many programs try a new call first and fall back to an old one, so
       -ENOSYS is often fine. */
    static uint32_t logged[512 / 32];
    if (nr < 512 && !(logged[nr / 32] & (1u << (nr % 32)))) {
        logged[nr / 32] |= 1u << (nr % 32);
        klog("syscall %lu not implemented (%s)\n", nr, thread_current()->name);
    }
    return -ENOSYS;
}
