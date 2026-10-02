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
#include <proc.h>
#include <string.h>
#include <uaccess.h>
#include <vfs.h>

#define HASH_SIZE  512              /* buckets (power of two) */
#define MAX_LINKS  8                /* symlinks followed in one lookup */

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

/* --- path lookup -------------------------------------------------------- */

/* Walk path[0..len) starting in `dir`. A symbolic link along the way is
   replaced by its target (read straight from the archive), resolved from
   the link's own directory; `links` counts how many we've followed, to
   stop loops (a -> b -> a). */
static int walk(struct inode *dir, const char *path, uint32_t len, int follow_last,
                int *links, struct inode **out)
{
    struct inode *cur = (len && path[0] == '/') ? &root : dir;
    uint32_t pos = 0;

    for (;;) {
        while (pos < len && path[pos] == '/')
            pos++;
        if (pos >= len)
            break;
        const char *name = path + pos;
        uint32_t n = 0;
        while (pos + n < len && path[pos + n] != '/')
            n++;
        pos += n;
        uint32_t rest = pos;
        while (rest < len && path[rest] == '/')
            rest++;
        int last = rest >= len;             /* nothing but slashes after it */

        if (!S_ISDIR(cur->mode))
            return -ENOTDIR;
        if (n > NAME_MAX)
            return -ENAMETOOLONG;
        if (n == 1 && name[0] == '.')
            continue;
        if (n == 2 && name[0] == '.' && name[1] == '.') {
            cur = cur->parent;      /* root's parent is root */
            continue;
        }
        struct inode *next = vfs_child(cur, name, n);
        if (!next)
            return -ENOENT;
        if (S_ISLNK(next->mode) && (!last || follow_last)) {
            if (++*links > MAX_LINKS)
                return -ELOOP;
            int err = walk(cur, (const char *)next->data, next->size, 1, links, &next);
            if (err)
                return err;
        }
        cur = next;
    }
    *out = cur;
    return 0;
}

int vfs_lookup_at(struct inode *dir, const char *path, int follow, struct inode **out)
{
    /* Absolute paths start at "/", relative ones at `dir` — by default
       the current directory. */
    if (!dir) {
        struct process *proc = proc_current();
        dir = (proc && proc->cwd) ? proc->cwd : &root;
    }
    int links = 0;
    return walk(dir, path, strlen(path), follow, &links, out);
}

int vfs_lookup(const char *path, struct inode **out)
{
    return vfs_lookup_at(NULL, path, 1, out);
}

int vfs_lookup_nofollow(const char *path, struct inode **out)
{
    return vfs_lookup_at(NULL, path, 0, out);
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

static const struct file_ops memfile_ops = { .read = mem_read, .write = mem_write };

/* --- open files ----------------------------------------------------------- */

struct file *file_new(const struct file_ops *ops, struct inode *i, uint32_t flags)
{
    struct file *f = kmalloc(sizeof *f);
    if (!f)
        return NULL;
    f->ops = ops;
    f->inode = i;
    f->pos = 0;
    f->flags = flags;
    f->refs = 1;
    f->priv = NULL;
    return f;
}

int vfs_open(const char *path, uint32_t flags, struct file **out)
{
    return vfs_open_at(NULL, path, flags, out);
}

int vfs_open_at(struct inode *dir, const char *path, uint32_t flags, struct file **out)
{
    struct inode *i;
    int err = vfs_lookup_at(dir, path, !(flags & O_NOFOLLOW), &i);
    if (err)
        return (err == -ENOENT && (flags & O_CREAT)) ? -EROFS : err;
    if (S_ISLNK(i->mode))
        return -ELOOP;              /* O_NOFOLLOW on a symlink */
    if ((flags & O_DIRECTORY) && !S_ISDIR(i->mode))
        return -ENOTDIR;
    int writing = (flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC);
    if (writing && !i->ops)         /* devices may be written; files not yet */
        return S_ISDIR(i->mode) ? -EISDIR : -EROFS;

    struct file *f = file_new(i->ops ? i->ops : &memfile_ops, i, flags);
    if (!f)
        return -ENOMEM;
    if (S_ISDIR(i->mode))
        f->priv = i->children;      /* getdents64 walks from here */
    *out = f;
    return 0;
}

void file_put(struct file *f)
{
    if (--f->refs == 0) {
        if (f->ops->release)
            f->ops->release(f);     /* e.g. a pipe end closing */
        if (f != &console_file)
            kfree(f);
    }
}

/* --- file descriptors ----------------------------------------------------- */
/* An fd is an index into the current process's file table. */

int fd_install(struct file *f)
{
    struct file **files = proc_current()->files;
    for (int fd = 0; fd < FD_MAX; fd++) {       /* lowest free fd, like POSIX */
        if (!files[fd]) {
            files[fd] = f;
            return fd;
        }
    }
    return -EMFILE;
}

int fd_install_at(int fd, struct file *f)
{
    if (fd < 0 || fd >= FD_MAX)
        return -EBADF;
    struct process *p = proc_current();
    struct file *old = p->files[fd];
    p->files[fd] = f;
    p->cloexec &= ~(1u << fd);
    if (old)
        file_put(old);              /* dup2 closes what was there */
    return fd;
}

struct file *fd_get(int fd)
{
    struct process *p = proc_current();
    if (fd < 0 || fd >= FD_MAX || !p)
        return NULL;
    return p->files[fd];
}

int fd_close(int fd)
{
    struct file *f = fd_get(fd);
    if (!f)
        return -EBADF;
    struct process *p = proc_current();
    p->files[fd] = NULL;
    p->cloexec &= ~(1u << fd);
    file_put(f);
    return 0;
}

void fd_open_console(struct process *p)
{
    for (int fd = 0; fd < 3; fd++) {
        p->files[fd] = &console_file;
        console_file.refs++;
    }
}

void fd_copy_all(struct process *dst, struct process *src)
{
    for (int fd = 0; fd < FD_MAX; fd++) {
        dst->files[fd] = src->files[fd];
        if (dst->files[fd])
            dst->files[fd]->refs++;     /* fork: both share the open file */
    }
}

void fd_close_all(struct process *p)
{
    for (int fd = 0; fd < FD_MAX; fd++) {
        if (p->files[fd]) {
            file_put(p->files[fd]);
            p->files[fd] = NULL;
        }
    }
}
