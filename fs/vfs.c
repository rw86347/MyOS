/*
 * vfs.c — the file tree, path lookup, open files and file descriptors
 *
 * Speed (rule 1 in CLAUDE.md): looking up a path costs one hash-table probe
 * per component ("/data/numbers.txt" = 2 probes), however many files there
 * are. The table is keyed on (directory, name), like Linux's dentry cache.
 * File data is never copied into a cache: read() copies straight from where
 * the file system keeps it (for the initramfs: the archive in RAM) into the
 * program's buffer.
 */
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <string.h>
#include <uaccess.h>
#include <vfs.h>

#define HASH_SIZE 512               /* buckets (power of two) */

static struct inode root = {
    .ino = 1, .mode = S_IFDIR | 0755, .nlink = 2, .name = "", .parent = &root,
};
static struct inode *hash[HASH_SIZE];
static uint32_t next_ino = 2;

struct inode *vfs_root(void) { return &root; }

/* FNV-1a over the name, mixed with the directory's address. */
static uint32_t hash_of(const struct inode *dir, const char *name, uint32_t len)
{
    uint32_t h = 2166136261u ^ (uint32_t)(uintptr_t)dir;
    for (uint32_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)name[i]) * 16777619u;
    return h & (HASH_SIZE - 1);
}

struct inode *vfs_child(struct inode *dir, const char *name, uint32_t len)
{
    for (struct inode *i = hash[hash_of(dir, name, len)]; i; i = i->hash_next)
        if (i->parent == dir && i->name_len == len && memcmp(i->name, name, len) == 0)
            return i;
    return NULL;
}

struct inode *vfs_add(struct inode *dir, const char *name, uint32_t len, uint32_t mode)
{
    struct inode *i = kmalloc(sizeof *i);
    if (!i)
        return NULL;
    memset(i, 0, sizeof *i);
    i->ino = next_ino++;
    i->mode = mode;
    i->nlink = S_ISDIR(mode) ? 2 : 1;
    i->name = name;
    i->name_len = len;
    i->parent = dir;
    i->next_sibling = dir->children;
    dir->children = i;
    uint32_t h = hash_of(dir, name, len);
    i->hash_next = hash[h];
    hash[h] = i;
    return i;
}

int vfs_lookup(const char *path, struct inode **out)
{
    struct inode *cur = &root;      /* absolute or relative: both from "/" */
    const char *p = path;

    for (;;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *name = p;
        while (*p && *p != '/')
            p++;
        uint32_t len = p - name;

        if (!S_ISDIR(cur->mode))
            return -ENOTDIR;
        if (len > NAME_MAX)
            return -ENAMETOOLONG;
        if (len == 1 && name[0] == '.')
            continue;
        if (len == 2 && name[0] == '.' && name[1] == '.') {
            cur = cur->parent;      /* root's parent is root */
            continue;
        }
        cur = vfs_child(cur, name, len);
        if (!cur)
            return -ENOENT;
    }
    *out = cur;
    return 0;
}

/* --- regular files in memory (initramfs) -------------------------------- */

/* Copy at most this much per read() call: every syscall must finish
   quickly (interrupts are off inside it). 256 KiB is about 0.1 ms of copying
   on a Pentium 4. Programs (and C libraries) loop on short reads anyway. */
#define READ_MAX (256 * 1024)

static int32_t mem_read(struct file *f, void *user_buf, uint32_t n)
{
    struct inode *i = f->inode;
    if (S_ISDIR(i->mode))
        return -EISDIR;
    if (f->pos >= i->size)
        return 0;                   /* end of file */
    if (n > i->size - f->pos)
        n = i->size - f->pos;
    if (n > READ_MAX)
        n = READ_MAX;
    if (copy_to_user(user_buf, i->data + f->pos, n) < 0)
        return -EFAULT;
    f->pos += n;
    return n;
}

static int32_t mem_write(struct file *f, const void *user_buf, uint32_t n)
{
    (void)f; (void)user_buf; (void)n;
    return -EROFS;                  /* the initramfs is read-only (for now) */
}

const struct file_ops memfile_ops = { mem_read, mem_write };

/* --- open files ----------------------------------------------------------- */

int vfs_open(const char *path, uint32_t flags, struct file **out)
{
    struct inode *i;
    int err = vfs_lookup(path, &i);
    if (err)
        return flags & O_CREAT ? -EROFS : err;
    if ((flags & O_DIRECTORY) && !S_ISDIR(i->mode))
        return -ENOTDIR;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))
        return S_ISDIR(i->mode) ? -EISDIR : -EROFS;

    struct file *f = kmalloc(sizeof *f);
    if (!f)
        return -ENOMEM;
    f->ops = i->ops ? i->ops : &memfile_ops;
    f->inode = i;
    f->pos = 0;
    f->flags = flags;
    f->refs = 1;
    *out = f;
    return 0;
}

void file_put(struct file *f)
{
    if (--f->refs == 0)
        kfree(f);
}

/* --- the console: fds 0, 1, 2 ---------------------------------------------- */

static int32_t console_read(struct file *f, void *user_buf, uint32_t n)
{
    (void)f; (void)user_buf; (void)n;
    return 0;                       /* no keyboard input for programs yet */
}

int32_t console_write_user(struct file *f, const void *user_buf, uint32_t n);

static const struct file_ops console_ops = { console_read, console_write_user };

static struct inode console_inode = {
    .ino = 0, .mode = S_IFCHR | 0620, .nlink = 1, .name = "console",
    .name_len = 7, .ops = &console_ops,
};
/* One shared open console; it never goes away (refs never reach 0). */
static struct file console_file = {
    .ops = &console_ops, .inode = &console_inode, .flags = O_RDWR, .refs = 1,
};

/* --- file descriptors ------------------------------------------------------- */

int fd_install(struct file *f)
{
    struct file **files = thread_current()->files;
    for (int fd = 0; fd < FD_MAX; fd++) {       /* lowest free fd, like POSIX */
        if (!files[fd]) {
            files[fd] = f;
            return fd;
        }
    }
    return -EMFILE;
}

struct file *fd_get(int fd)
{
    if (fd < 0 || fd >= FD_MAX)
        return NULL;
    return thread_current()->files[fd];
}

int fd_close(int fd)
{
    struct file *f = fd_get(fd);
    if (!f)
        return -EBADF;
    thread_current()->files[fd] = NULL;
    file_put(f);
    return 0;
}

void fd_open_console(struct thread *t)
{
    for (int fd = 0; fd < 3; fd++) {
        t->files[fd] = &console_file;
        console_file.refs++;
    }
}

void fd_close_all(struct thread *t)
{
    for (int fd = 0; fd < FD_MAX; fd++) {
        if (t->files[fd]) {
            file_put(t->files[fd]);
            t->files[fd] = NULL;
        }
    }
}
