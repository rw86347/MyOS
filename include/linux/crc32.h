#ifndef LINUX_CRC32_H
#define LINUX_CRC32_H
#include <linux/types.h>
u32 crc32_le(u32 crc, const void *p, size_t len);
u32 crc32_be(u32 crc, const void *p, size_t len);
#define crc32(seed, p, len) crc32_le(seed, p, len)
#endif
