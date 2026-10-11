/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * firmware.c — firmware files for drivers (Linux's request_firmware)
 *
 * Many devices run a program of their own that the driver hands them when
 * they start: Intel's Wi-Fi chips get 1-2 MiB of "ucode" every time they
 * power up, and forget it at power-off. Those programs are files in
 * /lib/firmware (Linux's linux-firmware collection); a driver asks for one
 * by name and gets its bytes.
 *
 * Where the bytes come from: a file in the initramfs, unchanged since boot,
 * is handed over where it lies in the archive — no copy (CLAUDE.md, rule 5).
 * One on a disk, or a RAM file that has been written to, is copied into the
 * kernel heap once. Either way the file is held until release_firmware().
 *
 * Linux also looks in /lib/firmware/<kernel version>/ and can decompress
 * .xz/.zst files; we look in /lib/firmware/updates/ then /lib/firmware/,
 * and want plain files (the live disk build unpacks them).
 */
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <linux/firmware.h>
#include <string.h>
#include <vfs.h>

#define FW_PATH_MAX 128
#define FW_SIZE_MAX (16u << 20)     /* the biggest firmware file we take */

static const char *const dirs[] = { "/lib/firmware/updates/", "/lib/firmware/" };

static int load(const struct firmware **out, const char *name, int warn)
{
    *out = NULL;
    size_t nlen = strlen(name);
    if (!nlen || name[0] == '/')
        return -EINVAL;
    struct inode *i = NULL;
    int err = -ENOENT;
    for (uint32_t k = 0; k < sizeof dirs / sizeof dirs[0] && err; k++) {
        char path[FW_PATH_MAX];
        size_t dlen = strlen(dirs[k]);
        if (dlen + nlen + 1 > sizeof path)
            return -ENAMETOOLONG;
        memcpy(path, dirs[k], dlen);
        memcpy(path + dlen, name, nlen + 1);
        err = vfs_lookup(path, &i);
        if (!err && (i->mode & S_IFMT) != S_IFREG)
            err = -ENOENT;
    }
    if (err) {
        if (warn)
            kprintf("firmware: %s: not in /lib/firmware\n", name);
        return err;
    }
    if (i->size > FW_SIZE_MAX)
        return -EFBIG;

    struct firmware *fw = kmalloc(sizeof *fw);
    if (!fw)
        return -ENOMEM;
    vfs_get(i);
    fw->inode = i;
    fw->size = i->size;
    fw->priv = NULL;
    if (!i->fs && !i->pages) {
        fw->data = i->data;             /* still the archive's bytes: lend them */
    } else {
        uint8_t *copy = kmalloc(i->size ? i->size : 1);
        if (!copy || file_copy_out(i, 0, copy, i->size) < 0) {
            if (copy)
                kfree(copy);
            vfs_put(i);
            kfree(fw);
            return copy ? -EIO : -ENOMEM;
        }
        fw->data = copy;
        fw->priv = copy;
    }
    *out = fw;
    return 0;
}

int request_firmware(const struct firmware **fw, const char *name, struct device *dev)
{
    (void)dev;
    return load(fw, name, 1);
}

int firmware_request_nowarn(const struct firmware **fw, const char *name, struct device *dev)
{
    (void)dev;
    return load(fw, name, 0);
}

void release_firmware(const struct firmware *fw)
{
    if (!fw)
        return;
    struct firmware *f = (struct firmware *)fw;
    if (f->priv)
        kfree(f->priv);
    vfs_put(f->inode);
    kfree(f);
}
