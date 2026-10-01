#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <thread.h>

/*
 * The VFS ("virtual file system"): files and directories, whatever they're
 * stored on. Today that's the initramfs (fs/initramfs.c) plus the console;
 * ext2 and FAT32 plug in later.
 *
 *   inode   one file or directory: type, size, where its data is
 *   file    an OPEN file: which inode, the current position, the flags
 *   fd      a small number (0, 1, 2, 3...) a program uses to name an open
 *           file; an index into the thread's file table
 */

/* File types and permission bits (st_mode), as Linux defines them. */
#define S_IFMT    0170000
#define S_IFREG   0100000           /* regular file */
#define S_IFDIR   0040000           /* directory */
#define S_IFLNK   0120000           /* symbolic link */
#define S_IFCHR   0020000           /* character device (the console) */
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)

/* open() flags (Linux i386 values) */
#define O_ACCMODE   3
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_TRUNC     01000
#define O_DIRECTORY 0200000

#define NAME_MAX    255
#define PATH_MAX    256             /* ours; Linux allows 4096 */
#define FD_MAX      THREAD_FILES

struct file;

struct file_ops {
    /* Copy up to n bytes to/from USER memory at the file's position.
       Return bytes done, or -errno. */
    int32_t (*read)(struct file *f, void *user_buf, uint32_t n);
    int32_t (*write)(struct file *f, const void *user_buf, uint32_t n);
};

struct inode {
    uint32_t ino;
    uint32_t mode;                  /* type + permissions */
    uint32_t size;
    uint32_t nlink, uid, gid, mtime;
    const uint8_t *data;            /* initramfs: the contents, in the archive */
    const struct file_ops *ops;     /* how to read/write it */

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
    uint32_t flags;                 /* the open() flags */
    uint32_t refs;                  /* fds pointing at it */
};

/* --- the tree ---------------------------------------------------------- */
struct inode *vfs_root(void);

/* Find a path (in kernel memory). Relative paths start at "/" (there is no
   chdir yet). 0, -ENOENT, -ENOTDIR or -ENAMETOOLONG. */
int vfs_lookup(const char *path, struct inode **out);

/* The entry `name` (len bytes) in directory `dir`, or NULL. O(1). */
struct inode *vfs_child(struct inode *dir, const char *name, uint32_t len);

/* Add an entry to a directory (used by file systems while building). */
struct inode *vfs_add(struct inode *dir, const char *name, uint32_t len, uint32_t mode);

/* --- open files and fds ----------------------------------------------- */
int  vfs_open(const char *path, uint32_t flags, struct file **out);
void file_put(struct file *f);                  /* drop one reference */

int  fd_install(struct file *f);                /* new fd in the current thread */
struct file *fd_get(int fd);                    /* NULL if not open */
int  fd_close(int fd);
void fd_open_console(struct thread *t);         /* fds 0, 1, 2 */
void fd_close_all(struct thread *t);

/* Unpack the initramfs into the tree (fs/initramfs.c). */
void initramfs_load(const uint8_t *archive, uint32_t size);

#endif
