#ifndef SCHED_H
#define SCHED_H

#include <thread.h>

/*
 * The time-slot scheduler (see docs/DESIGN.md).
 *
 * Time repeats in a "major frame" of SCHED_SLOTS slots, 1 ms each.
 * Slots 1 and 10 run the system (the "main" thread). Other slots run the
 * named thread that config/slots.conf assigns to them, or the idle thread.
 *
 * Until sched_start() the scheduler is in a simple round-robin mode, used
 * during boot and by the thread self-tests.
 */

#define SCHED_SLOTS         20
#define SCHED_SYSTEM_THREAD "main"      /* owns the system slots 1 and 10 */

/* slot_config[s] = name of the thread that owns slot s, or NULL.
   Generated at build time from config/slots.conf. */
extern const char *const slot_config[SCHED_SLOTS + 1];

/* Switch from round robin to the slot table. Creates the idle thread. */
void sched_start(void);

/* The slot running right now (1-20), or 0 before sched_start(). */
int sched_slot(void);

/* Name of the owner of slot s per the config (system slots included),
   or NULL if the slot is free. */
const char *sched_slot_owner(int s);

/* Round-robin mode only: when on, every timer tick switches threads. */
void sched_set_preempt(int on);

/* --- called by the thread and interrupt code --- */

/* Which thread should run now? `yielding` = the current thread is giving
   up the CPU (or exiting). Interrupts must be off. NULL = keep running. */
struct thread *sched_pick(int yielding);

void sched_tick(void);              /* timer IRQ, every 1 ms */
void sched_preempt_check(void);     /* end of every interrupt, after the EOI */
void sched_need_resched(void);      /* switch at the end of this interrupt */

#endif
