/*
 * procfs.c — /proc, the kernel's own information shown as files
 *
 * On Linux, /proc is how programs ask the kernel about processes: `ps`
 * reads /proc/<pid>/stat, `free` reads /proc/meminfo. Nothing is stored;
 * each file is made up when it's read.
 *
 * The first user is musl's ttyname(): "which terminal is fd 0?" It reads
 * the link /proc/self/fd/0 (say "/dev/tty1"), then checks that stat() of
 * that name is the same file as fstat(0). `login` refuses to run without
 * a terminal name. So for now /proc holds just that:
 *
 *   /proc/self/fd/0 ... 31    one link per fd slot
 *
 * The tree is built once, at mount time, from ordinary RAM nodes. What's
 * special is what a link points to: "self" means whoever is looking, so
 * each fd link is marked I_PROC_FD and vfs.c asks the CALLER's fd table
 * when the link is used. Looking one up is O(1) — no search, nothing
 * allocated — and there is nothing to keep up to date as processes come
 * and go. (Linux makes self a link to /proc/<pid>; that comes with the
 * per-process directories, ROADMAP 8j.)
 */
#include <errno.h>
#include <string.h>
#include <vfs.h>

/* The links' names, "0".."31": nodes point at their name, not a copy. */
static char fd_names[FD_MAX][3];

int procfs_mount(const char *path)
{
    int err = vfs_mount_ram(path, 0555);
    if (err)
        return err;
    struct inode *proc, *self, *fd;
    err = vfs_lookup(path, &proc);      /* the new RAM directory on top */
    if (err)
        return err;
    self = vfs_add(proc, "self", 4, S_IFDIR | 0555);
    fd = self ? vfs_add(self, "fd", 2, S_IFDIR | 0555) : NULL;
    if (!fd)
        return -ENOMEM;
    for (int n = 0; n < FD_MAX; n++) {
        char *s = fd_names[n];
        uint32_t len = n < 10 ? 1 : 2;
        s[0] = n < 10 ? '0' + n : '0' + n / 10;
        s[1] = '0' + n % 10;
        struct inode *l = vfs_add(fd, s, len, S_IFLNK | 0700);
        if (!l)
            return -ENOMEM;
        l->flags |= I_PROC_FD;
        l->fs_ino = n;
        l->size = 64;                   /* Linux shows that for these too */
    }
    return 0;
}

/* The path of the file the caller's fd is open on. */
int procfs_readlink(struct inode *link, char *buf, uint32_t size)
{
    struct file *f = fd_get(link->fs_ino);
    if (!f || !f->inode)
        return -ENOENT;
    return vfs_path(f->inode, buf, size);
}
