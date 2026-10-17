/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * pmm.c — physical memory manager: hands out 4 KiB pages ("frames") of RAM
 *
 * We keep a BITMAP with one bit per physical frame: 1 = used, 0 = free.
 * 768 MiB / 4 KiB = 196608 frames -> 24 KiB of bitmap. Simple and small.
 * Finding a free frame scans from a "lowest free" hint; setting up ranges
 * works a whole bitmap word (32 frames) at a time.
 *
 * Setup, from the BIOS E820 memory map stage 2 saved for us:
 *   1. mark everything USED,
 *   2. mark the "usable RAM" ranges FREE,
 *   3. mark any range that is NOT usable as USED again (E820 entries may
 *      overlap, and when in doubt, don't touch it),
 *   4. reserve the first 1 MiB + the kernel: BIOS data, the boot info,
 *      VGA memory and the BIOS ROM live below 1 MiB, the kernel image
 *      (and its .bss) right above it,
 *   5. reserve the initramfs, which stage 2 loaded after the kernel.
 *
 * Frames are always handed out lowest-address first. paging.c relies on
 * that while it builds the direct map (see vmm_init).
 *
 * Sharing (step 8): after fork, parent and child share their pages until one
 * of them writes ("copy-on-write"). So a frame can have several owners, and
 * may only really be freed when the last one lets go. extra_refs[f] counts
 * the owners BEYOND the first: 0 for a normal frame. pmm_ref() adds an
 * owner; pmm_free() removes one, and frees the frame only when none are
 * left. Counting "extra" owners means every frame starts correct at 0 —
 * even frames allocated before the array existed.
 */
#include <bootinfo.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pmm.h>
#include <string.h>

#define MAX_FRAMES (DIRECTMAP_LIMIT / PAGE_SIZE)

static uint32_t bitmap[MAX_FRAMES / 32];
static uint32_t nframes;            /* frames up to the end of usable RAM */
static uint32_t free_frames;
static uint32_t total_usable;       /* frames the BIOS called usable */
static uint32_t search_from;        /* every frame below this is used */
static uint16_t *extra_refs;        /* one per frame (see above); NULL early on */
static uint32_t refs_phys, refs_pages;

extern char __kernel_end[];         /* from linker.ld (virtual address) */

#ifdef MEM_DEBUG
/* make MEMDEBUG=1 — Solaris's kmem "0xdeadbeef" idea, a page at a time. A
   page that is freed gets filled with 0xDEADBEEF; when it is handed out
   again, every word is checked. A page that was written while it was free
   — through a mapping that should be gone, by a DMA that came late, by a
   pointer kept after the free — is reported: which page, where in it, what
   was written there, and who is taking it now. (The writer itself is long
   gone by then; the bytes usually say who it was.) Debug builds only: it
   touches every page twice. Pages free since boot were never filled, so a
   bit per frame says which ones to check. */
#include <thread.h>
#define DEAD 0xDEADBEEFu
static uint32_t poisoned[MAX_FRAMES / 32];
static uint32_t bad_pages;

static void poison(uint32_t f)
{
    if (!extra_refs)
        return;                         /* early boot: not all RAM is mapped yet */
    uint32_t *w = P2V((phys_addr_t)f * PAGE_SIZE);
    for (uint32_t k = 0; k < PAGE_SIZE / 4; k++)
        w[k] = DEAD;
    poisoned[f / 32] |= 1u << (f % 32);
}

static void check(uint32_t f)
{
    if (!(poisoned[f / 32] & (1u << (f % 32))))
        return;
    poisoned[f / 32] &= ~(1u << (f % 32));
    const uint32_t *w = P2V((phys_addr_t)f * PAGE_SIZE);
    uint32_t first = PAGE_SIZE / 4, n = 0;
    for (uint32_t k = 0; k < PAGE_SIZE / 4; k++)
        if (w[k] != DEAD) {
            if (first == PAGE_SIZE / 4)
                first = k;
            n++;
        }
    if (!n)
        return;
    struct thread *t = thread_current();
    kprintf("MEMDEBUG: page %08lx was written while it was free: %lu words changed, "
            "the first at +%03lx (now taken by \"%s\"):\n", (unsigned long)f * PAGE_SIZE,
            (unsigned long)n, (unsigned long)first * 4, t ? t->name : "?");
    for (uint32_t k = first & ~7u; k < (first & ~7u) + 16 && k < PAGE_SIZE / 4; k += 4)
        kprintf("  +%03lx: %08lx %08lx %08lx %08lx\n", (unsigned long)k * 4,
                (unsigned long)w[k], (unsigned long)w[k + 1], (unsigned long)w[k + 2],
                (unsigned long)w[k + 3]);
    bad_pages++;
}
#endif

static int  is_used(uint32_t f) { return bitmap[f / 32] & (1u << (f % 32)); }

#ifdef PT_AUDIT
/* The page-table audit (make PTAUDIT=1): owners of the frame at phys —
   0 if it is free, -1 if it is not RAM we manage (device memory). (Low
   zone only: high frames count as "not ours" here.) */
int pmm_owners(phys_addr_t phys)
{
    if (phys >= DIRECTMAP_LIMIT)
        return -1;
    uint32_t f = (uint32_t)(phys / PAGE_SIZE);
    if (f >= nframes)
        return -1;
    if (!is_used(f))
        return 0;
    return 1 + (extra_refs ? extra_refs[f] : 0);
}
#endif
static void set_used(uint32_t f) { bitmap[f / 32] |= 1u << (f % 32); }
static void set_free(uint32_t f) { bitmap[f / 32] &= ~(1u << (f % 32)); }

/* Mark one frame free or used, keeping free_frames right. */
static void mark_one(uint32_t f, int make_free)
{
    if (make_free && is_used(f)) {
        set_free(f);
        free_frames++;
    } else if (!make_free && !is_used(f)) {
        set_used(f);
        free_frames--;
    }
}

/* Count the 1 bits (used frames) in a bitmap word. All-used and all-free
   words are by far the most common, so they're answered at once. */
static uint32_t bits_set(uint32_t w)
{
    if (w == ~0u)
        return 32;
    uint32_t n = 0;
    for (; w; w &= w - 1)           /* clears the lowest 1 bit each round */
        n++;
    return n;
}

/* Mark the frames covering [base, base+len) free or used. Freeing rounds
   the range inward (only whole frames inside it); reserving rounds outward.

   Speed: boot marks all of RAM this way, so with 512 MiB that's 131072
   frames. One bitmap word holds 32 frames, so in the middle of a range we
   set a whole word at a time — 32x fewer steps. Only the ragged ends go
   frame by frame. */
static void mark_range(uint64_t base, uint64_t len, int make_free)
{
    uint64_t end = base + len;
    if (make_free) {        /* only whole pages that are entirely usable */
        base = (base + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        end &= ~(uint64_t)(PAGE_SIZE - 1);
    } else {                /* any page that touches a reserved byte */
        base &= ~(uint64_t)(PAGE_SIZE - 1);
        end = (end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    }
    if (end > DIRECTMAP_LIMIT)
        end = DIRECTMAP_LIMIT;      /* ignore RAM above 768 MiB for now */
    if (base >= end)
        return;

    uint32_t f = (uint32_t)(base / PAGE_SIZE), last = (uint32_t)(end / PAGE_SIZE);
    while (f < last && f % 32)              /* up to a word boundary */
        mark_one(f++, make_free);
    for (; f + 32 <= last; f += 32) {       /* whole words: 32 frames at once */
        uint32_t used = bits_set(bitmap[f / 32]);
        if (make_free) {
            free_frames += used;
            bitmap[f / 32] = 0;
        } else {
            free_frames -= 32 - used;
            bitmap[f / 32] = ~0u;
        }
    }
    while (f < last)                        /* the rest */
        mark_one(f++, make_free);
}

static const char *e820_type_name(uint32_t type)
{
    switch (type) {
    case E820_USABLE:       return "usable";
    case E820_RESERVED:     return "reserved";
    case E820_ACPI_RECLAIM: return "ACPI data";
    case E820_ACPI_NVS:     return "ACPI NVS";
    case E820_BAD:          return "bad RAM";
    default:                return "unknown";
    }
}

/* --- the high zone (x86_64): RAM above DIRECTMAP_LIMIT ------------------------
 *
 * A second bitmap and set of share counts, for frames from HIGH_BASE up.
 * pmm_init only notes where the high RAM is (it runs before paging maps
 * it); vmm_init direct-maps those ranges, and pmm_refs_init (after it)
 * makes the bitmap and counts, in low RAM, and frees the ranges. Same
 * rules as the low zone: lowest first, a hint below which all is used.
 */
#ifdef HIGHMEM
#define HIGH_BASE DIRECTMAP_LIMIT
static struct { phys_addr_t start, end; } high_ranges[E820_MAX];
static int nhigh_ranges;
static uint32_t *high_map;              /* 1 bit per frame: 1 = used */
static uint16_t *high_refs;             /* owners beyond the first */
static uint32_t high_frames, high_free, high_usable, high_hint;

/* The part of a usable range above the low zone, whole pages only. */
static void note_high(uint64_t base, uint64_t end)
{
    if (end <= HIGH_BASE || nhigh_ranges == E820_MAX)
        return;
    if (base < HIGH_BASE)
        base = HIGH_BASE;
    base = (base + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    end &= ~(uint64_t)(PAGE_SIZE - 1);
    if (base < end) {
        high_ranges[nhigh_ranges].start = base;
        high_ranges[nhigh_ranges++].end = end;
    }
}

int pmm_high_range(int i, phys_addr_t *start, phys_addr_t *end)
{
    if (i < 0 || i >= nhigh_ranges)
        return 0;
    *start = high_ranges[i].start;
    *end = high_ranges[i].end;
    return 1;
}

static int high_used(uint32_t f) { return high_map[f / 32] & (1u << (f % 32)); }

static void high_init(void)
{
    phys_addr_t top = HIGH_BASE;
    for (int i = 0; i < nhigh_ranges; i++)
        if (high_ranges[i].end > top)
            top = high_ranges[i].end;
    if (top == HIGH_BASE)
        return;                         /* no RAM above the low zone */
    uint64_t n = (top - HIGH_BASE) / PAGE_SIZE;
    if (n > 0xFFFFFFE0u)
        n = 0xFFFFFFE0u;                /* (16 TiB: more than a frame number holds) */
    uint32_t map_pages = (uint32_t)((n / 8 + 4 + PAGE_SIZE - 1) / PAGE_SIZE);
    uint32_t refs_pages_hi = (uint32_t)((n * sizeof(uint16_t) + PAGE_SIZE - 1) / PAGE_SIZE);
    uint32_t mp = pmm_alloc_run(map_pages), rp = pmm_alloc_run(refs_pages_hi);
    if (!mp || !rp) {
        kprintf("pmm: no room to manage the %lu MiB above %lu MiB: not used\n",
                (unsigned long)(n / 256), (unsigned long)(HIGH_BASE >> 20));
        if (mp)
            for (uint32_t k = 0; k < map_pages; k++)
                pmm_free(mp + k * PAGE_SIZE);
        if (rp)
            for (uint32_t k = 0; k < refs_pages_hi; k++)
                pmm_free(rp + k * PAGE_SIZE);
        return;
    }
    high_map = P2V(mp);
    high_refs = P2V(rp);
    memset(high_map, 0xFF, (size_t)map_pages * PAGE_SIZE);     /* all used ... */
    memset(high_refs, 0, (size_t)refs_pages_hi * PAGE_SIZE);
    high_frames = (uint32_t)n;
    for (int i = 0; i < nhigh_ranges; i++)                       /* ... but RAM */
        for (phys_addr_t pa = high_ranges[i].start; pa < high_ranges[i].end; pa += PAGE_SIZE) {
            uint32_t f = (uint32_t)((pa - HIGH_BASE) / PAGE_SIZE);
            if (f < high_frames && high_used(f)) {
                high_map[f / 32] &= ~(1u << (f % 32));
                high_free++;
            }
        }
    high_usable = high_free;
    klog("pmm: %lu MiB above %lu MiB, for programs' memory\n",
         (unsigned long)(high_usable / 256), (unsigned long)(HIGH_BASE >> 20));
}

/* The lowest free high frame, 32 at a time. */
static phys_addr_t high_take(void)
{
    for (uint32_t w = high_hint / 32; w * 32 < high_frames; w++) {
        if (high_map[w] == 0xFFFFFFFFu)
            continue;
        uint32_t f = w * 32 + __builtin_ctz(~high_map[w]);
        if (f >= high_frames)
            break;
        high_map[f / 32] |= 1u << (f % 32);
        high_free--;
        high_hint = f + 1;
        return HIGH_BASE + (phys_addr_t)f * PAGE_SIZE;
    }
    high_hint = high_frames;
    return 0;
}

/* A high frame's number, or -1 (not high: the low zone's). */
static int64_t high_frame(phys_addr_t phys)
{
    if (!high_map || phys < HIGH_BASE)
        return -1;
    return (int64_t)((phys - HIGH_BASE) / PAGE_SIZE);
}
#else
static void note_high(uint64_t base, uint64_t end) { (void)base; (void)end; }
static void high_init(void) { }
static int64_t high_frame(phys_addr_t phys) { (void)phys; return -1; }
int pmm_high_range(int i, phys_addr_t *start, phys_addr_t *end)
{
    (void)i, (void)start, (void)end;
    return 0;
}
#endif

void pmm_init(void)
{
    const struct bootinfo *bi = P2V(BOOTINFO_PHYS);
    uint32_t count = bi->count > E820_MAX ? E820_MAX : bi->count;

    memset(bitmap, 0xFF, sizeof bitmap);    /* 1. everything used */
    free_frames = 0;

    klog("E820 memory map (%lu entries):\n", count);
    for (uint32_t i = 0; i < count; i++) {
        const struct e820_entry *e = &bi->entries[i];
        klog("  %08lx%08lx - %08lx%08lx  %s\n",
             (uint32_t)(e->base >> 32), (uint32_t)e->base,
             (uint32_t)((e->base + e->length - 1) >> 32), (uint32_t)(e->base + e->length - 1),
             e820_type_name(e->type));

        if (e->type == E820_USABLE && e->length) {     /* 2. usable -> free */
            mark_range(e->base, e->length, 1);
            note_high(e->base, e->base + e->length);
            uint64_t end = e->base + e->length;
            if (end > DIRECTMAP_LIMIT)
                end = DIRECTMAP_LIMIT;
            if (end / PAGE_SIZE > nframes)
                nframes = (uint32_t)(end / PAGE_SIZE);
        }
    }
    total_usable = free_frames;

    for (uint32_t i = 0; i < count; i++) {              /* 3. re-reserve */
        const struct e820_entry *e = &bi->entries[i];
        if (e->type != E820_USABLE && e->length)
            mark_range(e->base, e->length, 0);
    }

    /* 4. Low memory + the kernel image (and its .bss). */
    mark_range(0, V2P(__kernel_end), 0);

    /* 5. The initramfs: it stays in memory as the root file system. */
    if (bi->initrd_size)
        mark_range(bi->initrd_phys, bi->initrd_size, 0);
    search_from = 0;

    /* 6. Room for the share counts: 2 bytes per frame, in one contiguous
       run (at boot the free RAM above the kernel is one big run). It is
       only zeroed in pmm_refs_init(), once paging maps all of RAM. */
    refs_pages = (nframes * sizeof(uint16_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t f = 0, run = 0; f < nframes; f++) {
        run = is_used(f) ? 0 : run + 1;
        if (run == refs_pages) {
            uint32_t first = f + 1 - refs_pages;
            mark_range((uint64_t)first * PAGE_SIZE, refs_pages * PAGE_SIZE, 0);
            refs_phys = first * PAGE_SIZE;
            break;
        }
    }
}

void pmm_refs_init(void)
{
    if (!refs_phys) {
        kprintf("pmm: no room for share counts\n");
        return;
    }
    extra_refs = P2V(refs_phys);
    memset(extra_refs, 0, refs_pages * PAGE_SIZE);
    high_init();                        /* vmm_init has mapped the high RAM */
}

void pmm_ref(phys_addr_t phys)
{
    int64_t h = high_frame(phys);
#ifdef HIGHMEM
    if (h >= 0) {
        high_refs[h]++;
        return;
    }
#endif
    (void)h;
    extra_refs[phys / PAGE_SIZE]++;
}

int pmm_ref_if_room(phys_addr_t phys)
{
    int64_t h = high_frame(phys);
#ifdef HIGHMEM
    if (h >= 0) {
        if (high_refs[h] >= 0xFFF0)
            return 0;
        high_refs[h]++;
        return 1;
    }
#endif
    (void)h;
    if (!extra_refs || extra_refs[phys / PAGE_SIZE] >= 0xFFF0)
        return 0;
    extra_refs[phys / PAGE_SIZE]++;
    return 1;
}

int pmm_shared(phys_addr_t phys)
{
    int64_t h = high_frame(phys);
#ifdef HIGHMEM
    if (h >= 0)
        return high_refs[h] != 0;
#endif
    (void)h;
    return extra_refs && extra_refs[phys / PAGE_SIZE] != 0;
}

/* A program's private page: high RAM first, where nothing else may go. */
phys_addr_t pmm_alloc_user(void)
{
#ifdef HIGHMEM
    if (high_free) {
        phys_addr_t pa = high_take();
        if (pa)
            return pa;
    }
#endif
    return pmm_alloc();
}

static uint32_t (*reclaim)(uint32_t want);

void pmm_set_reclaim(uint32_t (*fn)(uint32_t want))
{
    reclaim = fn;
}

/* The lowest free frame. Every frame below search_from is used, so the
   search starts there and tests 32 frames per step (a whole bitmap word). */
static uint32_t take(void)
{
    for (uint32_t w = search_from / 32; w * 32 < nframes; w++) {
        if (bitmap[w] == 0xFFFFFFFFu)
            continue;
        uint32_t f = w * 32 + __builtin_ctz(~bitmap[w]);
        if (f >= nframes)
            break;
        set_used(f);
        free_frames--;
        search_from = f + 1;
#ifdef MEM_DEBUG
        check(f);
#endif
        return f * PAGE_SIZE;
    }
    search_from = nframes;
    return 0;
}

/* Out of frames: the page cache gives clean pages back (it may use all
   free RAM — idle RAM is wasted RAM — but programs come first). */
uint32_t pmm_alloc(void)
{
    uint32_t pa = take();
    if (!pa && reclaim && reclaim(32))
        pa = take();
    return pa;          /* 0: out of memory (frame 0 is never free) */
}

uint32_t pmm_alloc_run(uint32_t pages)
{
    uint32_t run = 0;
    if (!pages)
        return 0;
    for (uint32_t f = 0; f < nframes; f++) {
        run = is_used(f) ? 0 : run + 1;
        if (run == pages) {
            uint32_t first = f + 1 - pages;
            mark_range((uint64_t)first * PAGE_SIZE, (uint64_t)pages * PAGE_SIZE, 0);
#ifdef MEM_DEBUG
            for (uint32_t k = first; k <= f; k++)
                check(k);
#endif
            return first * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free(phys_addr_t phys)
{
    int64_t h = high_frame(phys);
#ifdef HIGHMEM
    if (h >= 0) {
        uint32_t hf = (uint32_t)h;
        if (phys % PAGE_SIZE || h >= high_frames || !high_used(hf)) {
            kprintf("pmm_free: bad or double free of %08lx%08lx\n",
                    (unsigned long)(phys >> 32), (unsigned long)(uint32_t)phys);
            return;
        }
        if (high_refs[hf]) {
            high_refs[hf]--;
            return;
        }
        high_map[hf / 32] &= ~(1u << (hf % 32));
        high_free++;
        if (hf < high_hint)
            high_hint = hf;
        return;
    }
#endif
    (void)h;
    uint32_t f = (uint32_t)(phys / PAGE_SIZE);
    if (phys % PAGE_SIZE || f >= nframes || !is_used(f)) {
        kprintf("pmm_free: bad or double free of %08lx%08lx\n",
                (unsigned long)(phys >> 32), (unsigned long)(uint32_t)phys);
        return;
    }
    if (extra_refs && extra_refs[f]) {
        extra_refs[f]--;            /* someone else still uses it */
        return;
    }
#ifdef MEM_DEBUG
    poison(f);
#endif
    set_free(f);
    free_frames++;
    if (f < search_from)
        search_from = f;    /* keep "lowest first" */
}

#ifdef HIGHMEM
uint32_t pmm_free_count(void)    { return free_frames + high_free; }
uint32_t pmm_usable_count(void)  { return total_usable + high_usable; }
#else
uint32_t pmm_free_count(void)    { return free_frames; }
uint32_t pmm_usable_count(void)  { return total_usable; }
#endif
uint32_t pmm_end(void)           { return nframes * PAGE_SIZE; }
