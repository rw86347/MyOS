#ifndef BENCH_H
#define BENCH_H

/* Kernel benchmarks — compiled only into `make bench` builds (BENCH).
   Prints average cycles per operation, then switches QEMU off. */
#include <stdint.h>
void run_benchmarks(uint64_t boot_start);   /* cycles_now() at kernel_main */

/* A program is about to sleep until there's input (tty read, poll,
   select): the first time after /init starts, the command line is up. */
void bench_input_wait(void);

#endif
