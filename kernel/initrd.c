/*
 * initrd.c — the initramfs ("initial RAM file system")
 *
 * Linux boots with a cpio archive of files in RAM, so the kernel has files
 * (programs, config) before it has any disk driver. We do the same: the
 * build packs initramfs/ into build/initrd.cpio, and stage 2 loads it right
 * after the kernel. Here we just find it and check it's all there; the
 * cpio reader and VFS come next.
 *
 * cpio "newc" format: each file is a 110-byte text header starting with the
 * magic "070701", then the file name, then the data. The archive ends with
 * a file named "TRAILER!!!". So if the first bytes are the magic and the
 * trailer is at the end, the whole archive made it into memory.
 */
#include <bootinfo.h>
#include <initrd.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pmm.h>
#include <selftest.h>
#include <string.h>

static const uint8_t *data;
static uint32_t size;

const uint8_t *initrd_data(void) { return data; }
uint32_t initrd_size(void)       { return size; }

/* Is `needle` somewhere in the last `tail` bytes of the archive? */
static int ends_with_trailer(const char *needle, uint32_t tail)
{
    uint32_t n = strlen(needle);
    uint32_t from = size > tail ? size - tail : 0;
    for (uint32_t i = from; i + n <= size; i++)
        if (memcmp(data + i, needle, n) == 0)
            return 1;
    return 0;
}

void initrd_init(void)
{
    const struct bootinfo *bi = P2V(BOOTINFO_PHYS);
    if (!bi->initrd_size) {
        kprintf("initramfs: none\n");
        return;
    }
    data = P2V(bi->initrd_phys);
    size = bi->initrd_size;
    kprintf("initramfs: %lu KiB at physical %08lx\n", size / 1024, bi->initrd_phys);

    check(size >= 6 && memcmp(data, "070701", 6) == 0, "initramfs: starts with cpio magic");
    check(ends_with_trailer("TRAILER!!!", 1024), "initramfs: ends with cpio trailer");

    /* The page allocator must never hand out the initramfs' memory. It gives
       out the lowest free frame, and the initramfs sits right after the
       kernel — so if it weren't reserved, this frame would be inside it. */
    uint32_t f = pmm_alloc();
    check(f < bi->initrd_phys || f >= bi->initrd_phys + size,
          "initramfs: its memory is reserved");
    pmm_free(f);
}
