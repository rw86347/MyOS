#ifndef PCACHE_H
#define PCACHE_H

#include <stdint.h>

/*
 * Page cache: one 4 KiB frame per (file, page), shared by every reader and
 * writer of a disk file. Reads miss into a read-ahead run; writes dirty the
 * page and go to the disk later, in runs (pcache_flush / pcache_sync, the
 * flusher thread once a second, or when too much is dirty).
 */

struct inode;
struct pc_page;

void pcache_init(void);         /* after the heap; one slot per frame of RAM */

/* Copy file bytes into kernel memory. 0 or -errno. */
int pcache_read(struct inode *ino, uint32_t pos, void *dst, uint32_t n);

/* The same into a user buffer (read()): bytes copied, or -EFAULT / -EIO. */
int pcache_read_user(struct inode *ino, uint32_t pos, void *user_dst, uint32_t n);

/* For a file system's write(): page `index`, pinned. need_old: the write
   doesn't replace all of the page's existing bytes, so read them first;
   else a fresh zeroed page. NULL: *err says why. Then change the bytes at
   pcache_page_data() and give it back with pcache_put_write. */
struct pc_page *pcache_get_write(struct inode *ino, uint32_t index, int need_old, int *err);
void *pcache_page_data(struct pc_page *p);
void pcache_put_write(struct inode *ino, struct pc_page *p, int dirty);
int  pcache_too_dirty(void);    /* the writer should flush its file now */

int pcache_flush(struct inode *ino);    /* write the file's dirty pages; sleeps */
int pcache_sync(void);                  /* ...every file's */

/* Forget every cached page of this file, dirty ones too. */
void pcache_invalidate(struct inode *ino);

/* Clean pages back to the page allocator (it calls this when it runs out). */
uint32_t pcache_reclaim(uint32_t want);

/* How many page fills missed the cache, and how many were already there. */
void pcache_stats(uint32_t *hits, uint32_t *misses);

/* Frames the cache is holding. The leak check in the self-test counts these
   the same way it counts block-cache frames: they are supposed to stay. */
uint32_t pcache_pages(void);
uint32_t pcache_ra_pages(void);        /* pages read ahead without waiting */

#endif
