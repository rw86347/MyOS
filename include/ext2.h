#ifndef EXT2_H
#define EXT2_H

#include <blockdev.h>

/* Mount the ext2 file system on dev at path (fs/ext2.c). 0, or -EINVAL if
   dev doesn't hold ext2 (or one we can't read), or another -errno. */
int ext2_mount(struct blockdev *dev, const char *path);

#endif
