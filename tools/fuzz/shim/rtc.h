#ifndef FUZZ_RTC_H
#define FUZZ_RTC_H
#include <stdint.h>

/* A fixed "boot time", so random_init is reproducible. See cycles.h. */
static inline uint32_t rtc_read_epoch(void) { return 1767225600u; }

#endif
