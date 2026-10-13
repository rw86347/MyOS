/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef VFS_H
#define VFS_H

#include <proc.h>
#include <stdint.h>
#include <string.h>
#include <uaccess.h>

/*
 * The VFS ("virtual file system"): files and directories, whatever they're
 * stored on. Today everything is in RAM: the tree starts as the initramfs
 * (fs/initramfs.c) and can then be changed like any disk — new files,
 * writes, mkdir, rm, mv (fs/ramfs.c holds the contents). Plus pipes
 * (fs/pipe.c) and devices like the console (fs/dev.c). FAT32 and ext2
 * plug in later.
 *
 *   inode   one file, directory, symlink or device: type, size, contents
 *   file    an OPEN file: which inode, the current position, the flags
 *   fd      a small number (0, 1, 2, 3...) a program uses to name an open
 *           file; an index into its process's file table
 */

/* File types and permission bits (st_mode), as Linux defines them. */
/* The most one read() or write() transfers, whatever the file. A syscall
   cannot be preempted, so an uncapped one would hold the CPU for as long as
   the caller asked — 256 KiB is about 0.1 ms of copying on a Pentium 4, and
   a short read is something every caller already has to handle.
   sys_read/sys_write apply it, so no file's ops can be asked for more; the
   filesystems keep their own copy of the clamp as well, because the ops are
   also reached from inside the kernel. */
#define RW_MAX    (256 * 1024)

#define S_IFMT    0170000
#define S_IFREG   0100000           /* regular file */
#define S_IFDIR   0040000           /* directory */
#define S_IFLNK   0120000           /* symbolic link */
#define S_IFCHR   0020000           /* character device (the console) */
#define S_IFBLK   0060000           /* block device (a disk: /dev/sda) */
#define S_IFIFO   0010000           /* pipe */
#define S_IFSOCK  0140000           /* socket */
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISUID   04000             /* run as the file's owner */
#define S_ISGID   02000             /* run as its group; on a directory: new
                                       entries take the directory's group */
#define S_ISVTX   01000             /* sticky: on a directory (/tmp), only an
                                       entry's owner may remove it */
#define S_IXGRP   00010             /* group may execute */

/* open() flags (Linux i386 values) */
#define O_ACCMODE   3
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW  0400000
#define O_CLOEXEC   02000000

#define NAME_MAX    255
#define PATH_MAX    256             /* ours; Linux allows 4096 */
#define FD_MAX      PROC_FILES

struct blockdev;
struct file;
struct inode;
struct vma;

struct file_ops {
    /* Copy up to n bytes to/from USER memory at the file's position.
       Return bytes done, or -errno. */
    int32_t (*read)(struct file *f, void *user_buf, uint32_t n);
    int32_t (*write)(struct file *f, const void *user_buf, uint32_t n);
    void (*release)(struct file *f);        /* last close (may be NULL) */
    int32_t (*ioctl)(struct file *f, uint32_t cmd, uintptr_t arg);   /* or NULL */
    uint32_t (*poll)(struct file *f);       /* POLLIN/POLLOUT/... now (NULL: both) */
    /* Devices whose memory programs can map (/dev/fb0): describe it in vma
       with vm_iomap_memory (mm.h). vma->start is 0 here, end the length.
       NULL: mmap gives -ENODEV. */
    int (*mmap)(struct file *f, struct vma *vma);
    /* open() of the device node made f: set f->priv, or switch f->ops to
       another device's (/dev/tty -> the caller's terminal). -errno fails
       the open. NULL: nothing to do. */
    int (*open)(struct file *f);
    /* 1: write() fetches its bytes with file_copy_in, so sendfile can hand
       it kernel memory (file data straight from the page cache). */
    uint8_t kbuf_ok;
    /* A file that is always readable but has something to say when it
       changes (/proc/self/mountinfo): a count that moves on each change.
       Epoll reports an edge-triggered entry on it only when the count
       moved, as Linux does. NULL for everything else. */
    uint32_t (*change_count)(struct file *f);
};


/* inode flags */
#define I_NAME_OWNED 1              /* name is our kmalloc'd copy */
#define I_DATA_OWNED 2              /* data (a symlink's target) is ours too */
#define I_UNLINKED   4              /* removed from the tree; freed at refs == 0 */
#define I_UNLOADED   8              /* a disk directory whose entries aren't read yet */
#define I_MOUNTED   16              /* the root of a mounted file system */
#define I_PROC_FD   32              /* /proc/self/fd/N (fs_ino = N): a link to
                                       whatever the caller's fd N is open on */
#define I_PROC_EXE  64              /* /proc/self/exe: a link to the caller's
                                       program (the ELF that execve loaded) */
#define I_MEMFD    128              /* memfd_create: fs_ino holds F_SEAL_* */
#define I_WATCHED  256              /* inotify watches it (fs/inotify.c) */
#define I_SHMAPPED 1024             /* mapped MAP_SHARED once: shrinking keeps its
                                       pages (a mapping may still use them) */
#define I_PROC_TASK 512             /* /proc/self/task: its link count says how
                                       many threads the caller has */

/*
 * A file system on a disk (ext2): what the VFS asks of it. The VFS keeps
 * the tree in memory as before; a disk directory's entries join the tree
 * the first time someone looks inside (load_dir). Every change is made on
 * the disk first, then in the tree. Files on the RAM file system have
 * fs == NULL and need none of this.
 */
struct mount;
struct fs_ops {
    int (*load_dir)(struct inode *dir);             /* its entries -> the tree */
    int (*create)(struct inode *dir, struct inode *i);  /* i is new in dir (mode set) */
    int (*unlink)(struct inode *dir, struct inode *i);  /* remove i's name */
    int (*rename)(struct inode *i, struct inode *new_dir, const char *name, uint32_t len,
                  struct inode *replaced);          /* replaced: or NULL */
    void (*evict)(struct inode *i);                 /* unlinked, last user gone */
    int (*setattr)(struct inode *i);                /* mode/owner/times changed */
    int (*truncate)(struct inode *i, uint32_t size);
    int (*read_kernel)(struct inode *i, uint32_t pos, void *kbuf, uint32_t n);
    /* Fill n page frames with pages [index, index + n) of the file, zeros
       past its end — for the page cache. May be NULL (read_kernel is used). */
    int (*readpages)(struct inode *i, uint32_t index, uint32_t n, const uint32_t *frames);
    /* Write those pages (dirty, from the page cache) to the file's blocks.
       Blocks were allocated by the write. NULL: the file system can't. */
    int (*writepages)(struct inode *i, uint32_t index, uint32_t n, const uint32_t *frames);
    /* For read-ahead that doesn't wait: where pages [index, index + n) are
       on *dev — page k is sectors [lba[k], lba[k] + secs[k]) (fewer than 8
       only for the file's last page). Stops at the first page that isn't one
       piece of disk (a hole, or blocks apart). Returns how many pages.
       NULL: no read-ahead of this kind. */
    int (*map_pages)(struct inode *i, uint32_t index, uint32_t n, uint32_t *lba,
                     uint8_t *secs, struct blockdev **dev);
    int (*statfs)(struct mount *m, uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                  uint32_t *files, uint32_t *ffree);
    /* umount: nothing uses it any more and its pages are written; finish
       (mark it cleanly unmounted, let go of held blocks, forget its cache).
       May be NULL. */
    void (*unmount)(struct mount *m);
};

struct mount {
    const struct fs_ops *ops;
    void *priv;                     /* the file system's own state */
    struct inode *root;             /* where it appears in the tree */
    const char *type;               /* "ext2" (for /proc/mounts, df) */
    uint32_t magic;                 /* statfs f_type; 0 means ext2 */
    uint32_t dev_minor;             /* st_dev: 0:1 is the RAM file system */
    char source[24];                /* "/dev/sda2": what /proc/mounts names */
};

struct inode {
    uint32_t ino;
    uint32_t mode;                  /* type + permissions (0: a directory cursor) */
    uint32_t size;
    uint32_t nlink, uid, gid;
    uint32_t atime, mtime, ctime;   /* seconds since 1970 */
    uint32_t rdev;                  /* devices: (major << 8) | minor */

    /* The contents. A file from the initramfs starts out pointing into the
       archive (`data`, read-only, no copy). Its first change moves the
       contents into RAM pages of its own (`pages`: physical addresses,
       0 = a hole of zeros) — see ramfs.c. A symlink's target is in `data`. */
    const uint8_t *data;
    uint32_t *pages;
    uint32_t page_cap;              /* entries in pages[] */

    const struct file_ops *ops;     /* how to read/write it (NULL: ramfs) */
    const struct fs_ops *fs;        /* the disk file system it's on (NULL: RAM) */
    struct mount *mnt;              /* ...that one */
    uint32_t fs_ino;                /* its inode number there */
    uint32_t pc_id;                 /* page cache: its pages' tag (0: none yet) */
    uint32_t ra_next;               /* page cache: a miss here means "read in order" */
    uint16_t ra_win;                /* page cache: last read-ahead, pages */
    int16_t flock;                  /* flock(): >0 shared holders, -1 exclusive */
    uint32_t pc_ndirty;             /* page cache: dirty pages, all in */
    uint32_t dirty_lo, dirty_hi;    /*   [dirty_lo, dirty_hi] */
    struct inode *dirty_next;       /*   the list of files with dirty pages */

    /* Who is using it: open files, mmap'd areas, current directories. A
       file that's deleted while in use (`rm` of a running log file, of a
       program's own library) lives on, out of the tree, until the last
       user lets go — Unix semantics. */
    uint32_t refs;
    uint32_t flags;                 /* I_* above */

    /* Its name and place in the tree. An initramfs name points into the
       archive (not copied, not NUL-terminated: use name_len). The lists
       use "pprev" links (a pointer to whatever points at us), so taking
       an entry out is O(1) without knowing its predecessor. */
    const char *name;
    uint32_t name_len;
    struct inode *parent;
    struct inode *children;                         /* directories: entries */
    struct inode *next_sibling, **pprev_sibling;
    struct inode *hash_next, **pprev_hash;          /* lookup hash (vfs.c) */

    /* Mounts, as in Linux: a file system's root sits ON TOP of an existing
       directory, which it hides until unmounted. Lookups that reach the
       covered directory continue in `mounted`; ".." from a mounted root
       climbs out through `covers`. */
    struct inode *mounted;                          /* what's mounted on us */
    struct inode *covers;                           /* the directory we sit on */
};

struct file {
    const struct file_ops *ops;
    struct inode *inode;
    uint32_t pos;                   /* where the next read starts */
    uint32_t flags;                 /* the open() flags (O_APPEND, O_NONBLOCK...) */
    uint32_t refs;                  /* fds pointing at it */
    void *priv;                     /* pipe: the pipe; directory: its cursor */
    uint16_t flock;                 /* the flock() it holds: 0, LOCK_SH or LOCK_EX */
    /* 1 only in sendfile's private copy of an open file (on its stack, never
       in an fd table): write()'s buffer is then kernel memory. See
       file_copy_in. A program cannot set it. */
    uint8_t kbuf;
    /* Which open this is, counted from boot. Anything that remembers a
       struct file without holding a reference — epoll's interest list — needs
       to tell "still the file I was given" from "a different file that landed
       on the same freed block", and the address alone cannot: close one and
       open another and kmalloc hands back the same bytes at the same fd
       number.
       32 bits and not 64, which is a size-class decision rather than a
       guess: this struct is 28 bytes, so it comes from kmalloc's 32-byte
       class, and eight more bytes would push every open file in the system
       into the 64-byte one. `make bench` put that at 35 cycles on open+close
       and twice the memory per open file, to remove a collision that needs
       the counter to wrap through 2^32 opens *and* the new file to land on
       the same address *and* at the same fd number — and whose consequence
       is polling the wrong file, not touching freed memory. */
    uint32_t serial;
};

/* What a write() that sendfile can use fetches the caller's bytes with:
   user memory as usual, or, for sendfile's private copy of the file,
   kernel memory. */
static inline int file_copy_in(const struct file *f, void *dst, const void *src, uint32_t n)
{
    if (f->kbuf) {
        memcpy(dst, src, n);
        return 0;
    }
    return copy_from_user(dst, src, n);
}

/* --- the tree ---------------------------------------------------------- */
struct inode *vfs_root(void);

/* --- who may do what (fs/perm.c) ---------------------------------------- */
/*
 * Linux's rules, in the order Linux applies them. A file's mode has three
 * sets of rwx bits: for its owner, its group, and everyone else. EXACTLY
 * ONE set applies: the owner's if you are the owner, else the group's if
 * you're in the group, else the others'. (So a 0055 file is closed to its
 * owner, though everyone else may read it.) Root (fsuid 0) may read and
 * write anything, and search any directory, but run only a file that has
 * at least one x bit. The kernel's own threads count as root.
 *
 * "Search" a directory = MAY_EXEC on it: needed to go through it in a path.
 */
#define MAY_EXEC  1
#define MAY_WRITE 2
#define MAY_READ  4

/* 0, or -EACCES. Checked with the file-access ids (fsuid, fsgid, groups),
   or the real ones while the thread is inside access() (perm_real).
   Inline because it's on every open and every directory of every path:
   when all three sets allow it (a 0644 file for reading, a 0755
   directory for searching), whoever asks may — root too — so the answer
   needs no credentials at all. Only the rest goes to fs/perm.c. */
int  vfs_permission_slow(const struct inode *i, int mask);
static inline int vfs_permission(const struct inode *i, int mask)
{
    uint32_t m = i->mode, want = (uint32_t)mask;
    if (((m >> 6) & (m >> 3) & m & want) == want)
        return 0;
    return vfs_permission_slow(i, mask);
}
/* A new name in dir: write and search on it. */
int  vfs_may_create(const struct inode *dir);
/* Remove or rename victim out of dir: write and search on dir, and in a
   sticky directory (/tmp: mode 1777) only victim's owner, dir's owner or
   root (-EPERM otherwise, as Linux says). */
int  vfs_may_delete(const struct inode *dir, const struct inode *victim);
/* The owner, or root: may chmod it, set its times to anything. */
int  vfs_owner_or_root(const struct inode *i);
/* Is the caller in group gid (its fsgid or a supplementary group)? */
int  vfs_in_group(uint32_t gid);
/* Root, for the file-access checks (fsuid 0, or a kernel thread). */
int  vfs_fs_root(void);

/* Find a path (in kernel memory), following symbolic links — also in the
   last component unless `nofollow`. Relative paths start at the current
   directory. 0, -ENOENT, -ENOTDIR, -ELOOP, -ENAMETOOLONG, or -EACCES (a
   directory on the way you may not search). */
int vfs_lookup(const char *path, struct inode **out);
int vfs_lookup_nofollow(const char *path, struct inode **out);
/* ...starting from directory `dir` (NULL: the current directory). */
int vfs_lookup_at(struct inode *dir, const char *path, int follow, struct inode **out);

/* i's full path, with a NUL, into buf: what getcwd and readlink of
   /proc/self/fd/N say. Returns the length without the NUL, or
   -ENAMETOOLONG. */
int vfs_path(struct inode *i, char *buf, uint32_t size);

/* The entry `name` (len bytes) in directory `dir`, or NULL. O(1). */
struct inode *vfs_child(struct inode *dir, const char *name, uint32_t len);

/* Add an entry to a directory (used by file systems while building). */
struct inode *vfs_add(struct inode *dir, const char *name, uint32_t len, uint32_t mode);

/* --- open files and fds ----------------------------------------------- */
/* Open a path. With O_CREAT a missing file is created with `mode` (the
   caller has applied the umask already). */
int  vfs_open_at(struct inode *dir, const char *path, uint32_t flags, uint32_t mode,
                 struct file **out);
struct file *file_new(const struct file_ops *ops, struct inode *i, uint32_t flags);
void file_put(struct file *f);                  /* drop one reference */

/*
 * Hold an open file across something that can sleep.
 *
 * fd_get hands back the struct file without a reference, which is fine only
 * for a syscall that finishes without sleeping. A blocking read does sleep
 * inside f->ops->read, and clone(CLONE_THREAD) puts the new thread in the
 * same struct process -- so the fd table is shared, a sibling's close() can
 * take the last reference, and file_put runs ->release and kfree(f) while
 * the reader is still asleep holding the pointer. It then resumes and reads
 * f->flags out of a recycled heap block.
 *
 * That is not theory: user/musl/racetest.c arms it and a blocking read comes
 * back EAGAIN, which a blocking read may never do -- the O_NONBLOCK bit came
 * out of someone else's object. Pair this with file_put around anything that
 * can sleep, as Linux pairs fdget with fdput.
 */
static inline void file_get(struct file *f) { f->refs++; }

/* Reading a directory (getdents64), in order, while entries may come and
   go between calls: the first entry not yet returned through f (NULL: no
   more); the entry after i; and "f has returned everything up to i"
   (NULL: all of it). */
struct inode *vfs_dir_first(struct file *f);
struct inode *vfs_dir_after(struct inode *i);
int  vfs_dir_stop(struct file *f, struct inode *last);
void vfs_dir_rewind(struct file *f);

int  fd_install(struct file *f);                /* lowest free fd */
int  fd_install_min(struct file *f, int min);   /* lowest free fd >= min (F_DUPFD) */
void fd_table_init(struct process *p);          /* the built-in 64, empty */
void fd_set_cloexec(struct process *p, int fd, int on);
int  fd_get_cloexec(const struct process *p, int fd);
int  fd_install_at(int fd, struct file *f);     /* dup2: exactly this fd */
struct file *fd_get(int fd);                    /* NULL if not open */
int  fd_close(int fd);
void fd_open_console(struct process *p);        /* fds 0, 1, 2 */
void fd_copy_all(struct process *dst, struct process *src);   /* fork */
void fd_close_all(struct process *p);

/* --- changing the tree (vfs.c) ------------------------------------------ */

/* Split a path into its directory (found from `start`, following links)
   and its last name (a pointer into path, and its length; 0 for "/").
   0 or -errno. */
int vfs_lookup_parent(struct inode *start, const char *path, struct inode **dir,
                      const char **name, uint32_t *len);

/* A new, empty file / directory / device in dir (the name is copied),
   or a symlink to target. -EEXIST if the name is taken. */
int  vfs_create(struct inode *dir, const char *name, uint32_t len, uint32_t mode,
                uint32_t rdev, struct inode **out);
int  vfs_symlink(struct inode *dir, const char *name, uint32_t len,
                 const char *target, uint32_t tlen);
int  vfs_unlink(struct inode *i);           /* out of the tree (rm, rmdir) */
int  vfs_dir_empty(struct inode *dir);
int  vfs_truncate(struct inode *i, uint32_t size);
int  vfs_setattr(struct inode *i);          /* after changing mode/owner/times */
int  vfs_dir_load(struct inode *dir);       /* a disk directory's entries, now */

/* Mount file system m (root directory root_ino) on the directory at path,
   which is made if it doesn't exist. Path "/" REPLACES the root: done at
   boot, before any process exists. Returns 0 or -errno; m->root is the
   new root node (the caller fills in its attributes). */
int  vfs_mount(const char *path, struct mount *m, uint32_t root_ino, uint32_t mode);
/* Mount an existing in-memory directory tree on path (/dev onto a disk
   root). */
int  vfs_mount_tree(const char *path, struct inode *tree);
/* Mount a new, empty RAM directory on the existing directory at path
   (mount -t tmpfs). Its files share the RAM file system's pages. */
int  vfs_mount_ram(const char *path, uint32_t mode);
int  vfs_pivot_root(const char *new_root, const char *put_old);
int  vfs_mount_count(void);
/* Take the disk file system whose root is r off the tree: -EBUSY if
   anything still uses it (an open or mapped file, a current directory, a
   program, a bound socket, a mount inside it), -EINVAL if r is not a disk
   mount's root. RAM mounts (tmpfs, proc, sysfs) aren't handled: -EBUSY. */
int  vfs_umount(struct inode *r);
struct mount *vfs_mount_at(int index);
/* Move i to new_dir under a new name, replacing what's there (rename()
   rules: a directory only over an empty directory, a file only over a
   file). */
int  vfs_rename(struct inode *i, struct inode *new_dir, const char *name, uint32_t len,
                int noreplace);

static inline void vfs_get(struct inode *i) { i->refs++; }

static inline int vfs_is_dots(const char *name, uint32_t len)   /* "." or ".." */
{
    return len && name[0] == '.' && (len == 1 || (len == 2 && name[1] == '.'));
}
void vfs_put(struct inode *i);              /* frees an unlinked inode at 0 */

/* --- file contents (ramfs.c) --------------------------------------------- */
extern const struct file_ops ram_ops;       /* read/write a regular file */
/* Copy file bytes into kernel memory (exec, mmap): from RAM, the archive
   or the disk. 0, or -EIO (the bytes are then zeros). */
int     file_copy_out(struct inode *i, uint32_t pos, void *kbuf, uint32_t n);
int     file_truncate(struct inode *i, uint32_t size);
void    file_free(struct inode *i);
/* Page idx of an inode whose contents are only RAM pages (never the
   archive): System V shared memory. Its frame, made zeroed on first use;
   0 when memory or the RAM-file limit runs out. Freed with the inode. */
uint32_t ramfs_page(struct inode *i, uint32_t idx);
/* Before a RAM file is mapped MAP_SHARED: its data in pages of its own
   (not in the boot archive), and I_SHMAPPED. 0 or -ENOSPC. */
int ramfs_share(struct inode *i);
void    ramfs_usage(uint32_t *used_pages, uint32_t *max_pages);   /* statfs, df */
uint32_t time_now(void);                    /* seconds since 1970 (sys_info.c) */

/* --- inotify (fs/inotify.c) ------------------------------------------------
   The VFS calls these where files change. They cost a flag test or two
   unless someone watches the file or its directory. */
#define IN_ACCESS        0x001
#define IN_MODIFY        0x002
#define IN_ATTRIB        0x004
#define IN_CLOSE_WRITE   0x008
#define IN_CLOSE_NOWRITE 0x010
#define IN_OPEN          0x020
#define IN_MOVED_FROM    0x040
#define IN_MOVED_TO      0x080
#define IN_CREATE        0x100
#define IN_DELETE        0x200
#define IN_DELETE_SELF   0x400
#define IN_MOVE_SELF     0x800
#define IN_ISDIR         0x40000000     /* (in an event: it's about a directory) */

/* --- record locks (fs/lock.c) --- */
int32_t lock_fcntl(struct file *f, uint32_t cmd, uintptr_t arg);   /* F_*LK* */
int     lock_fcntl_cmd(uint32_t cmd);   /* is cmd one of those? */
void    lock_release(const void *owner, const struct inode *i) __attribute__((cold));
void    lock_exit(struct process *p);
extern uint32_t nlocks;                 /* record locks in the system: usually 0 */

/* close(): process p's POSIX locks on i go. (One test when nobody locks.) */
static inline void lock_close(struct process *p, struct inode *i)
{
    if (__builtin_expect(nlocks != 0, 0) && i)
        lock_release(p, i);
}

/* The last close of an open file: its OFD locks go. */
static inline void lock_file_gone(struct file *f)
{
    if (__builtin_expect(nlocks != 0, 0))
        lock_release(f, NULL);
}

/* (cold: tells GCC to keep the callers' common path straight — measured:
   without it the check alone cost `read` 38 cycles, docs/performance.md) */
void     fsnotify(struct inode *i, uint32_t mask) __attribute__((cold));
void     fsnotify_entry(struct inode *dir, struct inode *child, uint32_t mask,
                        uint32_t cookie, const char *name, uint32_t len)
                        __attribute__((cold));
void     fsnotify_deleted(struct inode *i);
/* Watches don't keep a file system mounted (Linux): how many an inode has,
   and, at umount, IN_UNMOUNT + IN_IGNORED for those on m, and they end. */
uint32_t fsnotify_watches_on(const struct inode *i);
void     fsnotify_unmount(const struct mount *m);
uint32_t fsnotify_cookie(void);         /* pairs IN_MOVED_FROM with IN_MOVED_TO */

extern uint32_t inotify_watches;        /* watches in the system: usually 0 */

/* Something happened to i itself (opened, read, written, closed, attributes):
   to its watchers and its directory's. With no watches anywhere — the normal
   case — one load and a branch: this is on every open, read and write. */
static inline void notify(struct inode *i, uint32_t mask)
{
    if (__builtin_expect(inotify_watches != 0, 0) && i &&
        ((i->flags & I_WATCHED) || (i->parent && (i->parent->flags & I_WATCHED))))
        fsnotify(i, mask);
}

/* Something happened to the entry `name` in dir (created, deleted, moved). */
static inline void notify_entry(struct inode *dir, struct inode *child, uint32_t mask,
                                uint32_t cookie, const char *name, uint32_t len)
{
    if (__builtin_expect(inotify_watches != 0, 0) && (dir->flags & I_WATCHED))
        fsnotify_entry(dir, child, mask, cookie, name, len);
}

/* --- file systems ------------------------------------------------------- */
void initramfs_load(const uint8_t *archive, uint32_t size);   /* initramfs.c */
void dev_init(void);                            /* dev.c: /dev and its nodes */
void dev_attach(struct inode *i);               /* give a device node its ops */
/* A device that appears after boot (/dev/fb0): add its node to /dev. */
void dev_make_node(const char *name, uint32_t rdev);
extern const struct file_ops console_ops;
extern struct file console_file;                /* the shared open console */
int  pipe_create(struct file **rd, struct file **wr);         /* pipe.c */
int  fifo_open(struct inode *i, uint32_t flags, struct file **out);  /* a named pipe */
/* procfs.c: mount -t proc, and readlink of /proc/self/fd/N or /proc/self/exe. */
int  procfs_mount(const char *path);
/* A node's link count for stat(): /proc/self/task's depends on the caller. */
uint32_t procfs_nlink(const struct inode *i);
int  procfs_readlink(struct inode *link, char *buf, uint32_t size);
/* sysfs.c: mount -t sysfs (just what Xorg checks: /sys/class/graphics/fb0). */
int  sysfs_mount(const char *path);
struct net_device;
void sysfs_net_add(struct net_device *d);       /* netdevice.c, as a link is listed */
/* /proc/<pid>: made with the process, gone when it is reaped (procfs.c). */
void procfs_pid_add(int pid);
void procfs_pid_remove(int pid);
/* The host's name (sethostname), for /proc/sys/kernel/hostname. */
const char *uts_nodename(void);
/* A RAM file system mounted with mount(2) (tmpfs, proc, sysfs): noted
   for /proc/mounts, which ext2 and FAT mounts are in already. */
void vfs_note_mount(const char *path, const char *type);
int  vfs_noted_count(void);
void vfs_noted_at(int i, const char **path, const char **type);
/* Take a node out of its directory (a generated file system's own). */
void vfs_remove(struct inode *i);
uint32_t vfs_mount_gen(void);               /* bumped when the mount table changes */

/* dev.c: /dev/sda and its partitions, whose positions pass 4 GiB. */
struct blockdev;
int32_t blkfile_rw(struct file *f, uintptr_t ubuf, uint32_t n, int writing, int64_t at);
int64_t blkfile_seek(struct file *f, int64_t off, uint32_t whence);
int     blkdev_busy(struct blockdev *d);    /* open for writing: not mountable */
/* flock.c: drop f's flock() (its last fd closed). */
void flock_release(struct file *f);
extern struct wait_queue poll_wait;     /* poll.c: woken when a file's state changes */

#endif
