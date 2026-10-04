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
#include <errno.h>
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
static int parent_of(uint32_t dirfd, uint32_t user_path, char *path,
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
static int target_of(uint32_t dirfd, uint32_t user_path, uint32_t flags,
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
static int32_t do_mkdir(uint32_t dirfd, uint32_t user_path, uint32_t mode)
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
   file, a named pipe (mkfifo), or a device node (`mknod /dev/null c 1 3`).
   Block devices come later (docs/gaps.md). */
static int32_t do_mknod(uint32_t dirfd, uint32_t user_path, uint32_t mode, uint32_t dev)
{
    uint32_t type = mode & S_IFMT ? mode & S_IFMT : S_IFREG;
    if (type != S_IFREG && type != S_IFCHR && type != S_IFIFO)
        return -EPERM;
    char path[PATH_MAX];
    struct inode *dir, *i;
    const char *name;
    uint32_t len;
    int err = parent_of(dirfd, user_path, path, &dir, &name, &len);
    if (!err)                           /* rdev: (major << 8) | minor */
        err = vfs_create(dir, name, len, type | umasked(mode),
                         type == S_IFCHR ? dev & 0xFFFF : 0, &i);
    if (!err && type == S_IFCHR)
        dev_attach(i);                  /* known numbers get their driver */
    return err;
}

SYSCALL_DEF(sys_mknod)   { return do_mknod(AT_FDCWD, a1, a2, a3); }
SYSCALL_DEF(sys_mknodat) { return do_mknod(a1, a2, a3, a4); }

/* int symlink(target, path) / symlinkat(target, dirfd, path). The target
   is just text: it doesn't have to exist. */
static int32_t do_symlink(uint32_t user_target, uint32_t dirfd, uint32_t user_path)
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
static int32_t do_remove(uint32_t dirfd, uint32_t user_path, int rmdir)
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
static int32_t do_rename(uint32_t olddirfd, uint32_t oldpath, uint32_t newdirfd,
                         uint32_t newpath, uint32_t flags)
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

static int32_t do_chown(uint32_t dirfd, uint32_t user_path, uint32_t uid, uint32_t gid,
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
static int32_t set_times(uint32_t dirfd, uint32_t user_path, uint32_t flags, int given,
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

static int32_t path_truncate(uint32_t user_path, int64_t size)
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
static int32_t fill_statfs(struct inode *i, uint32_t size, uint32_t user_buf)
{
    if (size != sizeof(struct statfs64) && size != 88)  /* 88: padded variant */
        return -EINVAL;
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
    return copy_to_user((void *)user_buf, &st, sizeof st);
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
static uint32_t tmpfs_mode(uint32_t user_data)
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

/* mount(source, target, fstype, flags, data) */
SYSCALL_DEF(sys_mount)
{
    char target[PATH_MAX], type[16];
    struct inode *i;
    uint32_t flags = a4;
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
    if (strcmp(type, "tmpfs") == 0)
        return vfs_mount_ram(target, tmpfs_mode(a5));
    if (strcmp(type, "proc") == 0)
        return procfs_mount(target);
    if (strcmp(type, "sysfs") == 0)
        return sysfs_mount(target);
    if (strcmp(type, "ext2") == 0 || strcmp(type, "vfat") == 0)
        return -ENOTBLK;                /* no block device files to name */
    return -ENODEV;
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
