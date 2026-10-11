/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_CLEANUP_H
#define LINUX_CLEANUP_H

/*
 * Scoped cleanup, Linux's way: guard(mutex)(&m) locks m and unlocks it
 * when the enclosing block ends, however it ends (the compiler's cleanup
 * attribute runs a function when a variable goes out of scope).
 * scoped_guard(mutex, &m) { ... } does the same for one block.
 */
#include <linux/compiler.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>

#define __cleanup(fn)            __attribute__((cleanup(fn)))
#define __free(name)             __cleanup(__free_##name)
#define DEFINE_FREE(name, type, free) \
    static inline void __free_##name(void *p) { type _T = *(type *)p; free; }
#define no_free_ptr(p)           ({ __typeof__(p) __p = (p); (p) = NULL; __p; })
#define return_ptr(p)            return no_free_ptr(p)

#define __guard_name(name)       guard_##name##_t
#define DEFINE_GUARD(name, type, lock, unlock)                                   \
    typedef type __guard_name(name);                                             \
    static inline void __guard_##name##_exit(__guard_name(name) *_p) { type _T = *_p; if (_T) { unlock; } } \
    static inline __guard_name(name) __guard_##name##_enter(type _T) { lock; return _T; }
#define __UNIQUE(p)              __UNIQ2(p, __COUNTER__)
#define __UNIQ2(p, n)            __UNIQ3(p, n)
#define __UNIQ3(p, n)            p##n
#define guard(name)                                                               \
    __guard_name(name) __UNIQUE(__guard) __cleanup(__guard_##name##_exit) = __guard_##name##_enter
#define scoped_guard(name, ...)                                                   \
    for (__guard_name(name) _scope __cleanup(__guard_##name##_exit) = __guard_##name##_enter(__VA_ARGS__), \
         *_once = (void *)1; _once; _once = NULL)

/* The guards Linux drivers use. (lock and unlock see _T, the guarded value.) */
DEFINE_GUARD(mutex, struct mutex *, mutex_lock(_T), mutex_unlock(_T))
DEFINE_GUARD(spinlock_bh, spinlock_t *, spin_lock_bh(_T), spin_unlock_bh(_T))
DEFINE_GUARD(spinlock, spinlock_t *, spin_lock(_T), spin_unlock(_T))
typedef int __guard_name(rcu);
static inline void __guard_rcu_exit(int *p) { (void)p; rcu_read_unlock(); }
static inline int __guard_rcu_enter(void) { rcu_read_lock(); return 1; }

#endif
