#ifndef LINUX_WAIT_H
#define LINUX_WAIT_H

/* Wait queues: sleep until a condition holds; whoever changes it calls
   wake_up. The condition is re-checked after every wake-up (a wake-up
   means "look again", not "it is true"). Interrupts are off around the
   check and the sleep, so a wake_up from an interrupt can't fall between
   them and be lost. */
#include <interrupts.h>
#include <linux/jiffies.h>
#include <signal.h>
#include <linux/types.h>
#include <thread.h>

struct wait_queue_head { struct wait_queue q; };
typedef struct wait_queue_head wait_queue_head_t;
#define __WAIT_QUEUE_HEAD_INITIALIZER(name) { { NULL } }
#define DECLARE_WAIT_QUEUE_HEAD(name) wait_queue_head_t name = __WAIT_QUEUE_HEAD_INITIALIZER(name)
static inline void init_waitqueue_head(wait_queue_head_t *h) { h->q.first = NULL; }
static inline void __linux_wake(wait_queue_head_t *h)
{
    unsigned long f = irq_save();
    wq_wake_all(&h->q);
    irq_restore(f);
}
#define wake_up(h)                    __linux_wake(h)
#define wake_up_all(h)                __linux_wake(h)
#define wake_up_interruptible(h)      __linux_wake(h)
#define wake_up_interruptible_all(h)  __linux_wake(h)
#define waitqueue_active(h)           ((h)->q.first != NULL)

#define wait_event(wq, cond) do {                                      \
        unsigned long _f = irq_save();                                 \
        while (!(cond))                                                \
            wq_sleep(&(wq).q);                                         \
        irq_restore(_f);                                               \
    } while (0)

/* The time left in jiffies (at least 1) when cond came true, 0 if not. */
#define wait_event_timeout(wq, cond, timeout) ({                       \
        unsigned long _end = jiffies + (timeout);                      \
        long _ret = 1;                                                 \
        unsigned long _f = irq_save();                                 \
        while (!(cond)) {                                              \
            if (!time_before(jiffies, _end) ||                         \
                (wq_sleep_until(&(wq).q, (uint32_t)_end ? (uint32_t)_end : 1) && !(cond))) { \
                _ret = (cond) ? 1 : 0;                                 \
                break;                                                 \
            }                                                          \
        }                                                              \
        if (_ret) {                                                    \
            long _left = (long)(_end - jiffies);                       \
            _ret = _left > 0 ? _left : 1;                              \
        }                                                              \
        irq_restore(_f);                                               \
        _ret; })

#define wait_event_interruptible(wq, cond) ({                          \
        int _ret = 0;                                                  \
        unsigned long _f = irq_save();                                 \
        while (!(cond)) {                                              \
            if (signal_pending()) { _ret = -512; break; }              \
            wq_sleep(&(wq).q);                                         \
        }                                                              \
        irq_restore(_f);                                               \
        _ret; })
#define wait_event_interruptible_timeout(wq, cond, timeout) wait_event_timeout(wq, cond, timeout)
#define wait_event_killable(wq, cond) wait_event_interruptible(wq, cond)

#endif
