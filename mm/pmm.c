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
   0 if it is free, -1 if it is not RAM we manage (device memory). */
int pmm_owners(uint32_t phys)
{
    uint32_t f = phys / PAGE_SIZE;
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
}

void pmm_ref(uint32_t phys)
{
    extra_refs[phys / PAGE_SIZE]++;
}

int pmm_shared(uint32_t phys)
{
    return extra_refs && extra_refs[phys / PAGE_SIZE] != 0;
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

void pmm_free(uint32_t phys)
{
    uint32_t f = phys / PAGE_SIZE;
    if (phys % PAGE_SIZE || f >= nframes || !is_used(f)) {
        kprintf("pmm_free: bad or double free of %08lx\n", phys);
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

uint32_t pmm_free_count(void)    { return free_frames; }
uint32_t pmm_usable_count(void)  { return total_usable; }
uint32_t pmm_end(void)           { return nframes * PAGE_SIZE; }
