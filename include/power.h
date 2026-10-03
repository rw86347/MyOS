#ifndef POWER_H
#define POWER_H

/*
 * Restart, halt, power off (drivers/power.c), for the reboot() system call.
 * None of them return. The caller syncs the disks first.
 */
void power_restart(void) __attribute__((noreturn));
void power_halt(void) __attribute__((noreturn));
void power_off(void) __attribute__((noreturn));   /* or halt, if we can't */

/* What Ctrl-Alt-Del does: restart at once (1), or send SIGINT to init (0),
   which then shuts down cleanly. Init picks (reboot(CAD_ON / CAD_OFF)). */
void power_set_ctrl_alt_del(int restart_now);

/* The keyboard saw Ctrl-Alt-Del. Interrupt context. */
void power_ctrl_alt_del(void);

#endif
