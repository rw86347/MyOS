/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_CPUMASK_H
#define LINUX_CPUMASK_H
/* A set of CPUs. MyOS runs on one CPU (number 0), so a mask is one word
   and every "spread this over the CPUs" request lands on CPU 0. */
#include <linux/bitops.h>
#include <linux/cpu.h>
#define NR_CPUS 1
typedef struct cpumask { unsigned long bits[1]; } cpumask_t;
#define nr_cpu_ids 1u
static inline void cpumask_set_cpu(unsigned int cpu, struct cpumask *m) { m->bits[0] |= 1ul << cpu; }
static inline void cpumask_clear_cpu(unsigned int cpu, struct cpumask *m) { m->bits[0] &= ~(1ul << cpu); }
static inline void cpumask_clear(struct cpumask *m) { m->bits[0] = 0; }
static inline bool cpumask_test_cpu(int cpu, const struct cpumask *m) { return m->bits[0] >> cpu & 1; }
/* The next CPU in the mask after n (nr_cpu_ids when none). */
static inline unsigned int cpumask_next(int n, const struct cpumask *m)
{
    for (unsigned int c = (unsigned int)(n + 1); c < nr_cpu_ids; c++)
        if (m->bits[0] >> c & 1)
            return c;
    return nr_cpu_ids;
}
static inline unsigned int cpumask_first(const struct cpumask *m) { return cpumask_next(-1, m); }
#define cpu_online_mask   (&(const struct cpumask){ { 1 } })
#define cpu_possible_mask cpu_online_mask
#endif
