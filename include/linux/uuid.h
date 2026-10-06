#ifndef LINUX_UUID_H
#define LINUX_UUID_H
/* A GUID (UEFI variables and ACPI methods are named by them): 16 bytes,
   the first three fields little-endian. */
#include <linux/types.h>
#include <string.h>
typedef struct { u8 b[16]; } guid_t;
#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                     \
    ((guid_t){ { (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, \
                 (b) & 0xff, ((b) >> 8) & 0xff, (c) & 0xff, ((c) >> 8) & 0xff,     \
                 (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) } })
static inline bool guid_equal(const guid_t *a, const guid_t *b) { return memcmp(a, b, sizeof *a) == 0; }
#endif
