#ifndef LINUX_GFP_H
#define LINUX_GFP_H

#include <linux/bits.h>

/* How an allocation may be satisfied ("get free pages" flags). Ours never
   sleeps to find memory; only __GFP_ZERO changes what happens. */
#define __GFP_HIGHMEM  0x0002u
#define __GFP_DMA32    0x0004u
#define __GFP_ZERO     0x0100u
#define __GFP_NOWARN   0x0200u
#define __GFP_RETRY_MAYFAIL 0x0400u
#define __GFP_NORETRY  0x0800u
#define __GFP_COMP     0x1000u
#define GFP_DMA        0x0001u
#define GFP_KERNEL     0x0010u
#define GFP_ATOMIC     0x0020u
#define GFP_NOWAIT     0x0040u
#define GFP_NOIO       0x0080u
#define GFP_NOFS       0x0080u
#define GFP_DMA32      __GFP_DMA32
#define GFP_USER       GFP_KERNEL
#define GFP_KERNEL_ACCOUNT GFP_KERNEL

#endif
