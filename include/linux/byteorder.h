#ifndef LINUX_BYTEORDER_H
#define LINUX_BYTEORDER_H

/* How devices and networks store numbers. x86 (and our ARM target) is
   little-endian, so the le* conversions are no-ops and be* swap bytes. */
#include <linux/types.h>

#define cpu_to_le16(x) ((__le16)(u16)(x))
#define cpu_to_le32(x) ((__le32)(u32)(x))
#define cpu_to_le64(x) ((__le64)(u64)(x))
#define le16_to_cpu(x) ((u16)(__le16)(x))
#define le32_to_cpu(x) ((u32)(__le32)(x))
#define le64_to_cpu(x) ((u64)(__le64)(x))
#define cpu_to_be16(x) ((__be16)__builtin_bswap16((u16)(x)))
#define cpu_to_be32(x) ((__be32)__builtin_bswap32((u32)(x)))
#define cpu_to_be64(x) ((__be64)__builtin_bswap64((u64)(x)))
#define be16_to_cpu(x) __builtin_bswap16((u16)(__be16)(x))
#define be32_to_cpu(x) __builtin_bswap32((u32)(__be32)(x))
#define be64_to_cpu(x) __builtin_bswap64((u64)(__be64)(x))
#define le16_to_cpup(p) le16_to_cpu(*(p))
#define __cpu_to_le16(x) cpu_to_le16(x)
#define __cpu_to_le32(x) cpu_to_le32(x)
#define __le16_to_cpu(x) le16_to_cpu(x)
#define __le32_to_cpu(x) le32_to_cpu(x)
#define __le64_to_cpu(x) le64_to_cpu(x)
#define __le16_to_cpup(p) le16_to_cpup(p)
#define __le32_to_cpup(p) le32_to_cpup(p)
#define le32_to_cpup(p) le32_to_cpu(*(p))
#define le64_to_cpup(p) le64_to_cpu(*(p))
#define be16_to_cpup(p) be16_to_cpu(*(p))
#define be32_to_cpup(p) be32_to_cpu(*(p))
#define le16_add_cpu(p, v) (*(p) = cpu_to_le16(le16_to_cpu(*(p)) + (v)))
#define le32_add_cpu(p, v) (*(p) = cpu_to_le32(le32_to_cpu(*(p)) + (v)))
#define swab16(x) __builtin_bswap16(x)
#define swab32(x) __builtin_bswap32(x)
#define swab64(x) __builtin_bswap64(x)
#ifndef htons
#define htons(x) cpu_to_be16(x)
#define ntohs(x) be16_to_cpu(x)
#define htonl(x) cpu_to_be32(x)
#define ntohl(x) be32_to_cpu(x)
#endif

#endif
