#ifndef VFS_H
#define VFS_H

#include <proc.h>
#include <stdint.h>

/*
 * The VFS ("virtual file system"): files and directories, whatever they're
 * stored on. Today: the initramfs (fs/initramfs.c), pipes (fs/pipe.c) and
 * devices like the console (fs/dev.c); ext2 and FAT32 plug in later.
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

/* A directory whose entries are made up on the fly (/proc): look up one
   name, or list them all (call fill once per entry; stop if it returns
   nonzero). */
struct dir_ops {
    struct inode *(*lookup)(struct inode *dir, const char *name, uint32_t len);
    int (*list)(struct inode *dir, uint32_t index, const char **name, uint32_t *len,
                struct inode **child);         /* entry #index: 0, or -1 = no more */
};

struct file_ops {
    /* Copy up to n bytes to/from USER memory at the file's position.
       Return bytes done, or -errno. */
    int32_t (*read)(struct file *f, void *user_buf, uint32_t n);
    int32_t (*write)(struct file *f, const void *user_buf, uint32_t n);
    void (*release)(struct file *f);        /* last close (may be NULL) */
    int (*open)(struct file *f);            /* just opened (may be NULL) */
    int32_t (*ioctl)(struct file *f, uint32_t cmd, uint32_t arg);   /* or NULL */
    uint32_t (*poll)(struct file *f);       /* POLLIN/POLLOUT/... now (NULL: both) */
};

struct inode {
    uint32_t ino;
    uint32_t mode;                  /* type + permissions */
    uint32_t size;
    uint32_t nlink, uid, gid;
    uint32_t atime, mtime, ctime;   /* seconds since 1970 */
    uint32_t rdev;                  /* devices: (major << 8) | minor */

    /* The contents. A file from the initramfs starts out pointing into the
       archive (`data`, read-only, no copy). Its first change moves the
       contents into RAM pages of its own (`pages`: physical frame numbers,
       0 = a hole of zeros) — see ramfs.c. A symlink's target is in `data`. */
    const uint8_t *data;
    uint32_t *pages;
    uint32_t page_cap;              /* entries in pages[] */

    const struct file_ops *ops;     /* how to read/write it (NULL: ramfs) */
    const struct dir_ops *dops;     /* generated directories (/proc), or NULL */
    uint32_t open_count;            /* open files using it */
    uint8_t name_owned;             /* name is our kmalloc'd copy */
    uint8_t mapped;                 /* mmap'd at some point: keep contents */

    /* Its name and place in the tree. The name points into the archive
       (not copied, not NUL-terminated: use name_len). */
    const char *name;
    uint32_t name_len;
    struct inode *parent;
    struct inode *children, *next_sibling;  /* directories: their entries */
    struct inode *hash_next;                /* lookup hash chain (vfs.c) */
};

struct file {
    const struct file_ops *ops;
    struct inode *inode;
    uint32_t pos;                   /* where the next read starts */
    uint32_t flags;                 /* the open() flags (O_APPEND, O_NONBLOCK...) */
    uint32_t refs;                  /* fds pointing at it */
    void *priv;                     /* pipe: the pipe; directory: next entry */
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
int  vfs_open(const char *path, uint32_t flags, struct file **out);
int  vfs_open_at(struct inode *dir, const char *path, uint32_t flags, struct file **out);
struct file *file_new(const struct file_ops *ops, struct inode *i, uint32_t flags);
void file_put(struct file *f);                  /* drop one reference */

int  fd_install(struct file *f);                /* lowest free fd */
int  fd_install_at(int fd, struct file *f);     /* dup2: exactly this fd */
struct file *fd_get(int fd);                    /* NULL if not open */
int  fd_close(int fd);
void fd_open_console(struct process *p);        /* fds 0, 1, 2 */
void fd_copy_all(struct process *dst, struct process *src);   /* fork */
void fd_close_all(struct process *p);

/* --- changing the tree (vfs.c) ------------------------------------------ */

/* Split a path into its directory (found, following links) and its last
   name (pointer into path, len). 0 or -errno. */
int vfs_lookup_parent(struct inode *start, const char *path, struct inode **dir,
                      const char **name, uint32_t *len);

/* A new, empty file / directory / symlink... in dir (name copied). */
int vfs_create(struct inode *dir, const char *name, uint32_t len, uint32_t mode,
               struct inode **out);
int vfs_unlink(struct inode *dir, struct inode *i);         /* rm / rmdir */
int vfs_rename(struct inode *i, struct inode *new_dir, const char *name, uint32_t len);
void vfs_release_inode(struct inode *i);    /* a file was closed */

/* --- file contents (ramfs.c) --------------------------------------------- */
int32_t file_read_at(struct inode *i, uint32_t pos, void *user_buf, uint32_t n);
int32_t file_write_at(struct inode *i, uint32_t pos, const void *user_buf, uint32_t n);
void    file_copy_out(struct inode *i, uint32_t pos, void *kbuf, uint32_t n);  /* kernel */
int     file_truncate(struct inode *i, uint32_t size);
void    file_free(struct inode *i);
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
