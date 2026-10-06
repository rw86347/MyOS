#ifndef LINUX_PERCPU_H
#define LINUX_PERCPU_H
/* Per-CPU variables: one copy per CPU, so CPUs never share a cache line.
   With one CPU there is one copy, and a per-CPU pointer is a plain one. */
#include <linux/slab.h>
#define alloc_percpu(type)          ((type *)kzalloc(sizeof(type), GFP_KERNEL))
#define alloc_percpu_gfp(type, gfp) ((type *)kzalloc(sizeof(type), gfp))
#define free_percpu(p)              kfree(p)
#define per_cpu_ptr(p, cpu)         ((void)(cpu), (p))
#define this_cpu_ptr(p)             (p)
#define raw_cpu_ptr(p)              (p)
#define get_cpu_ptr(p)              (p)
#define put_cpu_ptr(p)              do { (void)(p); } while (0)
#endif
