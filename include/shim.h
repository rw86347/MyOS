#ifndef SHIM_H
#define SHIM_H

/* The Linux driver API ("the shim": include/linux/, kernel/linux_*.c,
   timer_list.c, workqueue.c). Start its threads — ktimers, and "events"
   (system_wq) — after thread_init(), before any driver uses them. */
void shim_init(void);

#endif
