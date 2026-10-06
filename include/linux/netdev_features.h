#ifndef LINUX_NETDEV_FEATURES_H
#define LINUX_NETDEV_FEATURES_H
/* What a network device can do for the stack (checksums, segmentation,
   DMA from anywhere). Bit numbers as in Linux (ethtool shows them). */
#include <linux/types.h>
typedef u64 netdev_features_t;
#define __NETIF_F(bit)       ((netdev_features_t)1 << (bit))
#define NETIF_F_SG           __NETIF_F(0)   /* scatter/gather: packet in pieces */
#define NETIF_F_IP_CSUM      __NETIF_F(1)   /* computes TCP/UDP over IPv4 checksums */
#define NETIF_F_HW_CSUM      __NETIF_F(3)
#define NETIF_F_IPV6_CSUM    __NETIF_F(4)
#define NETIF_F_HIGHDMA      __NETIF_F(5)   /* can DMA above 4 GiB */
#define NETIF_F_TSO          __NETIF_F(16)  /* splits big TCP packets itself */
#define NETIF_F_TSO6         __NETIF_F(20)
#define NETIF_F_RXCSUM       __NETIF_F(40)  /* checks received checksums */
#define NETIF_F_CSUM_MASK    (NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM | NETIF_F_HW_CSUM)
#endif
