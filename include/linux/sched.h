#ifndef LINUX_SCHED_H
#define LINUX_SCHED_H
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/wait.h>
#define TASK_RUNNING         0
#define TASK_INTERRUPTIBLE   1
#define TASK_UNINTERRUPTIBLE 2
#define MAX_SCHEDULE_TIMEOUT LONG_MAX
#define current_pid_k()      0
long schedule_timeout(long timeout);
#define schedule_timeout_interruptible(t)   schedule_timeout(t)
#define schedule_timeout_uninterruptible(t) schedule_timeout(t)
#define set_current_state(s) do { } while (0)
#define __set_current_state(s) do { } while (0)
#define schedule()           thread_yield()
#endif
