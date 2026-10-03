#ifndef FUZZ_CYCLES_H
#define FUZZ_CYCLES_H
#include <stdint.h>

/* The host has no rdtsc worth reading here, and the fuzzer needs every run to
   be reproducible, so this counts instead of measuring. kernel/random.c is
   compiled for real against it, which means the tests exercise the actual
   ChaCha20 generator rather than a stand-in — they just get the same stream
   every time, which is what -replay depends on. */
uint64_t cycles_now(void);

#endif
