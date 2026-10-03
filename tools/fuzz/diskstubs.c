/* diskstubs.c — host stand-ins so the kernel's real filesystem code can be
 * run against a crafted disk image under AddressSanitizer.
 *
 * What is real here: fs/ext2.c, fs/fat32.c, fs/partition.c, fs/fatvfs.c,
 * fs/bcache.c, fs/pcache.c, fs/vfs.c, fs/ramfs.c and kernel/blockdev.c, all
 * compiled from the same source the kernel boots. What is stubbed is only the
 * machinery underneath: the page and heap allocators, the block device, the
 * scheduler's wait queues, the clock, and kprintf.
 *
 * Two of those stubs are deliberately *not* faithful, because being faithful
 * would hide bugs:
 *
 *   - kmalloc is malloc, not the kernel's size-class allocator. A size class
 *     rounds 100 bytes up to 128 and a 20-byte overflow lands in slack the
 *     allocator owns; malloc gives AddressSanitizer the exact length to guard.
 *   - pmm_alloc hands out pages from one arena with a poisoned redzone
 *     between them, rather than from a contiguous frame map. In the kernel a
 *     write just past a block buffer lands in the next frame and nothing
 *     notices. Here it is an immediate report with a stack trace.
 *
 * Both choices make this harness strictly more sensitive than the real
 * kernel, which is the point of a fuzz target.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bcache.h>
#include <blockdev.h>
#include <memlayout.h>
#include <pcache.h>
#include <pmm.h>
#include <vfs.h>

#include "diskstubs.h"

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    include <sanitizer/asan_interface.h>
#    define HAVE_ASAN 1
#  endif
#endif
#ifndef HAVE_ASAN
#  define __asan_poison_memory_region(a, b)   ((void)(a), (void)(b))
#  define __asan_unpoison_memory_region(a, b) ((void)(a), (void)(b))
#endif

/* --- the page arena ------------------------------------------------------- */

/* Pages are PAGE_SIZE apart plus a redzone, so a page's neighbours are never
   adjacent to it. STRIDE must keep every page PAGE_SIZE-aligned relative to
   the arena base, because the filesystem code rounds addresses down to pages. */
#define REDZONE   PAGE_SIZE
#define STRIDE    (PAGE_SIZE + REDZONE)
#define ARENA_PGS 4096                  /* 16 MiB of pages, 32 MiB with gaps */

uint8_t *fuzz_arena;
static uint32_t arena_next;             /* offset of the next free page */
static uint8_t *arena_raw;

static void arena_reset(void)
{
    if (!arena_raw) {
        /* Over-allocate by STRIDE so the base can be aligned up. */
        arena_raw = malloc((size_t)ARENA_PGS * STRIDE + STRIDE);
        if (!arena_raw) {
            fprintf(stderr, "diskstubs: out of memory for the arena\n");
            exit(2);
        }
    }
    uintptr_t base = ((uintptr_t)arena_raw + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    fuzz_arena = (uint8_t *)base;
    __asan_unpoison_memory_region(fuzz_arena, (size_t)ARENA_PGS * STRIDE);
    memset(fuzz_arena, 0, (size_t)ARENA_PGS * STRIDE);
    /* Offset 0 stays unused: pmm_alloc returns 0 for failure. */
    arena_next = STRIDE;
}

uint32_t pmm_alloc(void)
{
    if (arena_next + STRIDE > (uint32_t)ARENA_PGS * STRIDE)
        return 0;                       /* out of memory, as the kernel would */
    uint32_t off = arena_next;
    arena_next += STRIDE;
    /* The page itself is usable; the gap after it is not. */
    __asan_unpoison_memory_region(fuzz_arena + off, PAGE_SIZE);
    __asan_poison_memory_region(fuzz_arena + off + PAGE_SIZE, REDZONE);
    memset(fuzz_arena + off, 0, PAGE_SIZE);
    return off;
}

void pmm_free(uint32_t pa)
{
    /* Not recycled: a freed page stays poisoned, so a use-after-free of a
       block buffer is reported rather than silently working. */
    if (pa)
        __asan_poison_memory_region(fuzz_arena + pa, PAGE_SIZE);
}

uint32_t pmm_usable_count(void) { return ARENA_PGS; }
void     pmm_set_reclaim(uint32_t (*fn)(uint32_t want)) { (void)fn; }
/* File locking is a VFS feature with no bearing on parsing a disk image. */
void     flock_release(struct file *f) { (void)f; }
/* A named pipe is reached through vfs_open, which this target links but never
   calls that way: it parses images, and no image contains a FIFO we open. */
int      fifo_open(struct inode *i, uint32_t flags, struct file **out)
{
    (void)i; (void)flags; (void)out;
    return -22;                         /* -EINVAL, without pulling in errno.h */
}

/* --- the kernel heap ----------------------------------------------------- */

void *kmalloc(uint32_t n)
{
    void *p = malloc(n ? n : 1);
    if (p)
        memset(p, HEAP_POISON, n);      /* never hand out zeroed memory */
    return p;
}

void kfree(void *p) { free(p); }

uint32_t poison_run(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    uint32_t i = 0;
    while (i < n && b[i] == HEAP_POISON)
        i++;
    return i;
}

/* --- the block device ---------------------------------------------------- */

static uint8_t *img;
static uint32_t img_sectors;
static struct blockdev *bdev;

uint32_t disk_sectors(void) { return img_sectors; }

static int img_read(struct blockdev *d, uint32_t lba, uint32_t count, void *buf)
{
    (void)d;
    if (lba > img_sectors || count > img_sectors - lba)
        return -1;                      /* the real drivers reject this too */
    memcpy(buf, img + (size_t)lba * 512, (size_t)count * 512);
    return 0;
}

static int img_write(struct blockdev *d, uint32_t lba, uint32_t count, const void *buf)
{
    (void)d;
    if (lba > img_sectors || count > img_sectors - lba)
        return -1;
    memcpy(img + (size_t)lba * 512, buf, (size_t)count * 512);
    return 0;
}

/* --- the scheduler, the clock, the console ------------------------------- */

/* Nothing here switches threads, so a wait is a bug: the only reason the
   filesystem code waits is for I/O it has submitted, and this block device
   completes every request before returning. If one of these is ever reached
   it means a loop is waiting for something that will never happen. */
void wq_sleep(struct wait_queue *q)
{
    (void)q;
    fprintf(stderr, "diskstubs: slept on a wait queue with no I/O outstanding\n");
    abort();
}

int wq_sleep_until(struct wait_queue *q, uint32_t deadline)
{
    (void)q; (void)deadline;
    fprintf(stderr, "diskstubs: slept on a wait queue with no I/O outstanding\n");
    abort();
}

void wq_wake_all(struct wait_queue *q) { (void)q; }
void thread_yield(void) { }
struct process *proc_current(void) { return NULL; }

static uint32_t clock_ms;
uint32_t timer_ms(void) { return ++clock_ms; }
uint32_t time_now(void) { return 1767225600; }
uint32_t vmm_translate(uint32_t va) { return va; }

struct file console_file;
void dev_attach(struct inode *i) { (void)i; }

/* kprintf is where the filesystem code says what it found. It is silenced by
   default because a fuzz run makes millions of them; DISKFUZZ_VERBOSE=1 turns
   it back on, which is what you want when reading a crash. */
void kprintf(const char *fmt, ...)
{
    static int verbose = -1;
    if (verbose < 0)
        verbose = getenv("DISKFUZZ_VERBOSE") != NULL;
    if (!verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* --- coverage, in memory the parent shares --------------------------------
 *
 * Each image runs in its own fork, so counters in the child die with it. The
 * parent passes in a shared mapping instead, and the child marks the edges it
 * reached; the parent then compares it against its own copy to see whether
 * this image found anything the corpus did not already cover. That is what
 * makes the loop coverage-guided across processes.
 */
static uint8_t  *cov_map;
static uint32_t  cov_size;

void cov_use(uint8_t *map, uint32_t n)
{
    cov_map = map;
    cov_size = n;
}

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop)
{
    uint32_t id = 0;
    for (uint32_t *p = start; p < stop; p++)
        *p = ++id;
}

void __sanitizer_cov_trace_pc_guard(uint32_t *guard)
{
    uint32_t id = *guard;
    if (cov_map && id && id < cov_size)
        cov_map[id] = 1;
}

/* --- setup -------------------------------------------------------------- */

/* One call per process. The caller runs each case in a fresh child (see
   disktarget.c), which is why there is nothing here that tries to undo a
   previous mount: bcache, pcache and the VFS all keep static state, and
   unwinding it would mean adding reset functions to files the kernel boots.
   Forking is slower than resetting, and it buys two things worth more than
   the speed: a case cannot inherit anything from the one before it, so a
   saved crash always reproduces, and an AddressSanitizer abort becomes an
   exit status the parent can report rather than the end of the run. */
struct blockdev *disk_setup(const uint8_t *image, uint32_t bytes)
{
    uint32_t sectors = bytes / 512;
    if (!sectors)
        sectors = 1;
    if (sectors > 8192)                 /* 4 MiB is plenty for a filesystem */
        sectors = 8192;

    arena_reset();

    img = malloc((size_t)sectors * 512);
    if (!img) {
        fprintf(stderr, "diskstubs: out of memory for the image\n");
        exit(2);
    }
    memset(img, 0, (size_t)sectors * 512);
    uint32_t have = bytes < sectors * 512u ? bytes : sectors * 512u;
    memcpy(img, image, have);
    img_sectors = sectors;

    static struct blockdev proto;
    memset(&proto, 0, sizeof proto);
    memcpy(proto.name, "fz0", 4);
    proto.sectors = img_sectors;
    proto.read = img_read;
    proto.write = img_write;
    bdev = blockdev_register(&proto);

    bcache_init();
    pcache_init();
    return bdev;
}
