#ifndef VFS_H
#define VFS_H

#include <proc.h>
#include <stdint.h>

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
#define S_IFMT    0170000
#define S_IFREG   0100000           /* regular file */
#define S_IFDIR   0040000           /* directory */
#define S_IFLNK   0120000           /* symbolic link */
#define S_IFCHR   0020000           /* character device (the console) */
#define S_IFIFO   0010000           /* pipe */
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

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

struct file;
struct inode;

struct file_ops {
    /* Copy up to n bytes to/from USER memory at the file's position.
       Return bytes done, or -errno. */
    int32_t (*read)(struct file *f, void *user_buf, uint32_t n);
    int32_t (*write)(struct file *f, const void *user_buf, uint32_t n);
    void (*release)(struct file *f);        /* last close (may be NULL) */
    int32_t (*ioctl)(struct file *f, uint32_t cmd, uint32_t arg);   /* or NULL */
    uint32_t (*poll)(struct file *f);       /* POLLIN/POLLOUT/... now (NULL: both) */
};

/* inode flags */
#define I_NAME_OWNED 1              /* name is our kmalloc'd copy */
#define I_DATA_OWNED 2              /* data (a symlink's target) is ours too */
#define I_UNLINKED   4              /* removed from the tree; freed at refs == 0 */
#define I_UNLOADED   8              /* a disk directory whose entries aren't read yet */
#define I_MOUNTED   16              /* the root of a mounted file system */

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
    int (*statfs)(struct mount *m, uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                  uint32_t *files, uint32_t *ffree);
};

struct mount {
    const struct fs_ops *ops;
    void *priv;                     /* the file system's own state */
    struct inode *root;             /* where it appears in the tree */
    const char *type;               /* "ext2" (for /proc/mounts, df) */
    uint32_t magic;                 /* statfs f_type; 0 means ext2 */
    uint32_t dev_minor;             /* st_dev: 0:1 is the RAM file system */
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
};

/* --- the tree ---------------------------------------------------------- */
struct inode *vfs_root(void);

/* Find a path (in kernel memory), following symbolic links — also in the
   last component unless `nofollow`. Relative paths start at the current
   directory. 0, -ENOENT, -ENOTDIR, -ELOOP or -ENAMETOOLONG. */
int vfs_lookup(const char *path, struct inode **out);
int vfs_lookup_nofollow(const char *path, struct inode **out);
/* ...starting from directory `dir` (NULL: the current directory). */
int vfs_lookup_at(struct inode *dir, const char *path, int follow, struct inode **out);

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

/* Reading a directory (getdents64), in order, while entries may come and
   go between calls: the first entry not yet returned through f (NULL: no
   more); the entry after i; and "f has returned everything up to i"
   (NULL: all of it). */
struct inode *vfs_dir_first(struct file *f);
struct inode *vfs_dir_after(struct inode *i);
int  vfs_dir_stop(struct file *f, struct inode *last);
void vfs_dir_rewind(struct file *f);

int  fd_install(struct file *f);                /* lowest free fd */
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
int  vfs_mount_count(void);
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
void    ramfs_usage(uint32_t *used_pages, uint32_t *max_pages);   /* statfs, df */
uint32_t time_now(void);                    /* seconds since 1970 (sys_info.c) */

/* --- file systems ------------------------------------------------------- */
void initramfs_load(const uint8_t *archive, uint32_t size);   /* initramfs.c */
void dev_init(void);                            /* dev.c: /dev and its nodes */
void dev_attach(struct inode *i);               /* give a device node its ops */
extern const struct file_ops console_ops;
extern struct file console_file;                /* the shared open console */
int  pipe_create(struct file **rd, struct file **wr);         /* pipe.c */
extern struct wait_queue poll_wait;     /* poll.c: woken when a file's state changes */

#endif
