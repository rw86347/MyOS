/*
 * initrd.c — the initramfs ("initial RAM file system")
 *
 * Linux boots with a cpio archive of files in RAM, so the kernel has files
 * (programs, config) before it has any disk driver. We do the same: the
 * build packs initramfs/ into build/initrd.cpio, and stage 2 loads it right
 * after the kernel. Here we just find it; initrd_test.c checks it arrived
 * intact.
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
#include <vfs.h>

static const uint8_t *data;
static uint32_t size;

const uint8_t *initrd_data(void) { return data; }
uint32_t initrd_size(void)       { return size; }

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
    initramfs_load(data, size);     /* its files become the file tree */
    dev_init();                     /* plus /dev/null, /dev/console, ... */
}
