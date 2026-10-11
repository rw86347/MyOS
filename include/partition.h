/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef PARTITION_H
#define PARTITION_H

#include <blockdev.h>

/* Read `disk`'s MBR partition table and register each partition as a new
   block device (e.g. hdb1). Returns the number found, or -1 on read error. */
int partition_scan(struct blockdev *disk);
int partition_rescan(struct blockdev *disk);     /* BLKRRPART: 0 or -errno */

/* The MBR type byte of a partition device (stored in priv). */
static inline uint8_t partition_type(const struct blockdev *part)
{
    return (uint8_t)(uintptr_t)part->priv;
}

#endif
