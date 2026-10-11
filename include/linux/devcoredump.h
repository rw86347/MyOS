/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_DEVCOREDUMP_H
#define LINUX_DEVCOREDUMP_H
/* A firmware crash dump would go to user space through devcoredump; we
   have nowhere to put it yet, so it is freed at once (docs/gaps.md). */
#include <linux/scatterlist.h>
#include <linux/slab.h>
struct device;
/* Free a dump built as chained scatterlist arrays of whole pages. */
void _devcd_free_sgtable(struct scatterlist *table);
static inline void dev_coredumpv(struct device *d, void *data, size_t len, gfp_t g)
{ (void)d; (void)len; (void)g; vfree(data); }
static inline void dev_coredumpm(struct device *d, void *o, void *data, size_t len, gfp_t g,
                                 void *read, void (*free)(void *))
{ (void)d; (void)o; (void)len; (void)g; (void)read; if (free) free(data); }
static inline void dev_coredumpsg(struct device *d, struct scatterlist *t, size_t len, gfp_t g)
{ (void)d; (void)len; (void)g; _devcd_free_sgtable(t); }
#endif
