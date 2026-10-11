/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef FAT32_H
#define FAT32_H

#include <blockdev.h>
#include <stdint.h>

/* Directory-entry attribute bits */
#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F   /* all four low bits set = long-name entry */

/* One mounted FAT32 file system. */
struct fat32_fs {
    struct blockdev *dev;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;    /* sectors before the first FAT */
    uint32_t num_fats;
    uint32_t fat_size;            /* sectors per FAT */
    uint32_t root_cluster;
    uint32_t first_data_sector;   /* where cluster 2 starts */
    uint32_t total_clusters;
    char     label[12];

    uint32_t fat_cache_sector;    /* which FAT sector is in fat_cache */
    uint8_t  fat_cache[SECTOR_SIZE];
    /* Where the last read stopped: cluster number pos_cluster is the
       pos_index-th of the chain that starts at pos_first (fat32_read). */
    uint32_t pos_first, pos_index, pos_cluster;
};

/* A file or directory, as found in a directory listing. */
struct fat32_dirent {
    char     name[256];           /* long name if there is one, else 8.3 name */
    uint8_t  attr;
    uint32_t cluster;             /* first cluster of the data */
    uint32_t size;                /* in bytes (0 for directories) */
};

/* Check the boot sector and fill in `fs`. Returns 0, or -1 if not FAT32. */
int fat32_mount(struct fat32_fs *fs, struct blockdev *dev);

/* Call `cb` for every entry in a directory ("." and ".." are skipped).
   If `cb` returns non-zero, stop and return that value. Returns 0 at the
   end of the directory, -1 on disk error. */
typedef int (*fat32_dir_cb)(const struct fat32_dirent *e, void *ctx);
int fat32_readdir(struct fat32_fs *fs, const struct fat32_dirent *dir,
                  fat32_dir_cb cb, void *ctx);

/* Find a file or directory by path, e.g. "/docs/notes.txt" (case-insensitive,
   like FAT itself). "/" is the root directory. Returns 0 or -1. */
int fat32_lookup(struct fat32_fs *fs, const char *path, struct fat32_dirent *out);

/* Read up to `len` bytes of a file starting at `offset`.
   Returns the number of bytes read (0 at end of file), or -1 on error. */
int fat32_read(struct fat32_fs *fs, const struct fat32_dirent *file,
               uint32_t offset, void *buf, uint32_t len);

/* Mount a FAT32 partition into the VFS at `path` (read-only). 0 or -errno. */
int fat_vfs_mount(struct blockdev *dev, const char *path);

#endif
