/*
 * mem_test.c — boot-time self-tests for the page allocator, paging and kmalloc.
 */
#include <kheap.h>
#include <memlayout.h>
#include <mem_test.h>
#include <paging.h>
#include <pmm.h>
#include <selftest.h>
#include <string.h>

static void test_pmm(void)
{
    uint32_t before = pmm_free_count();
    uint32_t a = pmm_alloc(), b = pmm_alloc(), c = pmm_alloc();

    check(a && b && c && a != b && b != c && a != c, "pmm: three distinct frames");
    check(a % PAGE_SIZE == 0 && b % PAGE_SIZE == 0, "pmm: frames are page aligned");
    check(a >= 0x100000, "pmm: never hands out low memory");
    check(pmm_free_count() == before - 3, "pmm: free count drops by 3");

    /* Write through the direct map and read it back. */
    memset(P2V(a), 0xAB, PAGE_SIZE);
    memset(P2V(b), 0xCD, PAGE_SIZE);
    check(((uint8_t *)P2V(a))[PAGE_SIZE - 1] == 0xAB && ((uint8_t *)P2V(b))[0] == 0xCD,
          "pmm: frames hold data independently");

    pmm_free(b);
    check(pmm_alloc() == b, "pmm: a freed frame is reused (lowest first)");
    pmm_free(a); pmm_free(b); pmm_free(c);
    check(pmm_free_count() == before, "pmm: free count restored");
}

#ifndef ARCH_X86_64
static void test_paging(void)
{
    check(vmm_translate(0) == 0, "paging: address 0 (NULL) is not mapped");
    check(vmm_translate(KERNEL_VBASE + 0x100000) == 0x100000,
          "paging: kernel at 0xC0100000 -> physical 0x100000 (4 MiB page)");
    check(vmm_translate(KERNEL_VBASE + 0x1234567) == 0x1234567,
          "paging: direct map past the first 4 MiB");

    /* Map one frame at two virtual addresses: a write through one must be
       visible through the other — proof the CPU really translates. */
    uint32_t frame = pmm_alloc();
    uint32_t va = 0x40000000;           /* an arbitrary unused address */
    check(vmm_map(va, frame, PTE_WRITABLE) == 0, "paging: map a page");
    check(vmm_translate(va + 123) == frame + 123, "paging: translate it");
    *(volatile uint32_t *)va = 0x12345678;
    check(*(volatile uint32_t *)P2V(frame) == 0x12345678,
          "paging: same frame seen through two addresses");
    check(vmm_map(va, frame, PTE_WRITABLE) < 0, "paging: double map refused");
    check(vmm_unmap(va) == frame && vmm_translate(va) == 0, "paging: unmap");
    pmm_free(frame);
}
#endif /* !ARCH_X86_64 */

#define SMALL_SIZE_MAX 1024

static void test_kheap(void)
{
    uint32_t used0, free0, mapped0;
    kheap_stats(&used0, &free0, &mapped0);

    /* Many allocations of different sizes, each filled with its own byte. */
    enum { N = 64 };
    static uint8_t *p[N];
    int ok = 1;
    for (int i = 0; i < N; i++) {
        size_t size = 1 + i * 37;
        p[i] = kmalloc(size);
        if (!p[i] || ((uintptr_t)p[i] % 16) != 0)
            ok = 0;
        else
            memset(p[i], i, size);
    }
    check(ok, "kmalloc: 64 allocations, all 16-byte aligned");

    ok = 1;
    for (int i = 0; i < N; i++)
        for (size_t j = 0; j < 1 + i * 37u; j++)
            if (p[i][j] != (uint8_t)i)
                ok = 0;
    check(ok, "kmalloc: blocks don't overlap");

    /* A freed small object is the very next one handed out for its size
       class (p[1] is 38 bytes: class 64). */
    kfree(p[1]);
    void *again = kmalloc(37);
    check(again == p[1], "kmalloc: reuses a freed block of the same size class");
    p[1] = again;

    /* Free every other block, then all: freed space must merge back. */
    for (int i = 0; i < N; i += 2)
        kfree(p[i]);
    for (int i = 1; i < N; i += 2)
        kfree(p[i]);

    uint32_t used, free_bytes, mapped;
    kheap_stats(&used, &free_bytes, &mapped);
    check(used == used0, "kfree: everything returned");

    /* A big allocation spanning many pages (physically scattered frames,
       virtually contiguous thanks to paging). */
    uint8_t *big = kmalloc(100 * 1024);
    ok = big != NULL;
    for (uint32_t i = 0; ok && i < 100 * 1024; i++)
        big[i] = (uint8_t)(i * 13);
    for (uint32_t i = 0; ok && i < 100 * 1024; i++)
        if (big[i] != (uint8_t)(i * 13))
            ok = 0;
    check(ok, "kmalloc: 100 KiB block works");
    kfree(big);

    /* After freeing everything, the heap is back where it started (blocks
       that existed before — the file tree's inodes, a thread's stack —
       stay): all the space we used is free again, including what the heap
       grew by for the 100 KiB block. */
    kheap_stats(&used, &free_bytes, &mapped);
    check(used == used0 && free_bytes - free0 == mapped - mapped0,
          "kheap: everything freed is free again (merged)");

    /* Small objects come from the direct map, big ones from the heap range. */
    void *s = kmalloc(SMALL_SIZE_MAX), *b = kmalloc(SMALL_SIZE_MAX + 1);
    check((uintptr_t)s < KHEAP_START && (uintptr_t)b >= KHEAP_START,
          "kmalloc: size classes up to 1 KiB, big blocks above");
    kfree(s);
    kfree(b);

    /* A double free is caught: the object is handed out once, not twice
       (the second kfree prints "freed twice (ignored)"). */
    void *x = kmalloc(48);
    kfree(x);
    kfree(x);
    void *y1 = kmalloc(48), *y2 = kmalloc(48);
    check(y1 == x && y2 != x, "kfree: a double free is caught");
    kfree(y1);
    kfree(y2);
}

void mem_selftest(void)
{
    test_pmm();
#ifndef ARCH_X86_64
    test_paging();
#endif
    test_kheap();
}
