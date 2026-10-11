/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * shim_test.c — the Linux driver API's core (W1a), checked at boot in
 * `make test` builds: printing, parsing, lists, bits, memory, and the
 * pieces with threads behind them — timers, work queues, completions,
 * mutexes.
 */
#include <kprintf.h>
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <selftest.h>

static void t_format(void)
{
    static const u8 mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    static const u8 bytes[3] = { 0xde, 0xad, 0x01 };
    char buf[96];
    int n = snprintf(buf, sizeof buf, "%pM|%*ph|%5.2s|%-4d|%04x|%zu|%llx|%X",
                     mac, 3, bytes, "abc", 7, 0xbe, (size_t)42, 0x123456789abcULL, 255);
    const char *want = "52:54:00:12:34:56|de ad 01|   ab|7   |00be|42|123456789abc|FF";
    check(n == (int)strlen(want) && strcmp(buf, want) == 0, "shim: snprintf, Linux formats");
    if (strcmp(buf, want))
        kprintf("  got  \"%s\"\n", buf);
    char small[8];
    n = snprintf(small, sizeof small, "%d-%d", 12345, 67890);
    check(n == 11 && strcmp(small, "12345-6") == 0, "shim: snprintf cuts, counts the rest");

    u32 u = 0;
    int i = 0;
    u8 b = 0;
    check(kstrtou32("0x10\n", 0, &u) == 0 && u == 16 && kstrtoint("-5", 10, &i) == 0 && i == -5 &&
          kstrtou8("256", 10, &b) == -ERANGE && kstrtou32("12x", 10, &u) == -EINVAL,
          "shim: kstrto*");
    char s[8];
    check(strscpy(s, "hello", sizeof s) == 5 && strscpy(s, "too long here", sizeof s) == -E2BIG &&
          strcmp(s, "too lon") == 0, "shim: strscpy");
}

struct item { int v; struct list_head node; };

static void t_basics(void)
{
    LIST_HEAD(l);
    struct item a = { 1, { 0, 0 } }, c = { 3, { 0, 0 } }, d = { 2, { 0, 0 } };
    list_add_tail(&a.node, &l);
    list_add_tail(&c.node, &l);
    list_add(&d.node, &a.node);         /* after a: 1 2 3 */
    int sum = 0, order = 0;
    struct item *it;
    list_for_each_entry(it, &l, node)
        order = order * 10 + it->v, sum += it->v;
    list_del(&d.node);
    check(order == 123 && sum == 6 && list_count_nodes(&l) == 2, "shim: lists");

    unsigned long bits[2] = { 0, 0 };
    set_bit(3, bits);
    set_bit(BITS_PER_LONG + 1, bits);
    check(find_first_bit(bits, 2 * BITS_PER_LONG) == 3 &&
          find_next_bit(bits, 2 * BITS_PER_LONG, 4) == (unsigned long)BITS_PER_LONG + 1 &&
          find_first_zero_bit(bits, 8) == 0 && FIELD_GET(0x0f00u, 0x1234u) == 2 &&
          FIELD_PREP(0x00f0u, 7) == 0x70 && ilog2(4096) == 12 && hweight32(0xf0f0) == 8,
          "shim: bits and fields");
    check(mul_u64_u64_div_u64(1000000000000ULL, 3000000000000ULL, 1000000000ULL) ==
          3000000000000000ULL, "shim: mul_u64_u64_div_u64 (128-bit middle)");

    char *p = kzalloc(40, GFP_KERNEL);
    int zero = p && !memchr_inv(p, 0, 40);
    p = krealloc(p, 3000, GFP_KERNEL | __GFP_ZERO);
    char *dup = kstrdup("wifi", GFP_KERNEL);
    check(zero && p && !p[39] && !p[2999] && dup && !strcmp(dup, "wifi") && ksize(dup) >= 5,
          "shim: kzalloc, krealloc, kstrdup");
    kfree(p);
    kfree(dup);
}

/* --- threads: timers, work, completions, mutexes ------------------------- */

static int fired[3], fire_order, delayed_ran, cancelled_ran;
static struct timer_list t1, t2, t3;
static void tfn(struct timer_list *t)
{
    int k = t == &t1 ? 0 : t == &t2 ? 1 : 2;
    fired[k] = ++fire_order;
}

static struct completion done;
static struct mutex lock;
static int got_lock_after;              /* 1: the work only got the mutex once we let go */
static volatile int unlocked;
static struct work_struct w_complete, w_lock;
static struct delayed_work dw, dw_cancel;

static void complete_fn(struct work_struct *w) { (void)w; complete(&done); }
static void lock_fn(struct work_struct *w)
{
    (void)w;
    mutex_lock(&lock);
    got_lock_after = unlocked;
    mutex_unlock(&lock);
    complete(&done);
}
static void delayed_fn(struct work_struct *w) { (void)w; delayed_ran = 1; }
static void cancel_fn(struct work_struct *w) { (void)w; cancelled_ran = 1; }

static void t_threads(void)
{
    unsigned long start = jiffies;
    timer_setup(&t1, tfn, 0);
    timer_setup(&t2, tfn, 0);
    timer_setup(&t3, tfn, 0);
    mod_timer(&t1, jiffies + 20);
    mod_timer(&t2, jiffies + 10);
    mod_timer(&t3, jiffies + 15);
    timer_delete(&t3);
    msleep(40);
    check(fired[1] == 1 && fired[0] == 2 && fired[2] == 0 && !timer_pending(&t1),
          "shim: timers fire in order, a deleted one doesn't");
    check(jiffies - start >= 40, "shim: msleep, jiffies");

    init_completion(&done);
    INIT_WORK(&w_complete, complete_fn);
    queue_work(system_wq, &w_complete);
    check(wait_for_completion_timeout(&done, 100) > 0, "shim: work runs, completes");
    unsigned long t0 = jiffies;
    check(wait_for_completion_timeout(&done, 20) == 0 && jiffies - t0 >= 20,
          "shim: completion timeout");

    INIT_DELAYED_WORK(&dw, delayed_fn);
    INIT_DELAYED_WORK(&dw_cancel, cancel_fn);
    queue_delayed_work(system_wq, &dw, 15);
    queue_delayed_work(system_wq, &dw_cancel, 15);
    check(delayed_work_pending(&dw) && cancel_delayed_work_sync(&dw_cancel), "shim: delayed work queued");
    msleep(30);
    flush_delayed_work(&dw);
    check(delayed_ran && !cancelled_ran, "shim: delayed work runs, a cancelled one doesn't");

    mutex_init(&lock);
    mutex_lock(&lock);
    INIT_WORK(&w_lock, lock_fn);
    queue_work(system_wq, &w_lock);
    msleep(10);                         /* the work is now asleep on the mutex */
    unlocked = 1;
    mutex_unlock(&lock);
    check(wait_for_completion_timeout(&done, 100) > 0 && got_lock_after,
          "shim: a mutex makes the other thread wait");

    struct workqueue_struct *own = alloc_ordered_workqueue("shimtest", 0);
    delayed_ran = 0;
    INIT_DELAYED_WORK(&dw, delayed_fn);
    queue_delayed_work(own, &dw, 0);
    flush_workqueue(own);
    check(own && delayed_ran, "shim: a workqueue of its own");
    destroy_workqueue(own);
}

void shim_selftest(void)
{
    t_format();
    t_basics();
    t_threads();
}
