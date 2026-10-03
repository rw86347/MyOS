/* disktarget.c — run the kernel's real filesystem code against a crafted
 * disk image, under AddressSanitizer.
 *
 * Why this exists: on 2026-10-03 an audit found seven out-of-bounds accesses
 * in fs/ext2.c, every one of them from believing a number written on the
 * disk. They were found by reading the code. That is the wrong way round —
 * `make attacks` had been catching this class in the network stack for a
 * while, and the filesystem had no equivalent, which is why nothing noticed
 * that a USB stick could write four bytes anywhere in the 64 KiB past a
 * block buffer. `storage_init` mounts a volume labelled myos-root or
 * myos-data read-write without being asked, so plugging it in is the attack.
 *
 * The code under test is the kernel's own: fs/ext2.c, fs/fat32.c,
 * fs/partition.c, fs/fatvfs.c, fs/bcache.c, fs/pcache.c, fs/vfs.c and
 * kernel/blockdev.c, compiled from the same source that boots. Only the
 * allocators, the block device and the clock are stubbed (diskstubs.c), and
 * two of those are made deliberately more sensitive than the real thing so
 * that an overflow is a report rather than a quiet corruption.
 *
 * What the exercise below has to do is reach the code that trusts the disk:
 * mounting reads the superblock and the root inode, loading a directory walks
 * its entries, a symlink's target gets copied out, and creating, renaming and
 * unlinking reach the allocators and the ".." fixup. Four of the seven bugs
 * are only reachable through one of the write paths.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <blockdev.h>
#include <ext2.h>
#include <fat32.h>
#include <partition.h>
#include <vfs.h>

#include "diskstubs.h"

/* Bounds on the walk. A crafted image can describe a tree that is deep,
   wide, or a cycle; none of those are the bug we are looking for, and an
   unbounded walk would just time out. */
#define MAX_DEPTH 6
#define MAX_NODES 400
#define MAX_KIDS  64

static int nodes;

/* Something the kernel reported that it should not have been able to. These
   are findings sanitizers cannot make: the memory involved is validly
   allocated, it was simply never written, so reading it is an information
   leak rather than an out-of-bounds access. */
static void leak(const char *what, uint32_t n)
{
    fprintf(stderr, "DISK LEAK: %s (%u bytes of untouched kernel heap)\n", what, n);
    fflush(stderr);
    abort();
}

/* How many bytes at the end of a buffer are still heap poison, i.e. were
   never written after kmalloc handed them out. */
static uint32_t trailing_poison(const uint8_t *p, uint32_t n)
{
    uint32_t k = 0;
    while (k < n && p[n - 1 - k] == HEAP_POISON)
        k++;
    return k;
}

static void check_link(struct inode *i)
{
    if (!i->size)
        return;
    if (!i->data) {
        /* A link with a length but nothing behind it: the first path that
           walks through it dereferences NULL in ring 0. */
        fprintf(stderr, "DISK BUG: symlink of %u bytes with a NULL target\n", i->size);
        fflush(stderr);
        abort();
    }
    /* A fast symlink holds 60 bytes. If size claims more and the rest was
       never filled in, readlink() hands the caller recycled kernel heap. */
    uint32_t stale = trailing_poison(i->data, i->size);
    if (stale >= 8)
        leak("a symlink target runs past the bytes that were copied into it", stale);
}

static void check_name(struct inode *i)
{
    if (i->name_len > NAME_MAX) {
        fprintf(stderr, "DISK BUG: name of %u bytes (NAME_MAX is %u)\n",
                i->name_len, NAME_MAX);
        fflush(stderr);
        abort();
    }
    if (i->name_len >= 8 &&
        trailing_poison((const uint8_t *)i->name, i->name_len) >= 8)
        leak("a filename runs past the bytes that were copied into it",
             trailing_poison((const uint8_t *)i->name, i->name_len));
}

static void read_file(struct inode *i)
{
    if (!i->ops || !i->ops->read)
        return;
    struct file f;
    memset(&f, 0, sizeof f);
    f.ops = i->ops;
    f.inode = i;
    uint8_t buf[2048];
    /* Bounded: a crafted inode can claim a huge size, and reading all of it
       would only be slow. Two passes is enough to cross a block boundary. */
    for (int pass = 0; pass < 4; pass++) {
        int32_t n = i->ops->read(&f, buf, sizeof buf);
        if (n <= 0)
            break;
    }
}

static void walk(struct inode *dir, int depth)
{
    if (depth > MAX_DEPTH || nodes > MAX_NODES)
        return;
    vfs_dir_load(dir);

    /* Snapshot the children before touching any of them. Reading a file or
       loading a subdirectory can add to or remove from a list, and walking a
       list that something else is editing would be a bug in the harness
       rather than in the kernel. */
    struct inode *kids[MAX_KIDS];
    int n = 0;
    for (struct inode *c = dir->children; c && n < MAX_KIDS; c = c->next_sibling)
        kids[n++] = c;

    for (int k = 0; k < n; k++) {
        struct inode *c = kids[k];
        if (++nodes > MAX_NODES)
            return;
        check_name(c);
        if (S_ISLNK(c->mode))
            check_link(c);
        else if (S_ISREG(c->mode))
            read_file(c);
        else if (S_ISDIR(c->mode))
            walk(c, depth + 1);
    }
}

/* The write paths. Creating reaches the inode and block bitmaps; renaming a
   directory into a different parent reaches the ".." fixup, which is where
   the worst of the seven bugs was; unlinking reaches the bitmap clears and
   the indirect-block walk. */
static void exercise_writes(void)
{
    struct inode *root = vfs_root();
    struct inode *made = NULL;

    if (vfs_create(root, "fz-file", 7, S_IFREG | 0644, 0, &made) == 0 && made) {
        vfs_truncate(made, 4096);
        vfs_unlink(made);
    }

    struct inode *d1 = NULL, *d2 = NULL;
    if (vfs_create(root, "fz-a", 4, S_IFDIR | 0755, 0, &d1) == 0 && d1 &&
        vfs_create(root, "fz-b", 4, S_IFDIR | 0755, 0, &d2) == 0 && d2)
        vfs_rename(d1, d2, "moved", 5, 0);          /* ".." must be rewritten */

    /* And the same on a directory that came off the image rather than one we
       just made, because its "." entry is whatever the image says it is. */
    struct inode *sub = vfs_child(root, "sub", 3);
    if (sub && S_ISDIR(sub->mode) && d2)
        vfs_rename(sub, d2, "alsomoved", 9, 0);

    vfs_symlink(root, "fz-link", 7, "fz-file", 7);
}

/* One image, start to finish. Called once per process. */
void disk_exercise(const uint8_t *image, uint32_t bytes)
{
    struct blockdev *d = disk_setup(image, bytes);
    if (!d)
        return;

    partition_scan(d);                  /* the MBR, such as it is */

    char label[17];
    ext2_label(d, label);               /* what storage_init does first */

    if (ext2_mount(d, "/") == 0) {
        nodes = 0;
        walk(vfs_root(), 0);
        exercise_writes();
        nodes = 0;
        walk(vfs_root(), 0);            /* again: see what the writes did */
    } else if (fat_vfs_mount(d, "/mnt") == 0) {
        struct inode *mnt = NULL;
        if (vfs_lookup("/mnt", &mnt) == 0 && mnt) {
            nodes = 0;
            walk(mnt, 0);
        }
    }
}
