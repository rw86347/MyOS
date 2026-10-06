#ifndef LINUX_COMPILER_TYPES_H
#define LINUX_COMPILER_TYPES_H

#include <linux/kconfig.h>

/* What Linux's compiler.h gives drivers: branch hints, attributes, and
   READ_ONCE/WRITE_ONCE (one real load or store the compiler can't merge
   or repeat). Linux's build includes this in every file (-include), and
   so does ours for Linux drivers. */
#define likely(x)            __builtin_expect(!!(x), 1)
#define unlikely(x)          __builtin_expect(!!(x), 0)
#define barrier()            __asm__ volatile ("" ::: "memory")
#define READ_ONCE(x)         (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v)     do { *(volatile __typeof__(x) *)&(x) = (v); } while (0)
#define __packed             __attribute__((packed))
#define __aligned(n)         __attribute__((aligned(n)))
#define __must_check         __attribute__((warn_unused_result))
#define __always_inline      inline __attribute__((always_inline))
#define noinline             __attribute__((noinline))
#define __maybe_unused       __attribute__((unused))
#define __always_unused      __attribute__((unused))
#define __used               __attribute__((used))
#define __printf(a, b)       __attribute__((format(printf, a, b)))
#define __cold               __attribute__((cold))
#define __weak               __attribute__((weak))
#define __iomem
#define __user
#define __force
#define __rcu
#define __percpu
#define __bitwise
#define __init
#define __exit
#define __initdata
#define __read_mostly
#define __ro_after_init
#define __acquires(x)
#define __acquire(x)         ((void)0)   /* (sparse lock annotations) */
#define __release(x)         ((void)0)
#define __releases(x)
#define __must_hold(x)
#define __counted_by(m)
#define __nonstring
#define fallthrough          __attribute__((fallthrough))
#define __same_type(a, b)    __builtin_types_compatible_p(__typeof__(a), __typeof__(b))
/* A statement that fails to compile when c is a true constant (a negative
   array size). Like Linux's, it may be given a non-constant (inside a
   __builtin_constant_p test); then it checks nothing. */
#define BUILD_BUG_ON(c)      ((void)sizeof(char[1 - 2 * !!(c)]))
#define BUILD_BUG_ON_ZERO(e) ((int)(sizeof(struct { int:(-!!(e)); })))
#define static_assert(...)   _Static_assert(__VA_ARGS__)
#define __stringify_1(x)     #x
#define __stringify(x)       __stringify_1(x)

#endif
