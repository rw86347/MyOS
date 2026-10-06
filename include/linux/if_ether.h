#ifndef LINUX_IF_ETHER_H
#define LINUX_IF_ETHER_H

#define ETH_ALEN      6
#define ETH_TLEN      2
#define ETH_HLEN      14
#define ETH_ZLEN      60
#define ETH_DATA_LEN  1500
#define ETH_FRAME_LEN 1514
#define ETH_FCS_LEN   4
#define ETH_P_IP      0x0800
#define ETH_P_ARP     0x0806
#define ETH_P_IPV6    0x86DD
#define ETH_P_PAE     0x888E        /* EAPOL: Wi-Fi's WPA handshake frames */
#define ETH_P_TDLS    0x890D
#define ETH_P_80221   0x8917
#define ETH_P_AARP    0x80F3
#define ETH_P_IPX     0x8137
#define ETH_P_802_3   0x0001
#define ETH_P_ALL     0x0003
#define ETH_P_802_2   0x0004
#define ETH_P_PREAUTH 0x88C7
#define ETH_P_8021Q   0x8100
#define ETH_P_8021AD  0x88A8
#define ETH_P_LLDP    0x88CC
#define ARPHRD_ETHER        1
#define ARPHRD_IEEE80211    801
#define ARPHRD_IEEE80211_RADIOTAP 803
#define ARPHRD_LOOPBACK     772

#endif
