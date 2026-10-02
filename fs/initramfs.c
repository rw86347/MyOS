/*
 * initramfs.c — unpack the cpio archive into the VFS tree
 *
 * cpio "newc" format (the one Linux uses for its initramfs): one record per
 * file, one after another:
 *
 *   110-byte header, all text: "070701" then 13 fields of 8 hex digits
 *     (inode, mode, uid, gid, nlink, mtime, filesize, dev..., namesize, check)
 *   the file name, NUL-terminated (e.g. "./data/numbers.txt"), padded so
 *     the data starts on a 4-byte boundary
 *   the file's contents, padded to 4 bytes
 *
 * The last record is named "TRAILER!!!".
 *
 * Nothing is copied: each inode's name and data point straight into the
 * archive, which stays in RAM (pmm.c reserved it). Unpacking is one pass.
 * The archive is untrusted input, so every size is checked against its end.
 */
#include <errno.h>
#include <kprintf.h>
#include <string.h>
#include <vfs.h>

#define HDR_SIZE 110

/* 8 hex digits -> number. */
static uint32_t hex8(const uint8_t *s)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = s[i];
        v = v * 16 + (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
    }
    return v;
}

static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }

/* Create (or update) the entry for `path`, making missing directories. */
static struct inode *add_path(const char *path, uint32_t len, uint32_t mode)
{
    struct inode *dir = vfs_root();
    uint32_t pos = 0;
    while (pos < len && path[pos] == '/')
        pos++;
    if (len - pos == 1 && path[pos] == '.')
        return dir;                 /* "." itself: the root */
    if (len - pos >= 2 && path[pos] == '.' && path[pos + 1] == '/')
        pos += 2;                   /* "./name" -> "name" */

    for (;;) {
        const char *name = path + pos;
        uint32_t n = 0;
        while (pos + n < len && path[pos + n] != '/')
            n++;
        int last = pos + n >= len;
        struct inode *i = vfs_child(dir, name, n);
        if (!i)
            i = vfs_add(dir, name, n, last ? mode : (S_IFDIR | 0755));
        if (!i)
            return NULL;
        if (last)
            return i;
        dir = i;
        pos += n + 1;
    }
}

void initramfs_load(const uint8_t *a, uint32_t size)
{
    uint32_t off = 0, files = 0;

    while (off + HDR_SIZE <= size) {
        const uint8_t *h = a + off;
        if (memcmp(h, "070701", 6) != 0) {
            kprintf("initramfs: bad header at offset %lu\n", off);
            return;
        }
        uint32_t mode = hex8(h + 14), uid = hex8(h + 22), gid = hex8(h + 30);
        uint32_t mtime = hex8(h + 46);
        uint32_t filesize = hex8(h + 54), namesize = hex8(h + 94);
        uint32_t rdev = (hex8(h + 78) << 8) | hex8(h + 86);    /* devices */

        /* Name and data must lie inside the archive (watch for overflow). */
        uint32_t name_off = off + HDR_SIZE;
        if (namesize == 0 || namesize > size - name_off ||
            a[name_off + namesize - 1] != '\0') {
            kprintf("initramfs: bad name at offset %lu\n", off);
            return;
        }
        uint32_t data_off = align4(name_off + namesize);
        if (data_off > size || filesize > size - data_off) {
            kprintf("initramfs: file runs past the end at offset %lu\n", off);
            return;
        }

        const char *name = (const char *)a + name_off;
        uint32_t len = namesize - 1;
        if (len == 10 && memcmp(name, "TRAILER!!!", 10) == 0)
            break;

        struct inode *i = add_path(name, len, mode);
        if (i) {
            i->mode = mode;         /* an explicit entry wins over a guess */
            i->uid = uid;
            i->gid = gid;
            i->mtime = mtime;
            if (!S_ISDIR(mode)) {
                i->size = filesize;     /* (nlink stays 1: no hard links) */
                i->data = a + data_off;     /* zero-copy: points into the archive
                                               (a symlink's data is its target) */
            }
            if (S_ISCHR(mode)) {
                i->rdev = rdev;
                dev_attach(i);              /* /dev/null & co. */
            }
            files++;
        }
        off = align4(data_off + filesize);
    }
    kprintf("initramfs: %lu files and directories\n", files);
}
