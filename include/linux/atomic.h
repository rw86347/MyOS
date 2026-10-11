/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_ATOMIC_H
#define LINUX_ATOMIC_H

/* Counters an interrupt handler may change under us: the CPU's locked
   read-modify-write instructions (the compiler's __atomic builtins). */
#include <linux/types.h>

typedef struct { int counter; } atomic_t;
typedef struct { s64 counter __attribute__((aligned(8))); } atomic64_t;

#define ATOMIC_INIT(i)                 { (i) }
#define atomic_read(v)                 __atomic_load_n(&(v)->counter, __ATOMIC_RELAXED)
#define atomic_set(v, i)               __atomic_store_n(&(v)->counter, (i), __ATOMIC_RELAXED)
#define atomic_add(i, v)               ((void)__atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_sub(i, v)               ((void)__atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_inc(v)                  atomic_add(1, v)
#define atomic_dec(v)                  atomic_sub(1, v)
#define atomic_add_return(i, v)        __atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_sub_return(i, v)        __atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_inc_return(v)           atomic_add_return(1, v)
#define atomic_dec_return(v)           atomic_sub_return(1, v)
#define atomic_fetch_add(i, v)         __atomic_fetch_add(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_fetch_inc(v)            atomic_fetch_add(1, v)
#define atomic_dec_and_test(v)         (atomic_sub_return(1, v) == 0)
#define atomic_inc_and_test(v)         (atomic_add_return(1, v) == 0)
#define atomic_sub_and_test(i, v)      (atomic_sub_return(i, v) == 0)
#define atomic_xchg(v, n)              __atomic_exchange_n(&(v)->counter, (n), __ATOMIC_SEQ_CST)
static inline int atomic_cmpxchg(atomic_t *v, int old, int n)
{ __atomic_compare_exchange_n(&v->counter, &old, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return old; }
/* Add a unless the value is u; returns the value before. */
static inline int atomic_fetch_add_unless(atomic_t *v, int a, int u)
{
    int c = atomic_read(v);
    while (c != u && !__atomic_compare_exchange_n(&v->counter, &c, c + a, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        ;
    return c;
}
#define atomic_add_unless(v, a, u)     (atomic_fetch_add_unless((v), (a), (u)) != (u))
#define atomic_inc_not_zero(v)         atomic_add_unless((v), 1, 0)
/* Decrement unless that would go below zero; returns the new value (or -1, untouched). */
static inline int atomic_dec_if_positive(atomic_t *v)
{
    int c = atomic_read(v);
    while (c > 0 && !__atomic_compare_exchange_n(&v->counter, &c, c - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        ;
    return c - 1;
}
#define atomic_or(i, v)                ((void)__atomic_or_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_and(i, v)               ((void)__atomic_and_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#ifdef __x86_64__
#define atomic64_read(v)               __atomic_load_n(&(v)->counter, __ATOMIC_SEQ_CST)
#define atomic64_set(v, i)             __atomic_store_n(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic64_add_return(i, v)      __atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#else
/* A 32-bit CPU loads a 64-bit value atomically only through the FPU or SSE,
   which the kernel doesn't touch (-mgeneral-regs-only). One CPU, so with
   interrupts off for the two instructions nothing can come between. */
#include <interrupts.h>
static inline s64 atomic64_read(const atomic64_t *v)
{
    unsigned long f = irq_save();
    s64 x = v->counter;
    irq_restore(f);
    return x;
}
static inline void atomic64_set(atomic64_t *v, s64 i)
{
    unsigned long f = irq_save();
    v->counter = i;
    irq_restore(f);
}
static inline s64 atomic64_add_return(s64 i, atomic64_t *v)
{
    unsigned long f = irq_save();
    s64 x = v->counter += i;
    irq_restore(f);
    return x;
}
#endif
#define atomic64_add(i, v)             ((void)atomic64_add_return((i), (v)))
#define atomic64_inc(v)                atomic64_add(1, v)
#define atomic64_inc_return(v)         atomic64_add_return(1, (v))
#define smp_mb()                       __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_rmb()                      __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_wmb()                      __atomic_thread_fence(__ATOMIC_RELEASE)
#define mb()                           smp_mb()
#define rmb()                          smp_rmb()
#define wmb()                          smp_wmb()
#define dma_rmb()                      smp_rmb()
#define dma_wmb()                      smp_wmb()
#define smp_mb__before_atomic()        smp_mb()
#define smp_mb__after_atomic()         smp_mb()
#define smp_load_acquire(p)            __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p, v)        __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define xchg(p, v)                     __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define cmpxchg(p, o, n) ({ __typeof__(*(p)) _o = (o); \
    __atomic_compare_exchange_n((p), &_o, (n), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); _o; })

#endif
