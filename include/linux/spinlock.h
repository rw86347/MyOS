#ifndef LINUX_SPINLOCK_H
#define LINUX_SPINLOCK_H

/*
 * Spinlocks. On a machine with one CPU and a kernel that is never switched
 * away in the middle of kernel code (it only switches when a thread
 * sleeps), the only thing that can run "at the same time" as a driver is an
 * interrupt handler. So a spinlock is exactly: interrupts off while it is
 * held. Each lock remembers whether they were on, so locks nest.
 * (Linux's own non-SMP build reduces them the same way.)
 */
#include <interrupts.h>
#include <linux/compiler.h>
#include <linux/bitops.h>

typedef struct { unsigned long flags; } spinlock_t;
typedef spinlock_t raw_spinlock_t;
typedef spinlock_t rwlock_t;

#define __SPIN_LOCK_UNLOCKED(x)  { 0 }
#define DEFINE_SPINLOCK(x)       spinlock_t x = { 0 }
#define DEFINE_RWLOCK(x)         rwlock_t x = { 0 }
#define spin_lock_init(l)        ((l)->flags = 0)
#define raw_spin_lock_init(l)    spin_lock_init(l)
#define rwlock_init(l)           spin_lock_init(l)

static inline void spin_lock(spinlock_t *l) { l->flags = irq_save(); }
static inline void spin_unlock(spinlock_t *l) { irq_restore(l->flags); }
static inline int  spin_trylock(spinlock_t *l) { spin_lock(l); return 1; }
#define spin_lock_bh(l)               spin_lock(l)
#define spin_unlock_bh(l)             spin_unlock(l)
#define spin_trylock_bh(l)            spin_trylock(l)
#define spin_lock_irq(l)              spin_lock(l)
#define spin_unlock_irq(l)            spin_unlock(l)
#define spin_lock_irqsave(l, f)       do { (void)(l); (f) = irq_save(); } while (0)
#define spin_unlock_irqrestore(l, f)  do { (void)(l); irq_restore(f); } while (0)
#define spin_lock_nested(l, sub)      spin_lock(l)
#define raw_spin_lock(l)              spin_lock(l)
#define raw_spin_unlock(l)            spin_unlock(l)
#define raw_spin_lock_irqsave(l, f)   spin_lock_irqsave(l, f)
#define raw_spin_unlock_irqrestore(l, f) spin_unlock_irqrestore(l, f)
#define read_lock(l)                  spin_lock(l)
#define read_unlock(l)                spin_unlock(l)
#define write_lock(l)                 spin_lock(l)
#define write_unlock(l)               spin_unlock(l)
#define read_lock_bh(l)               spin_lock(l)
#define read_unlock_bh(l)             spin_unlock(l)
#define write_lock_bh(l)              spin_lock(l)
#define write_unlock_bh(l)            spin_unlock(l)
#define spin_is_locked(l)             0
#define assert_spin_locked(l)         do { (void)(l); } while (0)
#define lockdep_assert_held(l)        do { (void)(l); } while (0)
#define lockdep_assert_not_held(l)    do { (void)(l); } while (0)
#define lockdep_is_held(l)            1
#define local_bh_disable()            do { } while (0)
#define local_bh_enable()             do { } while (0)
#define local_irq_save(f)             ((f) = irq_save())
#define local_irq_restore(f)          irq_restore(f)
#define local_irq_disable()           interrupts_disable()
#define local_irq_enable()            interrupts_enable()
#define in_interrupt()                0
#define in_irq()                      0
#define in_atomic()                   0
#define in_softirq()                  0
#define in_task()                     1
#define irqs_disabled()               1
#define preempt_disable()             barrier()
#define preempt_enable()              barrier()

#endif
