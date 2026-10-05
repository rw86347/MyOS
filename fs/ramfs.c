/*
 * ramfs.c — file contents in RAM: reading, writing, growing, shrinking
 *
 * Every file in MyOS lives in memory. One that came with the initramfs
 * starts out as a pointer into the archive (`data`): reading it copies
 * straight from there, nothing in between. The first time it's changed,
 * its contents move into pages of its own:
 *
 *   pages[0] -> frame with bytes 0-4095
 *   pages[1] -> frame with bytes 4096-8191        (0 = a hole: zeros)
 *   ...
 *
 * Whole pages, straight from the page allocator: a file can grow as big as
 * the limit below (not limited by the kernel heap), appending is cheap, and
 * deleting a file gives every page back.
 *
 * Speed: one read() or write() copies at most RW_MAX bytes, so a syscall
 * never runs long with interrupts off (programs loop on short counts).
 */
#include <errno.h>
#include <pcache.h>
#include <kheap.h>
#include <memlayout.h>
#include <pmm.h>
#include <string.h>
#include <uaccess.h>
#include <vfs.h>

/* Frames holding file contents. At most half of RAM, like Linux's tmpfs:
   a runaway `yes > file` gets "No space left on device" instead of
   starving every program of memory. */
static uint32_t used_pages;

static uint32_t max_pages(void) { return pmm_usable_count() / 2; }

void ramfs_usage(uint32_t *used, uint32_t *max)
{
    *used = used_pages;
    *max = max_pages();
}

static uint32_t page_new(void)          /* not zeroed */
{
    if (used_pages >= max_pages())
        return 0;
    uint32_t f = pmm_alloc();
    if (f)
        used_pages++;
    return f;
}

static void page_drop(uint32_t f)
{
    pmm_free(f);
    used_pages--;
}

static uint32_t min(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* The page holding byte `pos`, or NULL for a hole. */
static const uint8_t *page_at(struct inode *i, uint32_t pos)
{
    uint32_t idx = pos / PAGE_SIZE;
    if (idx >= i->page_cap || !i->pages[idx])
        return NULL;
    return P2V(i->pages[idx]);
}

int file_copy_out(struct inode *i, uint32_t pos, void *kbuf, uint32_t n)
{
    if (i->fs)
        return pcache_read(i, pos, kbuf, n);    /* disk: shared page cache */
    if (!i->pages) {                    /* the archive is contiguous */
        memcpy(kbuf, i->data + pos, n);
        return 0;
    }
    uint8_t *dst = kbuf;
    while (n) {
        uint32_t in = pos % PAGE_SIZE, k = min(PAGE_SIZE - in, n);
        const uint8_t *src = page_at(i, pos);
        if (src)
            memcpy(dst, src + in, k);
        else
            memset(dst, 0, k);
        dst += k;
        pos += k;
        n -= k;
    }
    return 0;
}

/* read() on a regular file: from the file's position, which moves on. */
static int32_t ram_read(struct file *f, void *user_buf, uint32_t n)
{
    struct inode *i = f->inode;
    uint32_t pos = f->pos;
    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    if (pos >= i->size)
        return 0;                       /* end of file */
    n = min(min(n, i->size - pos), RW_MAX);
    if (!i->pages) {                    /* initramfs: one copy, no cache */
        if (copy_to_user(user_buf, i->data + pos, n) < 0)
            return -EFAULT;
        f->pos += n;
        return n;
    }

    static const uint8_t zeros[PAGE_SIZE];
    uint32_t done = 0;
    while (done < n) {
        uint32_t p = pos + done, in = p % PAGE_SIZE, k = min(PAGE_SIZE - in, n - done);
        const uint8_t *src = page_at(i, p);
        if (copy_to_user((uint8_t *)user_buf + done, src ? src + in : zeros, k) < 0) {
            if (!done)
                return -EFAULT;
            break;
        }
        done += k;
    }
    f->pos += done;
    return done;
}

/* Make room in pages[] for `count` entries (doubling, so appending a byte
   at a time doesn't reallocate every page). */
static int grow_table(struct inode *i, uint32_t count)
{
    if (count <= i->page_cap)
        return 0;
    uint32_t cap = i->page_cap ? i->page_cap : 4;
    while (cap < count)
        cap *= 2;
    uint32_t *t = kmalloc(cap * sizeof *t);
    if (!t)
        return -ENOMEM;
    memset(t, 0, cap * sizeof *t);
    if (i->pages) {
        memcpy(t, i->pages, i->page_cap * sizeof *t);
        kfree(i->pages);
    }
    i->pages = t;
    i->page_cap = cap;
    return 0;
}

/* The first change: from now on the contents live in pages of our own.
   Only the first `keep` bytes are copied from the archive (a truncate to
   0 copies nothing). If memory runs out, the file stays as it was. */
static int make_writable(struct inode *i, uint32_t keep)
{
    if (i->pages)
        return 0;
    uint32_t n = keep / PAGE_SIZE + (keep % PAGE_SIZE != 0);
    if (grow_table(i, n ? n : 1) < 0)
        return -ENOMEM;
    for (uint32_t p = 0; p < n; p++) {
        uint32_t f = page_new();
        if (!f) {
            while (p--)
                page_drop(i->pages[p]);
            kfree(i->pages);
            i->pages = NULL;
            i->page_cap = 0;
            return -ENOSPC;
        }
        uint32_t k = min(keep - p * PAGE_SIZE, PAGE_SIZE);
        memcpy(P2V(f), i->data + p * PAGE_SIZE, k);
        memset((uint8_t *)P2V(f) + k, 0, PAGE_SIZE - k);
        i->pages[p] = f;
    }
    i->data = NULL;                     /* the archive copy is no longer ours */
    return 0;
}

/* write() on a regular file: at its position (O_APPEND: at the end). */
static int32_t ram_write(struct file *f, const void *user_buf, uint32_t n)
{
    struct inode *i = f->inode;
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (f->flags & O_APPEND)
        f->pos = i->size;               /* every write goes to the end */
    uint32_t pos = f->pos;
    if (n == 0)
        return 0;
    n = min(n, RW_MAX);
    if (pos + n < pos)
        return -EFBIG;                  /* past 4 GiB */
    int err = make_writable(i, i->size);
    if (err)
        return err;
    if (grow_table(i, (pos + n - 1) / PAGE_SIZE + 1) < 0)
        return -ENOMEM;

    uint32_t done = 0;
    while (done < n) {
        uint32_t p = pos + done, idx = p / PAGE_SIZE, in = p % PAGE_SIZE;
        uint32_t k = min(PAGE_SIZE - in, n - done);
        if (!i->pages[idx]) {           /* a new page (or a hole being filled) */
            uint32_t frame = page_new();
            if (!frame)
                break;
            memset(P2V(frame), 0, PAGE_SIZE);
            i->pages[idx] = frame;
        }
        if (file_copy_in(f, (uint8_t *)P2V(i->pages[idx]) + in,
                         (const uint8_t *)user_buf + done, k) < 0) {
            if (!done)
                return -EFAULT;
            break;
        }
        done += k;
    }
    if (!done)
        return -ENOSPC;
    if (pos + done > i->size)
        i->size = pos + done;
    i->mtime = i->ctime = time_now();
    f->pos += done;
    return done;
}

const struct file_ops ram_ops = { .read = ram_read, .write = ram_write, .kbuf_ok = 1 };

uint32_t ramfs_page(struct inode *i, uint32_t idx)
{
    if (grow_table(i, idx + 1) < 0)
        return 0;
    if (!i->pages[idx]) {
        uint32_t frame = page_new();
        if (!frame)
            return 0;
        memset(P2V(frame), 0, PAGE_SIZE);
        i->pages[idx] = frame;
    }
    return i->pages[idx];
}

int file_truncate(struct inode *i, uint32_t size)
{
    int err = make_writable(i, min(size, i->size));
    if (err)
        return err;
    uint32_t keep = size / PAGE_SIZE + (size % PAGE_SIZE != 0);
    for (uint32_t p = keep; p < i->page_cap; p++) {
        if (i->pages[p]) {
            page_drop(i->pages[p]);     /* shrinking gives pages back */
            i->pages[p] = 0;
        }
    }
    /* Zero the cut-off tail of the last page, so growing the file again
       later shows zeros there, not the old bytes. */
    if (size % PAGE_SIZE && keep <= i->page_cap && i->pages[keep - 1]) {
        uint32_t in = size % PAGE_SIZE;
        memset((uint8_t *)P2V(i->pages[keep - 1]) + in, 0, PAGE_SIZE - in);
    }
    i->size = size;                     /* growing just leaves a hole */
    i->mtime = i->ctime = time_now();
    return 0;
}

void file_free(struct inode *i)
{
    if (i->pages) {
        for (uint32_t p = 0; p < i->page_cap; p++)
            if (i->pages[p])
                page_drop(i->pages[p]);
        kfree(i->pages);
        i->pages = NULL;
        i->page_cap = 0;
    }
    i->size = 0;
}
