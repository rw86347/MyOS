/*
 * fatvfs_test.c — FAT32 through the VFS (make test, step 9e read)
 *
 * The test stick is build/usb.img (tools/mkusb.sh). It is mounted at /mnt.
 * These checks read it the same way a program does: path lookup, then the
 * file bytes. Creating a file must be refused; writing is not in yet.
 */
#include <errno.h>
#include <fat32.h>
#include <kprintf.h>
#include <selftest.h>
#include <string.h>
#include <vfs.h>

void fatvfs_selftest(void)
{
    struct inode *ino;
    char buf[40];
    const char *hello = "Hello from a FAT32 USB stick!\n";

    int err = vfs_lookup("/mnt/HELLO.TXT", &ino);
    check(err == 0 && ino && ino->size == 30, "fat: HELLO.TXT is in the tree");
    if (err)
        return;
    err = file_copy_out(ino, 0, buf, 30);
    check(err == 0 && memcmp(buf, hello, 30) == 0, "fat: HELLO.TXT bytes");
    if (err == 0 && memcmp(buf, hello, 30) == 0)
        kprintf("fatvfs: HELLO.TXT read through the VFS\n");

    err = vfs_lookup("/mnt/docs/This is a long file name.txt", &ino);
    check(err == 0 && ino && ino->size == 52, "fat: long name through the VFS");

    err = vfs_lookup("/mnt/src/kernel/sched.c", &ino);
    check(err == 0 && ino && ino->size == 26, "fat: nested directory");

    err = vfs_lookup("/mnt/data/pattern.bin", &ino);
    uint8_t b0 = 1, b1 = 0;
    if (err == 0) {
        file_copy_out(ino, 0, &b0, 1);
        file_copy_out(ino, 65535, &b1, 1);
    }
    check(err == 0 && ino->size == 65536 && b0 == 0 && b1 == 255,
          "fat: 64 KiB file, first and last byte");

    struct inode *dir;
    err = vfs_lookup("/mnt", &dir);
    struct inode *created = NULL;
    int cerr = err ? err : vfs_create(dir, "new", 3, S_IFREG | 0644, 0, &created);
    check(cerr == -EROFS, "fat: creating a file is refused");
}
