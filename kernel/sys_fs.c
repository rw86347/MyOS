/*
 * sys_fs.c — system calls that change the file tree
 *
 *   mkdir, rmdir, unlink, rename, symlink, mknod   names in directories
 *   chmod, chown, utimensat, truncate              one file's attributes
 *   statfs64                                       how full the file system is
 *
 * The *at() versions take a directory fd (or AT_FDCWD); the old calls are
 * the same thing with AT_FDCWD. Every path is copied in from the program
 * first (get_path), so it can't change under us while we work.
 *
 * All of these are O(path components): one hash probe per name, then a
 * constant amount of work (vfs.c). Deleting a big file is the exception:
 * freeing its pages is proportional to its size.
 */
#include <blockdev.h>
#include <errno.h>
#include <ext2.h>
#include <fat32.h>
#include <kheap.h>
#include <proc.h>
#include <string.h>
#include <syscall.h>
#include <syscalls.h>
#include <uaccess.h>
#include <vfs.h>

#define AT_REMOVEDIR     0x200
#define RENAME_NOREPLACE 1
#define UTIME_NOW        ((1 << 30) - 1)
#define UTIME_OMIT       ((1 << 30) - 2)

/* Where a new name goes: the directory (found) and the last name. */
static int parent_of(uint32_t dirfd, uintptr_t user_path, char *path,
                     struct inode **dir, const char **name, uint32_t *len)
{
    struct inode *start;
    int err = get_path(path, user_path);
    if (!err)
        err = dir_of(dirfd, &start);
    if (!err)
        err = vfs_lookup_parent(start, path, dir, name, len);
    return err;
}

/* The file a path names. With AT_EMPTY_PATH, an empty path means the
   dirfd itself (that's how fchownat & co. reach an open file). */
static int target_of(uint32_t dirfd, uintptr_t user_path, uint32_t flags,
                     struct inode **out)
{
    char path[PATH_MAX];
    struct inode *dir;
    int err = get_path(path, user_path);
    if (err == -ENOENT && (flags & AT_EMPTY_PATH)) {
        struct file *f = fd_get(dirfd);
        if (!f)
            return -EBADF;
        *out = f->inode;
        return 0;
    }
    if (!err)
        err = dir_of(dirfd, &dir);
    if (!err)
        err = vfs_lookup_at(dir, path, !(flags & AT_SYMLINK_NOFOLLOW), out);
    return err;
}

static uint32_t umasked(uint32_t mode)
{
    return mode & 07777 & ~proc_current()->umask;
}

/* --- new names ------------------------------------------------------------- */

/* int mkdir(path, mode) / mkdirat(dirfd, path, mode) */
static int32_t do_mkdir(uint32_t dirfd, uintptr_t user_path, uint32_t mode)
{
    char path[PATH_MAX];
    struct inode *dir, *i;
    const char *name;
    uint32_t len;
    int err = parent_of(dirfd, user_path, path, &dir, &name, &len);
    return err ? err : vfs_create(dir, name, len, S_IFDIR | umasked(mode), 0, &i);
}

SYSCALL_DEF(sys_mkdir)   { return do_mkdir(AT_FDCWD, a1, a2); }
SYSCALL_DEF(sys_mkdirat) { return do_mkdir(a1, a2, a3); }

/* int mknod(path, mode, dev) / mknodat(dirfd, path, mode, dev): an empty
   file, a named pipe (mkfifo), or a device node (`mknod /dev/null c 1 3`,
   `mknod /dev/sda b 8 0`). Device nodes are root's only, as on Linux
   (CAP_MKNOD): anyone else could make a node for the raw disk somewhere
   they own and read around every file's permissions. */
static int32_t do_mknod(uint32_t dirfd, uintptr_t user_path, uint32_t mode, uint32_t dev)
{
    uint32_t type = mode & S_IFMT ? mode & S_IFMT : S_IFREG;
    if (type != S_IFREG && type != S_IFCHR && type != S_IFBLK && type != S_IFIFO)
        return -EPERM;
    if ((type == S_IFCHR || type == S_IFBLK) && proc_current()->cred.euid != 0)
        return -EPERM;
    char path[PATH_MAX];
    struct inode *dir, *i;
    const char *name;
    uint32_t len;
    int err = parent_of(dirfd, user_path, path, &dir, &name, &len);
    if (!err)                           /* rdev: (major << 8) | minor */
        err = vfs_create(dir, name, len, type | umasked(mode),
                         type == S_IFCHR || type == S_IFBLK ? dev & 0xFFFF : 0, &i);
    if (!err && (type == S_IFCHR || type == S_IFBLK))
        dev_attach(i);                  /* known numbers get their driver */
    return err;
}

SYSCALL_DEF(sys_mknod)   { return do_mknod(AT_FDCWD, a1, a2, a3); }
SYSCALL_DEF(sys_mknodat) { return do_mknod(a1, a2, a3, a4); }

/* int symlink(target, path) / symlinkat(target, dirfd, path). The target
   is just text: it doesn't have to exist. */
static int32_t do_symlink(uintptr_t user_target, uint32_t dirfd, uintptr_t user_path)
{
    char target[PATH_MAX], path[PATH_MAX];
    struct inode *dir;
    const char *name;
    uint32_t len;
    int err = get_path(target, user_target);
    if (!err)
        err = parent_of(dirfd, user_path, path, &dir, &name, &len);
    return err ? err : vfs_symlink(dir, name, len, target, strlen(target));
}

SYSCALL_DEF(sys_symlink)   { return do_symlink(a1, AT_FDCWD, a2); }
SYSCALL_DEF(sys_symlinkat) { return do_symlink(a1, a2, a3); }

/* link / linkat: a hard link is a second name for the same file, and here
   a file and its name are one struct. Not yet; EPERM is what Linux says
   on file systems without hard links (FAT). */
SYSCALL_DEF(sys_link) { return -EPERM; }

/* --- removing and moving names ---------------------------------------------- */

/* unlink(path) / rmdir(path) / unlinkat(dirfd, path, flags) */
static int32_t do_remove(uint32_t dirfd, uintptr_t user_path, int rmdir)
{
    char path[PATH_MAX];
    struct inode *dir;
    const char *name;
    uint32_t len;
    int err = parent_of(dirfd, user_path, path, &dir, &name, &len);
    if (err)
        return err;
    if (len == 0)                       /* "/" */
        return rmdir ? -EBUSY : -EISDIR;
    if (vfs_is_dots(name, len))
        return !rmdir ? -EISDIR : len == 1 ? -EINVAL : -ENOTEMPTY;
    struct inode *i = vfs_child(dir, name, len);
    if (!i)
        return -ENOENT;
    if (i->flags & I_MOUNTED)
        return -EBUSY;                  /* a mounted disk's root */
    if (rmdir) {
        if (!S_ISDIR(i->mode))
            return -ENOTDIR;
        if (!vfs_dir_empty(i))
            return -ENOTEMPTY;
    } else if (S_ISDIR(i->mode)) {
        return -EISDIR;
    }
    return vfs_unlink(i);       /* if it's open, it lives on until closed */
}

SYSCALL_DEF(sys_unlink) { return do_remove(AT_FDCWD, a1, 0); }
SYSCALL_DEF(sys_rmdir)  { return do_remove(AT_FDCWD, a1, 1); }
SYSCALL_DEF(sys_unlinkat)
{
    if (a3 & ~AT_REMOVEDIR)
        return -EINVAL;
    return do_remove(a1, a2, a3 & AT_REMOVEDIR);
}

/* rename(old, new) / renameat(olddir, old, newdir, new) / renameat2(...,
   flags): move a name, replacing what's at the new one — in one step, so
   no program ever sees neither or both. */
static int32_t do_rename(uint32_t olddirfd, uintptr_t oldpath, uint32_t newdirfd,
                         uintptr_t newpath, uint32_t flags)
{
    if (flags & ~RENAME_NOREPLACE)
        return -EINVAL;
    char from[PATH_MAX], to[PATH_MAX];
    struct inode *odir, *ndir;
    const char *oname, *nname;
    uint32_t olen, nlen;
    int err = parent_of(olddirfd, oldpath, from, &odir, &oname, &olen);
    if (!err)
        err = parent_of(newdirfd, newpath, to, &ndir, &nname, &nlen);
    if (err)
        return err;
    if (!olen || !nlen || vfs_is_dots(oname, olen) || vfs_is_dots(nname, nlen))
        return -EBUSY;                  /* "/", "." and ".." stay put */
    struct inode *i = vfs_child(odir, oname, olen);
    if (!i)
        return -ENOENT;
    return vfs_rename(i, ndir, nname, nlen, flags & RENAME_NOREPLACE);
}

SYSCALL_DEF(sys_rename)    { return do_rename(AT_FDCWD, a1, AT_FDCWD, a2, 0); }
SYSCALL_DEF(sys_renameat)  { return do_rename(a1, a2, a3, a4, 0); }
SYSCALL_DEF(sys_renameat2) { return do_rename(a1, a2, a3, a4, a5); }

/* --- attributes --------------------------------------------------------------- */

static int set_mode(struct inode *i, uint32_t mode)
{
    i->mode = (i->mode & S_IFMT) | (mode & 07777);
    i->ctime = time_now();
    return vfs_setattr(i);              /* on a disk: write it there too */
}

/* chmod(path, mode) / fchmodat(dirfd, path, mode) / fchmod(fd, mode) */
SYSCALL_DEF(sys_chmod)
{
    struct inode *i;
    int err = target_of(AT_FDCWD, a1, 0, &i);
    return err ? err : set_mode(i, a2);
}

SYSCALL_DEF(sys_fchmodat)
{
    struct inode *i;
    int err = target_of(a1, a2, 0, &i);
    return err ? err : set_mode(i, a3);
}

SYSCALL_DEF(sys_fchmod)
{
    struct file *f = fd_get(a1);
    return f ? set_mode(f->inode, a2) : -EBADF;
}

/* Owner and group; -1 leaves one unchanged. (We're all root, so this
   only matters to programs that look, like `ls -l` and tar.) */
static int set_owner(struct inode *i, uint32_t uid, uint32_t gid)
{
    if (uid != 0xFFFFFFFFu)
        i->uid = uid;
    if (gid != 0xFFFFFFFFu)
        i->gid = gid;
    i->ctime = time_now();
    return vfs_setattr(i);
}

static int32_t do_chown(uint32_t dirfd, uintptr_t user_path, uint32_t uid, uint32_t gid,
                        uint32_t flags)
{
    struct inode *i;
    int err = target_of(dirfd, user_path, flags, &i);
    return err ? err : set_owner(i, uid, gid);
}

SYSCALL_DEF(sys_chown32)  { return do_chown(AT_FDCWD, a1, a2, a3, 0); }
SYSCALL_DEF(sys_lchown32) { return do_chown(AT_FDCWD, a1, a2, a3, AT_SYMLINK_NOFOLLOW); }
SYSCALL_DEF(sys_fchownat) { return do_chown(a1, a2, a3, a4, a5); }
SYSCALL_DEF(sys_fchown32)
{
    struct file *f = fd_get(a1);
    return f ? set_owner(f->inode, a2, a3) : -EBADF;
}

/* utimensat(dirfd, path, times[2], flags): set the access and modify
   times (`touch`). No times: both "now". A path of NULL means the dirfd
   itself (futimens). Each nanosecond field may say UTIME_NOW or
   UTIME_OMIT instead. */
static int32_t set_times(uint32_t dirfd, uintptr_t user_path, uint32_t flags, int given,
                         int64_t s0, int32_t ns0, int64_t s1, int32_t ns1)
{
    struct inode *i;
    if (!user_path) {
        struct file *f = fd_get(dirfd);
        if (!f)
            return -EBADF;
        i = f->inode;
    } else {
        int err = target_of(dirfd, user_path, flags, &i);
        if (err)
            return err;
    }
    uint32_t now = time_now();
    if (!given || ns0 != UTIME_OMIT)
        i->atime = (!given || ns0 == UTIME_NOW) ? now : (uint32_t)s0;
    if (!given || ns1 != UTIME_OMIT)
        i->mtime = (!given || ns1 == UTIME_NOW) ? now : (uint32_t)s1;
    i->ctime = now;
    return vfs_setattr(i);
}

/* The 32-bit-time version: musl uses it while the times fit. */
SYSCALL_DEF(sys_utimensat)
{
    int32_t t[4] = { 0 };
    if (a3 && copy_from_user(t, (const void *)a3, sizeof t) < 0)
        return -EFAULT;
    return set_times(a1, a2, a4, a3 != 0, t[0], t[1], t[2], t[3]);
}

SYSCALL_DEF(sys_utimensat64)
{
    int64_t t[4] = { 0 };
    if (a3 && copy_from_user(t, (const void *)a3, sizeof t) < 0)
        return -EFAULT;
    return set_times(a1, a2, a4, a3 != 0, t[0], (int32_t)t[1], t[2], (int32_t)t[3]);
}

/* --- size ------------------------------------------------------------------- */

static int32_t do_truncate(struct inode *i, int64_t size)
{
    if (S_ISDIR(i->mode))
        return -EISDIR;
    if (!S_ISREG(i->mode) || size < 0)
        return -EINVAL;
    if (size > 0xFFFFFFFFll)
        return -EFBIG;
    return vfs_truncate(i, (uint32_t)size);
}

static int32_t path_truncate(uintptr_t user_path, int64_t size)
{
    struct inode *i;
    int err = target_of(AT_FDCWD, user_path, 0, &i);
    return err ? err : do_truncate(i, size);
}

static int32_t fd_truncate(uint32_t fd, int64_t size)
{
    struct file *f = fd_get(fd);
    if (!f)
        return -EBADF;
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EINVAL;                 /* must be open for writing */
    return do_truncate(f->inode, size);
}

/* truncate(path, length) / ftruncate(fd, length), and the 64-bit versions
   (musl's): the length comes in two registers, low half first. */
SYSCALL_DEF(sys_truncate)    { return path_truncate(a1, (int32_t)a2); }
SYSCALL_DEF(sys_ftruncate)   { return fd_truncate(a1, (int32_t)a2); }
SYSCALL_DEF(sys_truncate64)  { return path_truncate(a1, (int64_t)(((uint64_t)a3 << 32) | a2)); }
SYSCALL_DEF(sys_ftruncate64) { return fd_truncate(a1, (int64_t)(((uint64_t)a3 << 32) | a2)); }

/* fallocate(fd, mode, offset, len): Firefox uses this to size a /dev/shm
   file before mmap. On ramfs there is nothing to preallocate, so mode 0 is
   "grow the file if offset+len is past the end" — the same as ftruncate —
   and FALLOC_FL_KEEP_SIZE is a successful no-op. Other modes (punch hole,
   collapse) wait for a user. */
#define FALLOC_FL_KEEP_SIZE 0x01

/* posix_fadvise(fd, offset, len, advice): a hint about how the file will
   be read. Checked, then nothing to do: read-ahead here is fixed, and pages
   are dropped only when memory runs short. (i386 fadvise64_64 passes the
   64-bit numbers in halves; x86_64 fadvise64 whole.) */
#define POSIX_FADV_NOREUSE 5
static int32_t do_fadvise(uint32_t fd, int64_t off, int64_t len, uint32_t advice)
{
    struct file *f = fd_get(fd);
    if (!f)
        return -EBADF;
    if (S_ISFIFO(f->inode->mode))
        return -ESPIPE;
    if (advice > POSIX_FADV_NOREUSE || off < 0 || len < 0)
        return -EINVAL;
    return 0;
}

SYSCALL_DEF(sys_fadvise64_64)
{
    return do_fadvise(a1, (int64_t)(((uint64_t)a3 << 32) | a2),
                      (int64_t)(((uint64_t)a5 << 32) | a4), a6);
}

SYSCALL_DEF(sys_fadvise64)              /* x86_64 */
{
    return do_fadvise(a1, (int64_t)a2, (int64_t)a3, a4);
}

/* fallocate(fd, mode, offset, len) on x86_64: the 64-bit numbers whole. */
SYSCALL_DEF(sys_fallocate64)
{
    uint64_t off = a3, len = a4;
    return sys_fallocate(a1, a2, (uint32_t)off, (uint32_t)(off >> 32),
                         (uint32_t)len, (uint32_t)(len >> 32));
}

SYSCALL_DEF(sys_fallocate)
{
    int64_t off = (int64_t)(((uint64_t)a4 << 32) | a3);
    int64_t len = (int64_t)(((uint64_t)a6 << 32) | a5);
    if (off < 0 || len <= 0)
        return -EINVAL;
    if (a2 & ~FALLOC_FL_KEEP_SIZE)
        return -EOPNOTSUPP;
    struct file *f = fd_get(a1);
    if (!f)
        return -EBADF;
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    int64_t end = off + len;
    if (end < 0)
        return -EFBIG;
    if (!(a2 & FALLOC_FL_KEEP_SIZE) && end > (int64_t)f->inode->size)
        return do_truncate(f->inode, end);
    return 0;
}

/* memfd_create(name, flags): an anonymous RAM file, what Firefox prefers
   over a name in /dev/shm. The name is only for /proc (we don't show it
   yet). Sealing and huge pages are refused. */
#define MFD_CLOEXEC       0x0001u
#define MFD_ALLOW_SEALING 0x0002u

SYSCALL_DEF(sys_memfd_create)
{
    char name[256];
    if (a2 & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING))
        return -EINVAL;
    long n = strncpy_from_user(name, (const char *)a1, sizeof name);
    if (n < 0)
        return n;
    if (n == (long)sizeof name - 1)
        return -EINVAL;                 /* Linux: NAME_MAX, no NUL in the limit */
    struct inode *i = kmalloc(sizeof *i);
    if (!i)
        return -ENOMEM;
    memset(i, 0, sizeof *i);
    i->mode = S_IFREG | 0777;
    i->flags = I_UNLINKED | I_MEMFD;    /* fcntl F_ADD_SEALS / F_GET_SEALS */
    i->name = "memfd";
    i->name_len = 5;
    struct process *p = proc_current();
    if (p) {
        i->uid = p->cred.fsuid;
        i->gid = p->cred.fsgid;
    }
    struct file *f = file_new(&ram_ops, i, O_RDWR);
    if (!f) {
        kfree(i);
        return -ENOMEM;
    }
    int fd = fd_install(f);
    if (fd < 0) {
        file_put(f);
        return fd;
    }
    if ((a2 & MFD_CLOEXEC) && p)
        fd_set_cloexec(p, fd, 1);
    return fd;
}

/* --- statfs ------------------------------------------------------------------ */

/* struct statfs64, as Linux i386 lays it out (what `df` reads). */
struct statfs64 {
    uint32_t f_type, f_bsize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t  f_fsid[2];
    uint32_t f_namelen, f_frsize, f_flags, f_spare[4];
} __attribute__((packed));
_Static_assert(sizeof(struct statfs64) == 84, "struct statfs64 must match Linux");

#define TMPFS_MAGIC 0x01021994
#define EXT2_MAGIC  0xEF53

/* The numbers for the file system i is on: the RAM one, or a disk. */
static int32_t get_statfs(struct inode *i, struct statfs64 *out)
{
    struct statfs64 st;
    memset(&st, 0, sizeof st);
    st.f_namelen = NAME_MAX;
    if (i->mnt) {
        uint32_t bsize, blocks, bfree, files, ffree;
        int err = i->mnt->ops->statfs(i->mnt, &bsize, &blocks, &bfree, &files, &ffree);
        if (err)
            return err;
        st.f_type = i->mnt->magic ? i->mnt->magic : EXT2_MAGIC;
        st.f_bsize = st.f_frsize = bsize;
        st.f_blocks = blocks;
        st.f_bfree = st.f_bavail = bfree;
        st.f_files = files;
        st.f_ffree = ffree;
        st.f_fsid[0] = i->mnt->dev_minor;
    } else {
        uint32_t used, max;
        ramfs_usage(&used, &max);
        st.f_type = TMPFS_MAGIC;        /* "tmpfs": everything is in RAM */
        st.f_bsize = st.f_frsize = 4096;
        st.f_blocks = max;
        st.f_bfree = st.f_bavail = max > used ? max - used : 0;
        st.f_fsid[0] = 1;
    }
    *out = st;
    return 0;
}

/* statfs64 / fstatfs64 (i386): the struct's size is an argument. */
static int32_t fill_statfs(struct inode *i, uint32_t size, uintptr_t user_buf)
{
    if (size != sizeof(struct statfs64) && size != 88)  /* 88: padded variant */
        return -EINVAL;
    struct statfs64 st;
    int32_t err = get_statfs(i, &st);
    return err ? err : copy_to_user((void *)user_buf, &st, sizeof st);
}

/* statfs / fstatfs with Linux's struct statfs of longs (x86_64: 120 bytes;
   on i386 musl uses the 64 calls above). */
struct statfs_long {
    long f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t f_fsid[2];
    long f_namelen, f_frsize, f_flags, f_spare[4];
};

static int32_t fill_statfs_long(struct inode *i, uintptr_t user_buf)
{
    struct statfs64 st;
    int32_t err = get_statfs(i, &st);
    if (err)
        return err;
    struct statfs_long out = {
        .f_type = st.f_type, .f_bsize = st.f_bsize, .f_blocks = (long)st.f_blocks,
        .f_bfree = (long)st.f_bfree, .f_bavail = (long)st.f_bavail,
        .f_files = (long)st.f_files, .f_ffree = (long)st.f_ffree,
        .f_fsid = { st.f_fsid[0], st.f_fsid[1] },
        .f_namelen = st.f_namelen, .f_frsize = st.f_frsize, .f_flags = st.f_flags,
    };
    return copy_to_user((void *)user_buf, &out, sizeof out);
}

SYSCALL_DEF(sys_statfs)
{
    struct inode *i;
    int err = target_of(AT_FDCWD, a1, 0, &i);
    return err ? err : fill_statfs_long(i, a2);
}

SYSCALL_DEF(sys_fstatfs)
{
    struct file *f = fd_get(a1);
    return f ? fill_statfs_long(f->inode, a2) : -EBADF;
}

/* statfs64(path, size, buf) / fstatfs64(fd, size, buf) */
SYSCALL_DEF(sys_statfs64)
{
    struct inode *i;
    int err = target_of(AT_FDCWD, a1, 0, &i);
    return err ? err : fill_statfs(i, a2, a3);
}

SYSCALL_DEF(sys_fstatfs64)
{
    struct file *f = fd_get(a1);
    return f ? fill_statfs(f->inode, a2, a3) : -EBADF;
}

/* --- mount, umount ------------------------------------------------------------ */

/*
 * The kernel mounts the disks itself at boot (by label, main.c). What a
 * program can mount is a tmpfs (a fresh RAM directory, for /run and /tmp)
 * proc (fs/procfs.c) or sysfs (fs/sysfs.c). There are no block device
 * files yet, so "mount /dev/sda1 /mnt" can't name a disk; any other type
 * fails the way Linux fails for a file system type it wasn't built with.
 * Boot scripts expect that and go on.
 */
#define MS_RDONLY   1
#define MS_REMOUNT  32
#define MS_BIND     4096
#define MS_MOVE     8192
#define MS_MGC_MSK  0xFFFF0000u         /* old programs put a magic number here */
#define MS_MGC_VAL  0xC0ED0000u

/* Is i the top of a mounted file system (or "/")? */
static int is_mount_root(struct inode *i)
{
    return i->covers || i == vfs_root();
}

/* The "mode=1777" option of a tmpfs, from the comma-separated data
   string; Linux's default when it's missing. */
static uint32_t tmpfs_mode(uintptr_t user_data)
{
    char opt[128];
    uint32_t mode = 01777;
    if (!user_data || strncpy_from_user(opt, (const char *)user_data, sizeof opt) < 0)
        return mode;
    for (char *p = opt; *p; ) {
        if (p[0] == 'm' && p[1] == 'o' && p[2] == 'd' && p[3] == 'e' && p[4] == '=') {
            mode = 0;
            for (p += 5; *p >= '0' && *p <= '7'; p++)
                mode = mode * 8 + (uint32_t)(*p - '0');
            return mode & 07777;
        }
        while (*p && *p != ',')
            p++;
        if (*p)
            p++;
    }
    return mode;
}

/* ext2 or vfat from a disk file (/dev/sda2), as the installer mounts the
   system it just made. Root only. Not a disk that is mounted already, nor
   one a program has open for writing (dev.c): the two caches would
   overwrite each other's work. */
static int32_t mount_disk(uintptr_t user_source, const char *target, int ext2)
{
    char source[PATH_MAX];
    struct inode *i;
    if (proc_current()->cred.euid != 0)
        return -EPERM;
    int err = get_path(source, user_source);
    if (!err)
        err = vfs_lookup(source, &i);
    if (err)
        return err;
    if (!S_ISBLK(i->mode))
        return -ENOTBLK;
    struct blockdev *d = blockdev_by_devnum(i->rdev >> 8, i->rdev & 0xFF);
    if (!d)
        return -ENXIO;
    for (int k = 0; k < vfs_mount_count(); k++) {
        struct mount *m = vfs_mount_at(k);
        if (m && !strncmp(m->source, "/dev/", 5) && !strcmp(m->source + 5, d->name))
            return -EBUSY;
    }
    if (blkdev_busy(d))
        return -EBUSY;
    return ext2 ? ext2_mount(d, target) : fat_vfs_mount(d, target);
}

/* mount(source, target, fstype, flags, data) */
SYSCALL_DEF(sys_mount)
{
    char target[PATH_MAX], type[16];
    struct inode *i;
    uint32_t flags = a4;
    /* Root only, every kind (Linux: CAP_SYS_ADMIN). Anyone else could
       mount an empty tmpfs over /etc, write their own passwd or
       doas.conf there, and become root. */
    if (proc_current()->cred.euid != 0)
        return -EPERM;
    if ((flags & MS_MGC_MSK) == MS_MGC_VAL)
        flags &= ~MS_MGC_MSK;
    int err = get_path(target, a2);
    if (err)
        return err;

    if (flags & (MS_BIND | MS_MOVE))
        return -EINVAL;
    if (flags & MS_REMOUNT) {
        err = vfs_lookup(target, &i);
        if (err)
            return err;
        if (!is_mount_root(i))
            return -EINVAL;
        /* nosuid, noexec, ...: nothing checks them yet (no setuid files,
           everyone is root). Read-only needs the file system's help: no. */
        return (flags & MS_RDONLY) ? -EBUSY : 0;
    }

    long n = strncpy_from_user(type, (const char *)a3, sizeof type);
    if (n == -EFAULT)
        return -EFAULT;
    if (n < 0)
        return -ENODEV;                 /* too long to be a type we know */
    if (strcmp(type, "tmpfs") == 0 || strcmp(type, "proc") == 0 || strcmp(type, "sysfs") == 0) {
        err = type[0] == 't' ? vfs_mount_ram(target, tmpfs_mode(a5))
            : type[0] == 'p' ? procfs_mount(target) : sysfs_mount(target);
        if (!err)                       /* for /proc/mounts */
            vfs_note_mount(target, type[0] == 't' ? "tmpfs" : type[0] == 'p' ? "proc" : "sysfs");
        return err;
    }
    if (strcmp(type, "ext2") == 0 || strcmp(type, "vfat") == 0)
        return mount_disk(a1, target, type[0] == 'e');
    return -ENODEV;
}

/* --- extended attributes ---------------------------------------------------- */
/*
 * setxattr / getxattr / listxattr / removexattr, each in three forms: by
 * path, by path without following a final symlink (l*), by fd (f*).
 * MyOS's file systems keep no extended attributes (SELinux labels, ACLs,
 * user.* notes), so the answer is the one Linux gives on such a file system:
 * "not supported" for get, set and remove, and an empty list. Programs that
 * copy attributes (coreutils' install and cp -a, tar, rsync) take that as
 * "nothing to copy" and carry on; -ENOSYS made some of them complain. The
 * file is looked up (or the fd checked) first, so a missing one is still
 * -ENOENT / -EBADF, as on Linux.
 */
enum { XATTR_PATH, XATTR_LPATH, XATTR_FD };

static int32_t xattr_target(int how, uintptr_t arg)
{
    if (how == XATTR_FD)
        return fd_get(arg) ? 0 : -EBADF;
    struct inode *i;
    return target_of(AT_FDCWD, arg, how == XATTR_LPATH ? AT_SYMLINK_NOFOLLOW : 0, &i);
}

#define XATTR_CALL(name, how, answer)                                       \
    SYSCALL_DEF(name)                                                       \
    {                                                                       \
        int32_t err = xattr_target(how, a1);                                \
        return err ? err : (answer);                                        \
    }
XATTR_CALL(sys_setxattr,     XATTR_PATH,  -EOPNOTSUPP)
XATTR_CALL(sys_lsetxattr,    XATTR_LPATH, -EOPNOTSUPP)
XATTR_CALL(sys_fsetxattr,    XATTR_FD,    -EOPNOTSUPP)
XATTR_CALL(sys_getxattr,     XATTR_PATH,  -EOPNOTSUPP)
XATTR_CALL(sys_lgetxattr,    XATTR_LPATH, -EOPNOTSUPP)
XATTR_CALL(sys_fgetxattr,    XATTR_FD,    -EOPNOTSUPP)
XATTR_CALL(sys_listxattr,    XATTR_PATH,  0)            /* no names: length 0 */
XATTR_CALL(sys_llistxattr,   XATTR_LPATH, 0)
XATTR_CALL(sys_flistxattr,   XATTR_FD,    0)
XATTR_CALL(sys_removexattr,  XATTR_PATH,  -EOPNOTSUPP)
XATTR_CALL(sys_lremovexattr, XATTR_LPATH, -EOPNOTSUPP)
XATTR_CALL(sys_fremovexattr, XATTR_FD,    -EOPNOTSUPP)

/* pivot_root(new_root, put_old): see vfs_pivot_root. Root only, and not from
   inside a chroot (whose "/" is not the one being moved). */
SYSCALL_DEF(sys_pivot_root)
{
    char new_root[PATH_MAX], put_old[PATH_MAX];
    struct process *p = proc_current();
    if (p->cred.euid != 0)
        return -EPERM;
    if (p->root)
        return -EINVAL;
    int err = get_path(new_root, a1);
    if (!err)
        err = get_path(put_old, a2);
    return err ? err : vfs_pivot_root(new_root, put_old);
}

/* umount2(target, flags). Taking a file system off the tree needs to know
   that nothing in it is open or anyone's cwd; the VFS doesn't count that
   yet, so every mount stays busy. */
SYSCALL_DEF(sys_umount2)
{
    char target[PATH_MAX];
    struct inode *i;
    if (a2 & ~0xFu)
        return -EINVAL;
    int err = get_path(target, a1);
    if (!err)
        err = vfs_lookup(target, &i);
    if (err)
        return err;
    return is_mount_root(i) ? -EBUSY : -EINVAL;
}
