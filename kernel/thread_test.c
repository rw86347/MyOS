/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * thread_test.c — boot-time demo + self-test of the context switch.
 *
 * Test 1 (cooperative): "ping" and "pong" each print their name 3 times and
 * yield after every print. If switching works, the output alternates:
 *     Context switch: ping pong ping pong ping pong
 *
 * Test 2 (preemptive): two threads spin in a loop counting, and never yield.
 * The only way both counters can grow is if the timer takes the CPU away.
 */
#include <interrupts.h>
#include <kprintf.h>
#include <sched.h>
#include <selftest.h>
#include <string.h>
#include <thread.h>
#include <thread_test.h>
#include <timer.h>

static char order[64];          /* first letter of each print: "pPpPpP" */
static int  norder;
static int  local_ok = 1;

static void worker(void *arg)
{
    char tag = *(const char *)arg;          /* 'p' for ping, 'P' for pong */
    for (int i = 0; i < 3; i++) {
        /* A local variable must survive the switch: it lives on our own stack. */
        int mine = i * 100 + tag;
        kprintf(" %s", thread_current()->name);
        order[norder++] = tag;
        thread_yield();
        if (mine != i * 100 + tag)
            local_ok = 0;
    }
}                                           /* returning = thread_exit() */

static volatile uint32_t spins[2];
static volatile int stop;

static void spinner(void *arg)
{
    int me = (int)arg;
    while (!stop)
        spins[me]++;            /* no thread_yield() anywhere */
}

static void test_preemption(void)
{
    sched_set_preempt(1);
    struct thread *a = thread_create("spin0", spinner, (void *)0);
    struct thread *b = thread_create("spin1", spinner, (void *)1);
    check(a && b, "thread: create two spinners");

    sleep_ms(50);               /* main sleeps; the spinners share the CPU */
    stop = 1;
    while (a->state != THREAD_DEAD || b->state != THREAD_DEAD)
        cpu_wait();             /* wait for them to notice and finish */
    sched_set_preempt(0);

    kprintf("Preemption: spin0 counted %lu, spin1 counted %lu in 50 ms\n",
            spins[0], spins[1]);
    check(spins[0] > 0 && spins[1] > 0, "thread: timer preempts threads that never yield");
}

static void test_cooperative(void)
{
    static const char ping = 'p', pong = 'P';

    kprintf("Context switch:");
    struct thread *a = thread_create("ping", worker, (void *)&ping);
    struct thread *b = thread_create("pong", worker, (void *)&pong);
    check(a && b, "thread: create two threads");

    /* main keeps yielding until both workers have finished. */
    while (a->state != THREAD_DEAD || b->state != THREAD_DEAD)
        thread_yield();
    kprintf("\n");

    order[norder] = '\0';
    check(strcmp(order, "pPpPpP") == 0, "thread: ping and pong alternate");
    check(local_ok, "thread: local variables survive a switch");
    check(strcmp(thread_current()->name, "main") == 0, "thread: back in main");
}

void thread_selftest(void)
{
    test_cooperative();     /* preemption off: the order must be exact */
    test_preemption();
}
