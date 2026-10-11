/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef EXT2_H
#define EXT2_H

#include <blockdev.h>

/* Is there ext2 on dev? 0 and its volume label (NUL-terminated), or -1. */
int ext2_label(struct blockdev *dev, char label[17]);

/* Mount the ext2 file system on dev at path (fs/ext2.c). 0, or -EINVAL if
   dev doesn't hold ext2 (or one we can't read), or another -errno. */
int ext2_mount(struct blockdev *dev, const char *path);

#endif
