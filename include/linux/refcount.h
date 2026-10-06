#ifndef LINUX_REFCOUNT_H
#define LINUX_REFCOUNT_H

/* Reference counts (refcount_t) and kref: the object is freed by whoever
   drops the last reference. */
#include <linux/atomic.h>
#include <linux/kernel.h>

typedef struct { atomic_t refs; } refcount_t;
#define REFCOUNT_INIT(n)          { .refs = ATOMIC_INIT(n) }
#define refcount_set(r, n)        atomic_set(&(r)->refs, (n))
#define refcount_read(r)          atomic_read(&(r)->refs)
#define refcount_inc(r)           atomic_inc(&(r)->refs)
#define refcount_dec(r)           atomic_dec(&(r)->refs)
#define refcount_dec_and_test(r)  atomic_dec_and_test(&(r)->refs)
static inline bool refcount_inc_not_zero(refcount_t *r)
{
    int old = atomic_read(&r->refs);
    while (old && atomic_cmpxchg(&r->refs, old, old + 1) != old)
        old = atomic_read(&r->refs);
    return old != 0;
}

struct kref { refcount_t refcount; };
#define KREF_INIT(n) { .refcount = REFCOUNT_INIT(n) }
static inline void kref_init(struct kref *k) { refcount_set(&k->refcount, 1); }
static inline void kref_get(struct kref *k) { refcount_inc(&k->refcount); }
static inline unsigned int kref_read(const struct kref *k) { return (unsigned int)atomic_read(&k->refcount.refs); }
static inline int kref_put(struct kref *k, void (*release)(struct kref *))
{
    if (refcount_dec_and_test(&k->refcount)) {
        release(k);
        return 1;
    }
    return 0;
}

#endif
