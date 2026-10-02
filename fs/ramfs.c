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
 * free RAM (not limited by the kernel heap), appending is cheap, and
 * deleting a file gives every page back.
 *
 * Speed: one read() or write() copies at most RW_MAX bytes, so a syscall
 * never runs long with interrupts off (programs loop on short counts).
 */
#include <errno.h>
#include <kheap.h>
#include <memlayout.h>
#include <pmm.h>
#include <string.h>
#include <uaccess.h>
#include <vfs.h>

#define RW_MAX (256 * 1024)

/* The archive pointer, or the page holding byte `pos` (NULL: zeros). */
static const uint8_t *page_at(struct inode *i, uint32_t pos)
{
    uint32_t idx = pos / PAGE_SIZE;
    if (!i->pages)
        return i->data ? i->data + idx * PAGE_SIZE : NULL;
    if (idx >= i->page_cap || !i->pages[idx])
        return NULL;
    return P2V(i->pages[idx]);
}

void file_copy_out(struct inode *i, uint32_t pos, void *kbuf, uint32_t n)
{
    uint8_t *dst = kbuf;
    while (n) {
        uint32_t in = pos % PAGE_SIZE, k = PAGE_SIZE - in < n ? PAGE_SIZE - in : n;
        if (!i->pages && i->data) {     /* the archive is contiguous */
            memcpy(dst, i->data + pos, n);
            return;
        }
        const uint8_t *src = page_at(i, pos);
        if (src)
            memcpy(dst, src + in, k);
        else
            memset(dst, 0, k);
        dst += k;
        pos += k;
        n -= k;
    }
}

int32_t file_read_at(struct inode *i, uint32_t pos, void *user_buf, uint32_t n)
{
    if (pos >= i->size)
        return 0;                       /* end of file */
    if (n > i->size - pos)
        n = i->size - pos;
    if (n > RW_MAX)
        n = RW_MAX;
    if (!i->pages && i->data)           /* initramfs: one copy, no cache */
        return copy_to_user(user_buf, i->data + pos, n) < 0 ? -EFAULT : (int32_t)n;

    static const uint8_t zeros[PAGE_SIZE];
    uint32_t done = 0;
    while (done < n) {
        uint32_t p = pos + done, in = p % PAGE_SIZE;
        uint32_t k = PAGE_SIZE - in < n - done ? PAGE_SIZE - in : n - done;
        const uint8_t *src = page_at(i, p);
        if (copy_to_user((uint8_t *)user_buf + done, src ? src + in : zeros, k) < 0)
            return done ? (int32_t)done : -EFAULT;
        done += k;
    }
    return done;
}

/* Make room in pages[] for `count` entries. */
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

/* The first change: copy the archive bytes into pages of our own. */
static int make_writable(struct inode *i)
{
    if (i->pages || (!i->data && i->size == 0)) {
        if (!i->pages && grow_table(i, 1) < 0)
            return -ENOMEM;
        return 0;
    }
    uint32_t n = (i->size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (grow_table(i, n ? n : 1) < 0)
        return -ENOMEM;
    for (uint32_t p = 0; p < n; p++) {
        uint32_t f = pmm_alloc();
        if (!f)
            return -ENOSPC;
        uint32_t k = i->size - p * PAGE_SIZE < PAGE_SIZE ? i->size - p * PAGE_SIZE : PAGE_SIZE;
        memcpy(P2V(f), i->data + p * PAGE_SIZE, k);
        memset((uint8_t *)P2V(f) + k, 0, PAGE_SIZE - k);
        i->pages[p] = f;
    }
    i->data = NULL;                     /* the archive copy is no longer ours */
    return 0;
}

int32_t file_write_at(struct inode *i, uint32_t pos, const void *user_buf, uint32_t n)
{
    if (n > RW_MAX)
        n = RW_MAX;
    if (pos + n < pos)
        return -EFBIG;
    int err = make_writable(i);
    if (err)
        return err;
    if (grow_table(i, (pos + n + PAGE_SIZE - 1) / PAGE_SIZE) < 0)
        return -ENOMEM;

    uint32_t done = 0;
    while (done < n) {
        uint32_t p = pos + done, idx = p / PAGE_SIZE, in = p % PAGE_SIZE;
        uint32_t k = PAGE_SIZE - in < n - done ? PAGE_SIZE - in : n - done;
        if (!i->pages[idx]) {           /* a new page (or a hole being filled) */
            uint32_t f = pmm_alloc();
            if (!f)
                return done ? (int32_t)done : -ENOSPC;
            memset(P2V(f), 0, PAGE_SIZE);
            i->pages[idx] = f;
        }
        if (copy_from_user((uint8_t *)P2V(i->pages[idx]) + in,
                           (const uint8_t *)user_buf + done, k) < 0)
            return done ? (int32_t)done : -EFAULT;
        done += k;
    }
    if (pos + done > i->size)
        i->size = pos + done;
    i->mtime = i->ctime = time_now();
    return done;
}

int file_truncate(struct inode *i, uint32_t size)
{
    int err = make_writable(i);
    if (err)
        return err;
    uint32_t keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t p = keep; p < i->page_cap; p++) {
        if (i->pages[p]) {
            pmm_free(i->pages[p]);      /* shrinking gives pages back */
            i->pages[p] = 0;
        }
    }
    if (size % PAGE_SIZE && keep && keep <= i->page_cap && i->pages[keep - 1]) {
        uint32_t in = size % PAGE_SIZE;  /* zero the cut-off tail of the last page */
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
                pmm_free(i->pages[p]);
        kfree(i->pages);
        i->pages = NULL;
        i->page_cap = 0;
    }
    i->size = 0;
}
