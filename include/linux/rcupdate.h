/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_RCUPDATE_H
#define LINUX_RCUPDATE_H

/*
 * RCU ("read, copy, update"): readers look at shared data with no lock; a
 * writer publishes a new version and frees the old one only when no reader
 * can still be looking. With one CPU and kernel code that only gives up the
 * CPU by sleeping (and an RCU reader may not sleep), no reader is ever in
 * the middle of reading while a writer runs: a grace period has always
 * passed. That is Linux's "Tiny RCU". Freeing is still deferred
 * (call_rcu, kfree_rcu) to a worker thread, so a writer that is itself
 * inside a read section never frees what it is looking at.
 */
#include <linux/compiler.h>
#include <linux/list.h>

struct rcu_head {
    struct rcu_head *next;
    void (*func)(struct rcu_head *);
};

#define rcu_read_lock()            barrier()
#define rcu_read_unlock()          barrier()
#define rcu_read_lock_bh()         barrier()
#define rcu_read_unlock_bh()       barrier()
#define rcu_read_lock_held()       1
#define synchronize_rcu()          barrier()
#define synchronize_net()          barrier()
#define rcu_barrier()              rcu_flush()
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *));
void kfree_rcu_ptr(void *ptr);      /* (the object, freed later) */
void rcu_flush(void);               /* run the pending callbacks now */
#define kfree_rcu(ptr, field)      kfree_rcu_ptr(ptr)
#define kvfree_rcu(ptr, ...)       kfree_rcu_ptr(ptr)

#define rcu_dereference(p)              READ_ONCE(p)
#define rcu_dereference_bh(p)           READ_ONCE(p)
#define rcu_dereference_raw(p)          READ_ONCE(p)
#define rcu_dereference_check(p, c)     READ_ONCE(p)
#define rcu_dereference_protected(p, c) (p)
#define rcu_access_pointer(p)           READ_ONCE(p)
#define rcu_assign_pointer(p, v)        do { barrier(); WRITE_ONCE(p, v); } while (0)
#define RCU_INIT_POINTER(p, v)          WRITE_ONCE(p, v)
#define rcu_replace_pointer(p, v, c)    ({ __typeof__(p) _old = (p); rcu_assign_pointer(p, v); _old; })

/* The list operations readers may race with: publish the links last. */
#define list_add_rcu(n, h)              list_add(n, h)
#define list_add_tail_rcu(n, h)         list_add_tail(n, h)
#define list_del_rcu(e)                 __list_del((e)->prev, (e)->next)
#define list_for_each_entry_rcu(pos, head, member, ...) list_for_each_entry(pos, head, member)
#define list_first_or_null_rcu(h, t, m) list_first_entry_or_null(h, t, m)
#define hlist_add_head_rcu(n, h)        hlist_add_head(n, h)
#define hlist_del_rcu(n)                hlist_del(n)
#define hlist_for_each_entry_rcu(pos, head, member, ...) hlist_for_each_entry(pos, head, member)

#endif
