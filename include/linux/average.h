/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_AVERAGE_H
#define LINUX_AVERAGE_H
/* Exponentially weighted moving averages (Linux's DECLARE_EWMA), with the
   precision bits and weight given as powers of two. */
#include <linux/types.h>
#define DECLARE_EWMA(name, _precision, _weight_rcp)                                  \
    struct ewma_##name { unsigned long internal; };                                  \
    static inline void ewma_##name##_init(struct ewma_##name *e) { e->internal = 0; } \
    static inline unsigned long ewma_##name##_read(struct ewma_##name *e)           \
    { return e->internal >> (_precision); }                                          \
    static inline void ewma_##name##_add(struct ewma_##name *e, unsigned long val)   \
    {                                                                                \
        unsigned long internal = e->internal;                                        \
        unsigned long weight_rcp = ilog2(_weight_rcp);                               \
        e->internal = internal ? (((internal << weight_rcp) - internal) +            \
                                  (val << (_precision))) >> weight_rcp               \
                               : (val << (_precision));                              \
    }
#endif
