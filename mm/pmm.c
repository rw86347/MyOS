/*
 * pmm.c — physical memory manager: hands out 4 KiB pages ("frames") of RAM
 *
 * We keep a BITMAP with one bit per physical frame: 1 = used, 0 = free.
 * 768 MiB / 4 KiB = 196608 frames -> 24 KiB of bitmap. Simple and small;
 * finding a free frame is a linear scan, which is fine for now.
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

extern char __kernel_end[];         /* from linker.ld (virtual address) */

static int  is_used(uint32_t f) { return bitmap[f / 32] & (1u << (f % 32)); }
static void set_used(uint32_t f) { bitmap[f / 32] |= 1u << (f % 32); }
static void set_free(uint32_t f) { bitmap[f / 32] &= ~(1u << (f % 32)); }

/* Mark the frames covering [base, base+len) free or used. Freeing rounds
   the range inward (only whole frames inside it); reserving rounds outward. */
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

    for (uint64_t a = base; a < end; a += PAGE_SIZE) {
        uint32_t f = (uint32_t)(a / PAGE_SIZE);
        if (make_free && is_used(f)) {
            set_free(f);
            free_frames++;
        } else if (!make_free && !is_used(f)) {
            set_used(f);
            free_frames--;
        }
    }
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
}

uint32_t pmm_alloc(void)
{
    for (uint32_t f = search_from; f < nframes; f++) {
        if (!is_used(f)) {
            set_used(f);
            free_frames--;
            search_from = f + 1;
            return f * PAGE_SIZE;
        }
    }
    return 0;           /* out of memory (frame 0 is never free, so 0 = failure) */
}

void pmm_free(uint32_t phys)
{
    uint32_t f = phys / PAGE_SIZE;
    if (phys % PAGE_SIZE || f >= nframes || !is_used(f)) {
        kprintf("pmm_free: bad or double free of %08lx\n", phys);
        return;
    }
    set_free(f);
    free_frames++;
    if (f < search_from)
        search_from = f;    /* keep "lowest first" */
}

uint32_t pmm_free_count(void)    { return free_frames; }
uint32_t pmm_usable_count(void)  { return total_usable; }
uint32_t pmm_end(void)           { return nframes * PAGE_SIZE; }
