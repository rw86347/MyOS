#ifndef LINUX_MUTEX_H
#define LINUX_MUTEX_H

/* A mutex is a lock the holder may sleep with (wait for the hardware,
   allocate). Whoever finds it held sleeps until it is let go. */
#include <linux/compiler.h>
#include <linux/types.h>
#include <thread.h>

struct mutex {
    struct thread *owner;
    struct wait_queue wait;
};
#define __MUTEX_INITIALIZER(name) { NULL, { NULL } }
#define DEFINE_MUTEX(name) struct mutex name = __MUTEX_INITIALIZER(name)
static inline void mutex_init(struct mutex *m) { m->owner = NULL; m->wait.first = NULL; }
#define mutex_destroy(m) do { (void)(m); } while (0)
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
int  mutex_trylock(struct mutex *m);
int  mutex_lock_interruptible(struct mutex *m);
static inline int mutex_is_locked(const struct mutex *m) { return m->owner != NULL; }
#define mutex_lock_nested(m, sub)  mutex_lock(m)

#endif
