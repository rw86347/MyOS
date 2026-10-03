#ifndef FUTEX_H
#define FUTEX_H

#include <stdint.h>

struct mm;

/* The futex system call (both the 32-bit and the time64 timeout forms). */
int32_t sys_futex_impl(uint32_t uaddr, uint32_t op, uint32_t val, uint32_t timeout,
                       uint32_t uaddr2, uint32_t val3, int time64);

/* Wake up to n threads sleeping on the word at uaddr in address space mm.
   Returns how many were woken. Interrupts off. */
int futex_wake(struct mm *mm, uint32_t uaddr, int n);

#endif
