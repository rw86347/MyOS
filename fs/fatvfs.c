/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * fatvfs.c — FAT32 as a file system in the tree (step 9e, read half)
 *
 * fat32.c knows the on-disk format. This file is the VFS side: a directory's
 * entries become inodes the first time someone looks inside, and read()
 * copies file bytes out. Writing is refused (EROFS). The cluster allocator
 * and directory updates that writing needs are the rest of step 9e.
 *
 * Linux reports this type as MSDOS_SUPER_MAGIC (0x4d44).
 */
#include <errno.h>
#include <fat32.h>
#include <kheap.h>
#include <kprintf.h>
#include <string.h>
#include <uaccess.h>
#include <vfs.h>

#define MSDOS_MAGIC 0x4d44
#define FAT_CHUNK   (64 * 1024)     /* bytes per disk read in read() */

struct fatmnt {
    struct mount m;
    struct fat32_fs fat;
};

static struct fat32_fs *fat_of(struct inode *i)
{
    return &((struct fatmnt *)i->mnt)->fat;
}

static void ent_of(struct inode *i, struct fat32_dirent *e)
{
    memset(e, 0, sizeof *e);
    e->cluster = i->fs_ino;
    e->size = i->size;
    e->attr = S_ISDIR(i->mode) ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
}

static int ro(void) { return -EROFS; }

static int fat_create(struct inode *dir, struct inode *i)
{
    (void)dir;
    (void)i;
    return ro();
}

static int fat_unlink(struct inode *dir, struct inode *i)
{
    (void)dir;
    (void)i;
    return ro();
}

static int fat_rename(struct inode *i, struct inode *new_dir, const char *name,
                      uint32_t len, struct inode *replaced)
{
    (void)i;
    (void)new_dir;
    (void)name;
    (void)len;
    (void)replaced;
    return ro();
}

static int fat_setattr(struct inode *i)
{
    (void)i;
    return ro();
}

static int fat_truncate(struct inode *i, uint32_t size)
{
    (void)i;
    (void)size;
    return ro();
}

static int fat_read_kernel(struct inode *i, uint32_t pos, void *kbuf, uint32_t n)
{
    struct fat32_dirent e;
    ent_of(i, &e);
    if (pos >= i->size || n == 0)
        return 0;
    if (n > i->size - pos)
        n = i->size - pos;
    int got = fat32_read(fat_of(i), &e, pos, kbuf, n);
    if (got < 0 || (uint32_t)got != n)
        return -EIO;
    return 0;
}

static int32_t fat_read(struct file *f, void *user_buf, uint32_t n)
{
    struct inode *i = f->inode;
    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    if (f->pos >= i->size)
        return 0;
    n = n > i->size - f->pos ? i->size - f->pos : n;
    if (n > RW_MAX)
        n = RW_MAX;

    /* Through a 64 KiB kernel buffer the disk reads straight into, one
       copy to the program: big pieces, so a run of clusters is one disk
       command (fat32_read), not 128. */
    uint32_t size = n < FAT_CHUNK ? (n + 511) & ~511u : FAT_CHUNK;
    uint8_t *chunk = kmalloc_contig(size);
    if (!chunk)
        return -ENOMEM;
    uint32_t done = 0;
    int32_t err = 0;
    while (done < n) {
        uint32_t k = n - done;
        if (k > size)
            k = size;
        if (fat_read_kernel(i, f->pos + done, chunk, k) < 0)
            err = -EIO;
        else if (copy_to_user((uint8_t *)user_buf + done, chunk, k) < 0)
            err = -EFAULT;
        if (err)
            break;
        done += k;
    }
    kfree(chunk);
    if (!done)
        return err;
    f->pos += done;
    return (int32_t)done;
}

static int32_t fat_write(struct file *f, const void *user_buf, uint32_t n)
{
    (void)f;
    (void)user_buf;
    (void)n;
    return -EROFS;
}

static const struct file_ops fat_ops = { .read = fat_read, .write = fat_write };

struct add_ctx {
    struct inode *dir;
    int err;
};

static int add_cb(const struct fat32_dirent *e, void *ctx)
{
    struct add_ctx *a = ctx;
    uint32_t len = 0;
    while (e->name[len] && len < NAME_MAX)
        len++;
    if (len == 0)
        return 0;
    char *name = kmalloc(len);
    if (!name) {
        a->err = -ENOMEM;
        return 1;
    }
    memcpy(name, e->name, len);
    uint32_t mode = (e->attr & FAT_ATTR_DIRECTORY) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    if (e->attr & FAT_ATTR_READ_ONLY)
        mode &= ~0222u;
    struct inode *c = vfs_add(a->dir, name, len, mode);
    if (!c) {
        kfree(name);
        a->err = -ENOMEM;
        return 1;
    }
    c->flags |= I_NAME_OWNED;
    c->fs = a->dir->fs;
    c->mnt = a->dir->mnt;
    c->fs_ino = e->cluster;
    c->size = e->size;
    c->nlink = 1;
    if (S_ISDIR(mode)) {
        c->nlink = 2;
        c->flags |= I_UNLOADED;
    } else {
        c->ops = &fat_ops;
    }
    return 0;
}

static int fat_load_dir(struct inode *dir)
{
    if (!(dir->flags & I_UNLOADED))
        return 0;
    dir->flags &= ~I_UNLOADED;
    struct fat32_dirent self;
    ent_of(dir, &self);
    struct add_ctx a = { dir, 0 };
    if (fat32_readdir(fat_of(dir), &self, add_cb, &a) < 0)
        return -EIO;
    return a.err;
}

static int fat_statfs(struct mount *m, uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                      uint32_t *files, uint32_t *ffree)
{
    struct fat32_fs *fs = &((struct fatmnt *)m)->fat;
    *bsize = fs->sectors_per_cluster * 512;
    *blocks = fs->total_clusters;
    *bfree = 0;                         /* free-cluster count is part of write */
    *files = 0;
    *ffree = 0;
    return 0;
}

static const struct fs_ops fat_fsops = {
    .load_dir = fat_load_dir,
    .create = fat_create,
    .unlink = fat_unlink,
    .rename = fat_rename,
    .setattr = fat_setattr,
    .truncate = fat_truncate,
    .read_kernel = fat_read_kernel,
    .statfs = fat_statfs,
};

int fat_vfs_mount(struct blockdev *dev, const char *path)
{
    struct fatmnt *fm = kmalloc(sizeof *fm);
    if (!fm)
        return -ENOMEM;
    memset(fm, 0, sizeof *fm);
    if (fat32_mount(&fm->fat, dev) < 0) {
        kfree(fm);
        return -EINVAL;
    }
    fm->m.ops = &fat_fsops;
    fm->m.priv = fm;
    fm->m.type = "vfat";
    strlcpy(fm->m.source, "/dev/", sizeof fm->m.source);
    strlcpy(fm->m.source + 5, dev->name, sizeof fm->m.source - 5);
    fm->m.magic = MSDOS_MAGIC;
    int err = vfs_mount(path, &fm->m, fm->fat.root_cluster, 0755);
    if (err) {
        kfree(fm);
        return err;
    }
    klog("fat32: %s mounted at %s (read-only), label \"%s\"\n",
            dev->name, path, fm->fat.label);
    return 0;
}
