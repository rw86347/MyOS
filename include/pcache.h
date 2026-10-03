#ifndef PCACHE_H
#define PCACHE_H

#include <stdint.h>

/*
 * Page cache: one 4 KiB frame per (file, page), shared by every reader.
 *
 * exec and mmap fault a page in through here, and the next page is read
 * ahead, so a program walking a file (or the dynamic linker walking a
 * library) does not wait on the disk for every page. A write drops that
 * file's cached pages; the next read fills them again from the disk.
 */

struct inode;

/* Copy file bytes into kernel memory. Disk files come from this cache. */
int pcache_read(struct inode *ino, uint32_t pos, void *dst, uint32_t n);

/* Forget every cached page of this file (after a write, or when it is freed). */
void pcache_invalidate(struct inode *ino);

/* How many page fills missed the cache, and how many were already there. */
void pcache_stats(uint32_t *hits, uint32_t *misses);

/* Frames the cache is holding. The leak check in the self-test counts these
   the same way it counts block-cache frames: they are supposed to stay. */
uint32_t pcache_pages(void);

#endif
