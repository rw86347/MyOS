/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * sim.c — a simulated Wi-Fi radio and the air around it, so the 802.11
 * core can be tested in QEMU, which has no Wi-Fi card (docs/WIFI-PLAN.md).
 * It is written to the same driver API as Intel's iwlwifi
 * (<net/mac80211.h>), so the core runs the same paths for both. Linux has
 * a much bigger cousin, mac80211_hwsim; this one is ours.
 *
 * The air holds a few access points (sim_aps). Scanning is the radio's
 * job, as on Intel cards (hw_scan): on each channel asked for, every AP
 * there "answers" with a probe response, handed up the way a card hands
 * up what it received (ieee80211_rx_irqsafe), at the AP's signal. Like a
 * real radio it stays SIM_DWELL_MS on each channel, so a scan takes a
 * while (17 channels: about 170 ms) — programs such as iw count on that:
 * iw starts listening for the "scan done" event only after asking.
 *
 * Built into `make test` kernels only.
 */
#include <kprintf.h>
#include <linux/etherdevice.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/mac80211.h>

struct sim_ap {
    u8 bssid[ETH_ALEN];
    const char *ssid;
    u16 freq;                           /* MHz */
    s8 signal;                          /* dBm, as we'd hear it */
    bool wpa2;                          /* WPA2-Personal (CCMP), or open */
};

/* The WPA2 network's passphrase (the test's wpa_supplicant.conf says it too). */
#define SIM_PASSPHRASE "myos-wifi-test"

/* sim_crypto.c */
void sim_hmac_sha1(const u8 *key, size_t klen, const u8 *const *part, const size_t *plen, int n,
                   u8 out[20]);
void sim_pbkdf2_sha1(const char *pass, const u8 *ssid, size_t ssid_len, u8 pmk[32]);
void sim_prf_sha1(const u8 *key, size_t klen, const char *label, const u8 *data, size_t dlen,
                  u8 *out, size_t n);
void sim_aes_wrap(const u8 kek[16], const u8 *plain, size_t n, u8 *out);

/* An AP's side of the WPA2 4-way handshake with us (IEEE 802.11-2020 12.7.6). */
struct sim_hs {
    int step;                           /* 0 idle, 1 sent message 1, 3 sent message 3, 4 done */
    u8 pmk[32];
    bool have_pmk;
    u8 anonce[32];
    u8 ptk[48];                         /* KCK | KEK | TK */
    u8 gtk[16];
    u64 replay;
    u8 sta[ETH_ALEN];
};

static const struct sim_ap sim_aps[] = {
    { { 0x02, 0x00, 0x00, 0x00, 0x0a, 0x01 }, "MyOS-Test",    2437, -42, true },    /* channel 6 */
    { { 0x02, 0x00, 0x00, 0x00, 0x0a, 0x02 }, "MyOS-Open-5G", 5180, -61, false },   /* channel 36 */
};

/* --- what the radio can do ----------------------------------------------------------- */

#define CHAN2G(n) { .center_freq = 2407 + 5 * (n), .hw_value = (n), .max_power = 20 }
#define CHAN5G(n) { .center_freq = 5000 + 5 * (n), .hw_value = (n), .max_power = 20 }
#define RATE(r, f) { .bitrate = (r), .flags = (f) }

static struct ieee80211_channel sim_chan_2g[] = {
    CHAN2G(1), CHAN2G(2), CHAN2G(3), CHAN2G(4), CHAN2G(5), CHAN2G(6), CHAN2G(7),
    CHAN2G(8), CHAN2G(9), CHAN2G(10), CHAN2G(11), CHAN2G(12), CHAN2G(13),
};
static struct ieee80211_channel sim_chan_5g[] = {
    CHAN5G(36), CHAN5G(40), CHAN5G(44), CHAN5G(48),
};
/* 802.11b's four, then OFDM's eight (2.4 GHz); OFDM alone at 5 GHz. */
static struct ieee80211_rate sim_rates[] = {
    RATE(10, 0), RATE(20, IEEE80211_RATE_SHORT_PREAMBLE), RATE(55, IEEE80211_RATE_SHORT_PREAMBLE),
    RATE(110, IEEE80211_RATE_SHORT_PREAMBLE),
    RATE(60, 0), RATE(90, 0), RATE(120, 0), RATE(180, 0), RATE(240, 0), RATE(360, 0),
    RATE(480, 0), RATE(540, 0),
};
static struct ieee80211_supported_band sim_band_2g = {
    .channels = sim_chan_2g, .n_channels = ARRAY_SIZE(sim_chan_2g),
    .bitrates = sim_rates, .n_bitrates = 12,
};
static struct ieee80211_supported_band sim_band_5g = {
    .channels = sim_chan_5g, .n_channels = ARRAY_SIZE(sim_chan_5g),
    .bitrates = sim_rates + 4, .n_bitrates = 8,
};

/* --- the driver's state -------------------------------------------------------------- */

#define SIM_DWELL_MS 10                 /* per channel (real active scans: 20-40) */

struct sim_priv {
    struct ieee80211_hw *hw;
    bool started;
    struct ieee80211_vif *vif;          /* the one station interface */
    struct ieee80211_scan_request *scan;    /* the scan being run */
    u32 scan_next;                      /* ...its next channel */
    struct delayed_work scan_work;
    int scans;                          /* (for the self-test) */
    struct ieee80211_channel *chan;     /* tuned to (the channel context) */
    struct sk_buff_head air;            /* frames from the APs, on their way to us */
    struct delayed_work air_work;
    const struct sim_ap *assoc;         /* the AP that has us associated */
    enum ieee80211_sta_state sta_state; /* of that AP, as the core told us */
    int heard_arp, heard_ping, heard_data;  /* (what the APs got from us: the self-test) */
    struct sim_hs hs[2];                /* per AP (sim_aps) */
    u8 aifs[IEEE80211_NUM_ACS];         /* conf_tx: what the core set (0: never) */
    bool bssid_told;                    /* the core announced a (non-zero) BSSID */
    int violations;                     /* broken rules (strict()) */
    u8 sta_tk[16], sta_gtk[4][16];      /* the keys the core installed (set_key): "the hardware's" */
    bool have_tk, have_gtk[4];
};

#define SIM_AIR_MS 2                    /* an AP's answer takes this long to arrive */

static struct ieee80211_hw *sim_hw;

/* A management frame from ap to `to`, with `fixed` bytes of fixed fields
   (zeroed) after the header. */
static struct sk_buff *ap_frame(const struct sim_ap *ap, const u8 *to, u16 stype, size_t fixed)
{
    struct sk_buff *skb = alloc_skb(320, GFP_ATOMIC);
    if (!skb)
        return NULL;
    memset(skb->cb, 0, sizeof skb->cb);
    struct ieee80211_mgmt *m = skb_put_zero(skb, 24 + (unsigned int)fixed);
    m->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | stype);
    ether_addr_copy(m->da, to);
    ether_addr_copy(m->sa, ap->bssid);
    ether_addr_copy(m->bssid, ap->bssid);
    return skb;
}

static u16 ap_capab(const struct sim_ap *ap)
{
    return WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_SHORT_SLOT_TIME |
           (ap->wpa2 ? WLAN_CAPABILITY_PRIVACY : 0);
}

/* The AP's rates (basic ones marked) and, at 2.4 GHz, its channel. */
static void ap_rates(struct sk_buff *skb, const struct sim_ap *ap)
{
    static const u8 rates_2g[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    static const u8 xrates_2g[] = { 0x30, 0x48, 0x60, 0x6c };
    static const u8 rates_5g[] = { 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c };
    bool g24 = ap->freq < 3000;
    u8 *p = skb_put(skb, 2 + 8);
    p[0] = WLAN_EID_SUPP_RATES;
    p[1] = 8;
    memcpy(p + 2, g24 ? rates_2g : rates_5g, 8);
    if (g24) {
        p = skb_put(skb, 3);
        p[0] = WLAN_EID_DS_PARAMS;
        p[1] = 1;
        p[2] = (u8)((ap->freq - 2407) / 5);
        p = skb_put(skb, 2 + sizeof xrates_2g);
        p[0] = WLAN_EID_EXT_SUPP_RATES;
        p[1] = sizeof xrates_2g;
        memcpy(p + 2, xrates_2g, sizeof xrates_2g);
    }
}

/* A probe response from ap to `to`, as the AP would send it. */
static struct sk_buff *probe_resp(const struct sim_ap *ap, const u8 *to)
{
    /* RSN: version 1, group CCMP, one pairwise CCMP, one AKM PSK, no capabilities. */
    static const u8 rsn[] = { 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
                              1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
    size_t ssid_len = strlen(ap->ssid);
    struct sk_buff *skb = ap_frame(ap, to, IEEE80211_STYPE_PROBE_RESP, 12);
    if (!skb)
        return NULL;
    struct ieee80211_mgmt *m = (void *)skb->data;
    m->u.probe_resp.timestamp = cpu_to_le64(jiffies * 1000);      /* the AP's clock, in us */
    m->u.probe_resp.beacon_int = cpu_to_le16(100);                /* TUs */
    m->u.probe_resp.capab_info = cpu_to_le16(ap_capab(ap));
    u8 *p = skb_put(skb, 2 + ssid_len);
    p[0] = WLAN_EID_SSID;
    p[1] = (u8)ssid_len;
    memcpy(p + 2, ap->ssid, ssid_len);
    ap_rates(skb, ap);
    if (ap->wpa2) {
        u8 *p = skb_put(skb, 2 + sizeof rsn);
        p[0] = WLAN_EID_RSN;
        p[1] = sizeof rsn;
        memcpy(p + 2, rsn, sizeof rsn);
    }
    return skb;
}

/* --- the air: what the APs send us, and what they make of our frames ------------- */

/* Send skb from ap to us: it arrives SIM_AIR_MS later, on ap's channel. */
static void air_send(struct sim_priv *sim, const struct sim_ap *ap, struct sk_buff *skb)
{
    struct ieee80211_rx_status *st = IEEE80211_SKB_RXCB(skb);
    u32 flag = st->flag & (RX_FLAG_DECRYPTED | RX_FLAG_IV_STRIPPED | RX_FLAG_MIC_STRIPPED |
                           RX_FLAG_PN_VALIDATED);
    memset(st, 0, sizeof *st);
    st->flag = flag;
    st->freq = ap->freq;
    st->band = ap->freq < 3000 ? NL80211_BAND_2GHZ : NL80211_BAND_5GHZ;
    st->signal = ap->signal;
    skb_queue_tail(&sim->air, skb);
    queue_delayed_work(system_wq, &sim->air_work, msecs_to_jiffies(SIM_AIR_MS));
}

static void air_work(struct work_struct *w)
{
    struct sim_priv *sim = container_of(to_delayed_work(w), struct sim_priv, air_work);
    struct sk_buff *skb;
    while ((skb = skb_dequeue(&sim->air))) {
        if (sim->started && sim->chan && sim->chan->center_freq == IEEE80211_SKB_RXCB(skb)->freq)
            ieee80211_rx_irqsafe(sim->hw, skb);
        else
            kfree_skb(skb);             /* (we left that channel meanwhile) */
    }
}

/* --- the network behind the AP: a router at .1 that leases .10 (W3b) ------------- */

#define AP_IP    0xc0a84d01u            /* 192.168.77.1: the AP itself */
#define LEASE_IP 0xc0a84d0au            /* 192.168.77.10: what its DHCP server hands out */

static void wr16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wr32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u16 rd16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 rd32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

static u16 inet_csum(const u8 *p, size_t n)
{
    u32 sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += rd16(p + i);
    if (n & 1)
        sum += (u32)p[n - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (u16)~sum;
}

/* An Ethernet-type payload from the AP to `to`, as a from-DS data frame. */
static void ap_send_data(struct sim_priv *sim, const struct sim_ap *ap, const u8 *to, u16 type,
                         const u8 *payload, size_t len)
{
    struct sk_buff *skb = alloc_skb(24 + 8 + (unsigned int)len, GFP_ATOMIC);
    if (!skb)
        return;
    memset(skb->cb, 0, sizeof skb->cb);
    struct ieee80211_hdr_3addr *h = skb_put_zero(skb, 24);
    h->frame_control = cpu_to_le16(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_DATA | IEEE80211_FCTL_FROMDS);
    struct sim_hs *hs = &sim->hs[ap - sim_aps];
    if (ap->wpa2 && hs->step == 4 && type != 0x888e) {
        /* Encrypted with the TK (unicast) or the GTK (group): our "hardware"
           decrypts it if it holds the same key, and hands it up stripped. */
        bool group = is_multicast_ether_addr(to);
        bool ok = group ? sim->have_gtk[1] && !memcmp(sim->sta_gtk[1], hs->gtk, 16)
                        : sim->have_tk && !memcmp(sim->sta_tk, hs->ptk + 32, 16);
        if (!ok) {
            kfree_skb(skb);
            return;
        }
        h->frame_control |= cpu_to_le16(IEEE80211_FCTL_PROTECTED);
        IEEE80211_SKB_RXCB(skb)->flag = RX_FLAG_DECRYPTED | RX_FLAG_IV_STRIPPED |
                                         RX_FLAG_MIC_STRIPPED | RX_FLAG_PN_VALIDATED;
    }
    ether_addr_copy(h->addr1, to);
    ether_addr_copy(h->addr2, ap->bssid);
    ether_addr_copy(h->addr3, ap->bssid);   /* (the AP is also the router: it sent it) */
    u8 *llc = skb_put(skb, 8);
    memcpy(llc, rfc1042_header, 6);
    wr16(llc + 6, type);
    skb_put_data(skb, payload, (unsigned int)len);
    air_send(sim, ap, skb);
}

/* An IPv4 packet from the AP (header built here; payload after it). */
static void ap_send_ip(struct sim_priv *sim, const struct sim_ap *ap, const u8 *to, u32 dst, u8 proto,
                       const u8 *payload, size_t len)
{
    u8 pkt[600];
    if (20 + len > sizeof pkt)
        return;
    memset(pkt, 0, 20);
    pkt[0] = 0x45;
    wr16(pkt + 2, (u16)(20 + len));
    pkt[8] = 64;                        /* TTL */
    pkt[9] = proto;
    wr32(pkt + 12, AP_IP);
    wr32(pkt + 16, dst);
    wr16(pkt + 10, inet_csum(pkt, 20));
    memcpy(pkt + 20, payload, len);
    ap_send_data(sim, ap, to, 0x0800, pkt, 20 + len);
}

/* DHCP: DISCOVER -> OFFER, REQUEST -> ACK, always for LEASE_IP, broadcast. */
static void ap_dhcp(struct sim_priv *sim, const struct sim_ap *ap, const u8 *bootp, size_t len)
{
    if (len < 240 || bootp[0] != 1 || rd32(bootp + 236) != 0x63825363)
        return;
    int type = 0;
    for (size_t o = 240; o + 1 < len && bootp[o] != 255; o += bootp[o] ? 2u + bootp[o + 1] : 1u)
        if (bootp[o] == 53 && o + 2 < len)
            type = bootp[o + 2];
    if (type != 1 && type != 3)
        return;
    u8 udp[8 + 300];
    memset(udp, 0, sizeof udp);
    wr16(udp, 67);
    wr16(udp + 2, 68);
    u8 *b = udp + 8;
    b[0] = 2;                           /* a reply */
    b[1] = 1;
    b[2] = 6;
    memcpy(b + 4, bootp + 4, 4);        /* xid */
    memcpy(b + 10, bootp + 10, 2);      /* flags */
    wr32(b + 16, LEASE_IP);             /* yiaddr */
    wr32(b + 20, AP_IP);                /* siaddr */
    memcpy(b + 28, bootp + 28, 16);     /* chaddr */
    wr32(b + 236, 0x63825363);
    u8 *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = type == 1 ? 2 : 5;        /* OFFER / ACK */
    *o++ = 54; *o++ = 4; wr32(o, AP_IP); o += 4;          /* server */
    *o++ = 51; *o++ = 4; wr32(o, 3600); o += 4;           /* lease: an hour */
    *o++ = 1; *o++ = 4; wr32(o, 0xffffff00u); o += 4;     /* netmask */
    *o++ = 3; *o++ = 4; wr32(o, AP_IP); o += 4;           /* router */
    *o++ = 6; *o++ = 4; wr32(o, AP_IP); o += 4;           /* DNS */
    *o++ = 255;
    size_t ulen = (size_t)(o - udp);
    wr16(udp + 4, (u16)ulen);           /* (UDP checksum 0: none, allowed over IPv4) */
    static const u8 bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    ap_send_ip(sim, ap, bcast, 0xffffffffu, 17, udp, ulen);
}

/* --- the AP's WPA2: the 4-way handshake and its keys (W3c) --------------------------- */

static const u8 ap_rsn_ie[] = { WLAN_EID_RSN, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
                                1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };

#define KI_VER2      0x0002             /* key info: HMAC-SHA1 MIC, AES key wrap */
#define KI_PAIRWISE  0x0008
#define KI_INSTALL   0x0040
#define KI_ACK       0x0080
#define KI_MIC       0x0100
#define KI_SECURE    0x0200
#define KI_ENCDATA   0x1000
#define EAPOL_FIXED  (4 + 95)           /* EAPOL header + the EAPOL-Key body up to its data */

/* An EAPOL-Key frame from the AP, its MIC filled in when the KCK is known. */
static void hs_send(struct sim_priv *sim, const struct sim_ap *ap, struct sim_hs *hs, u16 info,
                    const u8 *data, size_t dlen)
{
    u8 f[EAPOL_FIXED + 64];
    if (dlen > 64)
        return;
    memset(f, 0, EAPOL_FIXED);
    f[0] = 2;                           /* 802.1X-2004 */
    f[1] = 3;                           /* EAPOL-Key */
    wr16(f + 2, (u16)(95 + dlen));
    u8 *k = f + 4;
    k[0] = 2;                           /* RSN key descriptor */
    wr16(k + 1, info);
    wr16(k + 3, 16);                    /* CCMP: a 16-byte key */
    for (int i = 0; i < 8; i++)
        k[5 + i] = (u8)(hs->replay >> (56 - 8 * i));
    memcpy(k + 13, hs->anonce, 32);
    wr16(k + 93, (u16)dlen);
    memcpy(k + 95, data, dlen);
    if (info & KI_MIC) {
        const u8 *part = f;
        size_t plen = EAPOL_FIXED + dlen;
        u8 mic[20];
        sim_hmac_sha1(hs->ptk, 16, &part, &plen, 1, mic);
        memcpy(k + 77, mic, 16);
    }
    ap_send_data(sim, ap, hs->sta, 0x888e, f, EAPOL_FIXED + dlen);
}

/* After association: message 1 (our nonce). */
static void hs_start(struct sim_priv *sim, const struct sim_ap *ap, const u8 *sta)
{
    struct sim_hs *hs = &sim->hs[ap - sim_aps];
    if (!hs->have_pmk) {                /* (4096 rounds of HMAC-SHA1: once) */
        sim_pbkdf2_sha1(SIM_PASSPHRASE, (const u8 *)ap->ssid, strlen(ap->ssid), hs->pmk);
        hs->have_pmk = true;
    }
    ether_addr_copy(hs->sta, sta);
    get_random_bytes(hs->anonce, sizeof hs->anonce);
    get_random_bytes(hs->gtk, sizeof hs->gtk);
    hs->replay++;
    hs->step = 1;
    hs_send(sim, ap, hs, KI_VER2 | KI_PAIRWISE | KI_ACK, NULL, 0);
}

static int cmp(const u8 *a, const u8 *b, size_t n) { return memcmp(a, b, n); }

/* An EAPOL-Key frame from the station: message 2 or 4. */
static void hs_rx(struct sim_priv *sim, const struct sim_ap *ap, const u8 *f, size_t len)
{
    struct sim_hs *hs = &sim->hs[ap - sim_aps];
    if (len < EAPOL_FIXED || f[1] != 3 || f[4] != 2)
        return;
    const u8 *k = f + 4;
    u16 info = rd16(k + 1);
    size_t dlen = rd16(k + 93);
    if (!(info & KI_MIC) || EAPOL_FIXED + dlen > len)
        return;
    u64 replay = 0;
    for (int i = 0; i < 8; i++)
        replay = replay << 8 | k[5 + i];
    if (replay != hs->replay)
        return;

    if (hs->step == 1 && !(info & KI_SECURE)) {
        /* Message 2: their nonce. The keys: PRF-384 over both addresses and
           both nonces, each pair smaller first. */
        u8 d[2 * ETH_ALEN + 64];
        const u8 *aa = ap->bssid, *spa = hs->sta, *sn = k + 13;
        bool a_first = cmp(aa, spa, ETH_ALEN) < 0, n_first = cmp(hs->anonce, sn, 32) < 0;
        memcpy(d, a_first ? aa : spa, ETH_ALEN);
        memcpy(d + ETH_ALEN, a_first ? spa : aa, ETH_ALEN);
        memcpy(d + 12, n_first ? hs->anonce : sn, 32);
        memcpy(d + 44, n_first ? sn : hs->anonce, 32);
        sim_prf_sha1(hs->pmk, 32, "Pairwise key expansion", d, sizeof d, hs->ptk, 48);
    } else if (!(hs->step == 3 && (info & KI_SECURE))) {
        return;
    }
    /* Their MIC, over the frame with the MIC field zeroed. */
    u8 copy[EAPOL_FIXED + 64], mic[20];
    if (EAPOL_FIXED + dlen > sizeof copy)
        return;
    memcpy(copy, f, EAPOL_FIXED + dlen);
    memset(copy + 4 + 77, 0, 16);
    const u8 *part = copy;
    size_t plen = EAPOL_FIXED + dlen;
    sim_hmac_sha1(hs->ptk, 16, &part, &plen, 1, mic);
    if (memcmp(mic, k + 77, 16))
        return;                         /* wrong passphrase: the MIC can't match */

    if (hs->step == 1) {
        /* Message 3: install; our RSN element and the group key, wrapped
           with the KEK. */
        u8 plain[48], wrapped[56];
        memcpy(plain, ap_rsn_ie, sizeof ap_rsn_ie);                     /* 22 bytes */
        u8 *kde = plain + sizeof ap_rsn_ie;
        kde[0] = 0xdd;
        kde[1] = 22;
        kde[2] = 0x00; kde[3] = 0x0f; kde[4] = 0xac; kde[5] = 1;       /* GTK KDE */
        kde[6] = 1;                                                   /* key id 1 */
        kde[7] = 0;
        memcpy(kde + 8, hs->gtk, 16);
        plain[46] = 0xdd;                                             /* padding */
        plain[47] = 0;
        sim_aes_wrap(hs->ptk + 16, plain, 6, wrapped);
        hs->replay++;
        hs->step = 3;
        hs_send(sim, ap, hs, KI_VER2 | KI_PAIRWISE | KI_INSTALL | KI_ACK | KI_MIC | KI_SECURE |
                             KI_ENCDATA, wrapped, sizeof wrapped);
    } else {
        hs->step = 4;                   /* message 4: done, both sides have the keys */
    }
}

/* Would a protected frame between us and the AP decrypt? Only if the
   hardware (the core, through set_key) holds the key the AP derived. */
static bool pairwise_ok(struct sim_priv *sim, const struct sim_ap *ap)
{
    struct sim_hs *hs = &sim->hs[ap - sim_aps];
    return hs->step == 4 && sim->have_tk && !memcmp(sim->sta_tk, hs->ptk + 32, 16);
}

/* A data frame from us to the AP: ARP for the AP, DHCP, ping. */
static void ap_rx_data(struct sim_priv *sim, const struct sim_ap *ap, struct sk_buff *skb)
{
    const struct ieee80211_hdr *h = (const void *)skb->data;
    if (sim->assoc != ap || !ieee80211_has_tods(h->frame_control))
        return;
    if (ieee80211_has_protected(h->frame_control) ? !pairwise_ok(sim, ap) : ap->wpa2 && sim->hs[ap - sim_aps].step == 4)
        return;                         /* wrong key, or plain text on a protected link */
    unsigned int hl = ieee80211_hdrlen(h->frame_control);
    if (skb->len < hl + 8 || memcmp(skb->data + hl, rfc1042_header, 6))
        return;
    const u8 *from = h->addr2, *p = skb->data + hl + 8;
    size_t len = skb->len - hl - 8;
    u16 type = rd16(skb->data + hl + 6);
    sim->heard_data++;
    if (type == 0x888e && ap->wpa2) {
        hs_rx(sim, ap, p, len);
        return;
    }
    sim->heard_arp += type == 0x0806;

    if (type == 0x0806 && len >= 28 && rd16(p + 6) == 1 && rd32(p + 24) == AP_IP) {
        u8 a[28];                       /* ARP: "who has .1?" -> us */
        memcpy(a, p, 8);
        wr16(a + 6, 2);
        memcpy(a + 8, ap->bssid, 6);
        wr32(a + 14, AP_IP);
        memcpy(a + 18, p + 8, 10);      /* their MAC and IP */
        ap_send_data(sim, ap, from, 0x0806, a, sizeof a);
        return;
    }
    if (type != 0x0800 || len < 20 || (p[0] >> 4) != 4)
        return;
    size_t ihl = (size_t)(p[0] & 15) * 4, tot = rd16(p + 2);
    if (ihl < 20 || tot > len || tot < ihl)
        return;
    const u8 *l4 = p + ihl;
    size_t l4len = tot - ihl;
    if (p[9] == 17 && l4len >= 8 && rd16(l4 + 2) == 67) {
        ap_dhcp(sim, ap, l4 + 8, l4len - 8);
    } else if (p[9] == 1 && l4len >= 8 && l4[0] == 8 && rd32(p + 16) == AP_IP && l4len <= 500) {
        sim->heard_ping++;
        u8 icmp[500];                   /* ping -> pong */
        memcpy(icmp, l4, l4len);
        icmp[0] = 0;
        icmp[2] = icmp[3] = 0;
        wr16(icmp + 2, inet_csum(icmp, l4len));
        ap_send_ip(sim, ap, from, rd32(p + 12), 1, icmp, l4len);
    }
}

/* A frame we sent reaches ap: the AP's side of authentication and
   association, as hostapd would answer. */
static void ap_rx(struct sim_priv *sim, const struct sim_ap *ap, struct sk_buff *skb)
{
    const struct ieee80211_mgmt *m = (const void *)skb->data;
    __le16 fc = m->frame_control;
    struct sk_buff *r = NULL;
    if (ieee80211_is_data(fc)) {
        ap_rx_data(sim, ap, skb);
        return;
    }
    if (!ieee80211_is_mgmt(fc))
        return;
    if (ieee80211_is_auth(fc) && skb->len >= 30 && m->u.auth.auth_alg == 0 &&
        le16_to_cpu(m->u.auth.auth_transaction) == 1) {
        r = ap_frame(ap, m->sa, IEEE80211_STYPE_AUTH, 6);
        if (r) {
            struct ieee80211_mgmt *a = (void *)r->data;
            a->u.auth.auth_transaction = cpu_to_le16(2);    /* (open system, success) */
        }
    } else if ((ieee80211_is_assoc_req(fc) || ieee80211_is_reassoc_req(fc)) && skb->len >= 28) {
        size_t base = ieee80211_is_assoc_req(fc) ? offsetof(struct ieee80211_mgmt, u.assoc_req.variable)
                                                 : offsetof(struct ieee80211_mgmt, u.reassoc_req.variable);
        const struct element *ssid = skb->len > base ?
            cfg80211_find_elem(WLAN_EID_SSID, skb->data + base, (int)(skb->len - base)) : NULL;
        bool ok = ssid && ssid->datalen == strlen(ap->ssid) && !memcmp(ssid->data, ap->ssid, ssid->datalen);
        r = ap_frame(ap, m->sa, IEEE80211_STYPE_ASSOC_RESP, 6);
        if (r) {
            struct ieee80211_mgmt *a = (void *)r->data;
            a->u.assoc_resp.capab_info = cpu_to_le16(ap_capab(ap));
            a->u.assoc_resp.status_code = cpu_to_le16(ok ? 0 : WLAN_STATUS_UNSPECIFIED_FAILURE);
            a->u.assoc_resp.aid = cpu_to_le16(ok ? 0xc001 : 0);     /* AID 1 (the top two bits set) */
            ap_rates(r, ap);
            if (ok) {
                sim->assoc = ap;
                sim->hs[ap - sim_aps].step = 0;
                sim->have_tk = false;
            }
        }
    } else if (ieee80211_is_deauth(fc) || ieee80211_is_disassoc(fc)) {
        if (sim->assoc == ap)
            sim->assoc = NULL;
    }
    if (r)
        air_send(sim, ap, r);
    if (r && ap->wpa2 && sim->assoc == ap && (ieee80211_is_assoc_req(fc) || ieee80211_is_reassoc_req(fc)))
        hs_start(sim, ap, m->sa);       /* message 1 follows the association answer */
}

/* One channel of the scan: every AP there answers. Then the next channel
   after the dwell time, or the end. */
static void sim_scan_work(struct work_struct *w)
{
    struct sim_priv *sim = container_of(to_delayed_work(w), struct sim_priv, scan_work);
    struct ieee80211_scan_request *req = sim->scan;
    if (!req || !sim->vif)
        return;
    if (sim->scan_next < req->req.n_channels) {
        struct ieee80211_channel *ch = req->req.channels[sim->scan_next++];
        for (size_t a = 0; a < ARRAY_SIZE(sim_aps); a++) {
            const struct sim_ap *ap = &sim_aps[a];
            if (ap->freq != ch->center_freq)
                continue;
            /* An AP answers a probe request to the wildcard BSSID or to its
               own, and ignores the rest — all zeros included, which our
               scans sent until 2026-10-06 (docs/LOG.md). */
            if (!is_broadcast_ether_addr(req->req.bssid) && !ether_addr_equal(req->req.bssid, ap->bssid))
                continue;
            struct sk_buff *skb = probe_resp(ap, sim->vif->addr);
            if (!skb)
                continue;
            struct ieee80211_rx_status *st = IEEE80211_SKB_RXCB(skb);
            memset(st, 0, sizeof *st);
            st->freq = ch->center_freq;
            st->band = ch->band;
            st->signal = ap->signal;
            st->rate_idx = 0;
            ieee80211_rx_irqsafe(sim->hw, skb);
        }
        queue_delayed_work(system_wq, &sim->scan_work, msecs_to_jiffies(SIM_DWELL_MS));
        return;
    }
    sim->scan = NULL;
    sim->scans++;
    struct cfg80211_scan_info info = { .aborted = false };
    ieee80211_scan_completed(sim->hw, &info);
}

/* --- the callbacks -------------------------------------------------------------------- */

/* Send: the AP on our channel with that BSSID hears it (and acks it). */
static void sim_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control, struct sk_buff *skb)
{
    struct sim_priv *sim = hw->priv;
    struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
    const struct ieee80211_hdr *h = (const void *)skb->data;
    (void)control;
    for (size_t a = 0; skb->len >= 24 && sim->chan && a < ARRAY_SIZE(sim_aps); a++)
        if (sim_aps[a].freq == sim->chan->center_freq && ether_addr_equal(h->addr1, sim_aps[a].bssid)) {
            ap_rx(sim, &sim_aps[a], skb);
            info->flags |= IEEE80211_TX_STAT_ACK;
        }
    ieee80211_tx_status_skb(hw, skb);
}

static int sim_start(struct ieee80211_hw *hw)
{
    struct sim_priv *sim = hw->priv;
    sim->started = true;
    return 0;
}

static void sim_stop(struct ieee80211_hw *hw, bool suspend)
{
    struct sim_priv *sim = hw->priv;
    (void)suspend;
    cancel_delayed_work_sync(&sim->scan_work);
    cancel_delayed_work_sync(&sim->air_work);
    skb_queue_purge(&sim->air);
    sim->scan = NULL;
    sim->assoc = NULL;
    sim->have_tk = false;
    memset(sim->have_gtk, 0, sizeof sim->have_gtk);
    memset(sim->hs, 0, sizeof sim->hs);
    sim->started = false;
}

static int sim_add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
    struct sim_priv *sim = hw->priv;
    if (sim->vif || vif->type != NL80211_IFTYPE_STATION)
        return -EOPNOTSUPP;             /* one station interface */
    sim->vif = vif;
    return 0;
}

static void sim_remove_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
    struct sim_priv *sim = hw->priv;
    if (sim->vif == vif)
        sim->vif = NULL;
}

static int sim_config(struct ieee80211_hw *hw, int radio_idx, u32 changed)
{
    (void)hw; (void)radio_idx; (void)changed;
    return 0;
}

static void sim_configure_filter(struct ieee80211_hw *hw, unsigned int changed,
                                 unsigned int *total, u64 multicast)
{
    (void)hw; (void)changed; (void)multicast;
    *total &= FIF_BCN_PRBRESP_PROMISC | FIF_ALLMULTI | FIF_PROBE_REQ;
}

/* The rules Intel's firmware enforced on the MSI (docs/LOG.md, W7), which
   mac80211 keeps without saying and our core broke once each. This driver
   holds the core to them, so a self-test finds a break, not a laptop:
   "wifi sim: iwlwifi would fail" in the log fails `make test`. */
static void strict(struct sim_priv *sim, bool ok, const char *rule)
{
    if (ok)
        return;
    sim->violations++;
    printk(KERN_ERR "wifi sim: iwlwifi would fail: %s\n", rule);
}

static int sim_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                       struct ieee80211_scan_request *req)
{
    struct sim_priv *sim = hw->priv;
    strict(sim, !is_zero_ether_addr(req->req.bssid),
           "a scan's probe requests to BSSID 00:00:00:00:00:00 (the firmware asserted)");
    if (!sim->started || vif != sim->vif)
        return -EINVAL;
    if (sim->scan)
        return -EBUSY;
    sim->scan = req;
    sim->scan_next = 0;
    queue_delayed_work(system_wq, &sim->scan_work, 0);
    return 0;
}

static void sim_cancel_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
    struct sim_priv *sim = hw->priv;
    (void)vif;
    cancel_delayed_work_sync(&sim->scan_work);
    sim->scan = NULL;
}

/* Channel contexts: one radio, so the context assigned is the channel. */
static int sim_add_chanctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx)
{
    (void)hw; (void)ctx;
    return 0;
}

static void sim_remove_chanctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx)
{
    (void)hw; (void)ctx;
}

static void sim_change_chanctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx, u32 changed)
{
    struct sim_priv *sim = hw->priv;
    (void)changed;
    if (sim->chan)
        sim->chan = ctx->def.chan;
}

static int sim_assign_vif_chanctx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                                  struct ieee80211_bss_conf *link, struct ieee80211_chanctx_conf *ctx)
{
    struct sim_priv *sim = hw->priv;
    (void)vif;
    strict(sim, !is_zero_ether_addr(link->addr),
           "a link activated with address 00:00:00:00:00:00 (LINK_CONFIG_CMD asserted)");
    strict(sim, sim->aifs[IEEE80211_AC_BE] != 0,
           "a link activated before the EDCA timing was set (conf_tx; AIFS 0)");
    sim->chan = ctx->def.chan;
    return 0;
}

static void sim_unassign_vif_chanctx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                                     struct ieee80211_bss_conf *link, struct ieee80211_chanctx_conf *ctx)
{
    struct sim_priv *sim = hw->priv;
    (void)vif; (void)link; (void)ctx;
    sim->chan = NULL;
}

static int sim_sta_state(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_sta *sta,
                         enum ieee80211_sta_state old, enum ieee80211_sta_state new)
{
    struct sim_priv *sim = hw->priv;
    (void)vif; (void)sta;
    if (old == IEEE80211_STA_NOTEXIST && new == IEEE80211_STA_NONE) {
        strict(sim, sim->bssid_told, "the AP added as a peer before its BSSID was announced");
        strict(sim, sim->chan && sta->deflink.supp_rates[sim->chan->band],
               "the AP added as a peer with no rates (TLC_MNG_CONFIG_CMD asserted)");
    }
    sim->sta_state = new;
    return 0;
}

/* The access categories' timing (conf_tx): kept, for the self-test to
   see that the core never leaves them at zero, as iwlwifi's firmware
   would refuse. */
static int sim_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif, unsigned int link_id, u16 ac,
                       const struct ieee80211_tx_queue_params *params)
{
    struct sim_priv *sim = hw->priv;
    (void)vif; (void)link_id;
    if (ac >= IEEE80211_NUM_ACS)
        return -EINVAL;
    sim->aifs[ac] = params->aifs;
    return 0;
}

/* Keys for "the hardware": kept, compared with what the AP uses. */
static int sim_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd, struct ieee80211_vif *vif,
                       struct ieee80211_sta *sta, struct ieee80211_key_conf *key)
{
    struct sim_priv *sim = hw->priv;
    (void)vif; (void)sta;
    if (key->cipher != WLAN_CIPHER_SUITE_CCMP || key->keylen != 16 || key->keyidx > 3)
        return -EOPNOTSUPP;
    bool on = cmd == SET_KEY;
    if (key->flags & IEEE80211_KEY_FLAG_PAIRWISE) {
        memcpy(sim->sta_tk, key->key, 16);
        sim->have_tk = on;
    } else {
        memcpy(sim->sta_gtk[key->keyidx], key->key, 16);
        sim->have_gtk[key->keyidx] = on;
    }
    key->hw_key_idx = (u8)key->keyidx;
    return 0;
}

static void sim_bss_info_changed(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                                 struct ieee80211_bss_conf *info, u64 changed)
{
    struct sim_priv *sim = hw->priv;
    strict(sim, info->bssid != NULL, "link_conf->bssid is NULL (iwlwifi copies it: a NULL read)");
    strict(sim, vif->cfg.assoc || !(changed & (BSS_CHANGED_ERP_SLOT | BSS_CHANGED_ERP_PREAMBLE |
                                              BSS_CHANGED_ERP_CTS_PROT)),
           "slot/preamble changed before association (a LINK_CONFIG_CMD the firmware rejected)");
    if ((changed & BSS_CHANGED_BSSID) && info->bssid)
        sim->bssid_told = !is_zero_ether_addr(info->bssid);
}

static const struct ieee80211_ops sim_ops = {
    .tx = sim_tx,
    .start = sim_start,
    .stop = sim_stop,
    .add_interface = sim_add_interface,
    .remove_interface = sim_remove_interface,
    .config = sim_config,
    .configure_filter = sim_configure_filter,
    .hw_scan = sim_hw_scan,
    .cancel_hw_scan = sim_cancel_hw_scan,
    .add_chanctx = sim_add_chanctx,
    .remove_chanctx = sim_remove_chanctx,
    .change_chanctx = sim_change_chanctx,
    .assign_vif_chanctx = sim_assign_vif_chanctx,
    .unassign_vif_chanctx = sim_unassign_vif_chanctx,
    .sta_state = sim_sta_state,
    .bss_info_changed = sim_bss_info_changed,
    .conf_tx = sim_conf_tx,
    .set_key = sim_set_key,
};

/* --- bringing it up ------------------------------------------------------------------- */

int sim_wifi_init(void)
{
    static const u8 addr[ETH_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    struct ieee80211_hw *hw = ieee80211_alloc_hw(sizeof(struct sim_priv), &sim_ops);
    if (!hw)
        return -ENOMEM;
    struct sim_priv *sim = hw->priv;
    sim->hw = hw;
    INIT_DELAYED_WORK(&sim->scan_work, sim_scan_work);
    INIT_DELAYED_WORK(&sim->air_work, air_work);
    skb_queue_head_init(&sim->air);

    ieee80211_hw_set(hw, SIGNAL_DBM);
    hw->wiphy->bands[NL80211_BAND_2GHZ] = &sim_band_2g;
    hw->wiphy->bands[NL80211_BAND_5GHZ] = &sim_band_5g;
    hw->wiphy->max_scan_ssids = 4;
    static const u32 ciphers[] = { WLAN_CIPHER_SUITE_CCMP };
    hw->wiphy->cipher_suites = ciphers;
    hw->wiphy->n_cipher_suites = ARRAY_SIZE(ciphers);
    hw->queues = IEEE80211_NUM_ACS;     /* one per access category, as Intel's */
    SET_IEEE80211_PERM_ADDR(hw, addr);

    int err = ieee80211_register_hw(hw);
    if (err) {
        ieee80211_free_hw(hw);
        return err;
    }
    sim_hw = hw;
    return 0;
}

/* (For the self-test.) */
int sim_wifi_scans(void)
{
    return sim_hw ? ((struct sim_priv *)sim_hw->priv)->scans : -1;
}

/* A firmware crash, as iwlwifi has one: the "firmware" forgets the
   interface, the peer, the keys, the channel and the queue timing, and the driver asks the
   core to start it again (ieee80211_restart_hw). The AP knows nothing of
   it: we are still associated there. (The self-test of the restart.) */
void sim_wifi_crash(void)
{
    struct sim_priv *sim = sim_hw ? sim_hw->priv : NULL;
    if (!sim)
        return;
    sim->sta_state = IEEE80211_STA_NOTEXIST;
    sim->chan = NULL;
    sim->vif = NULL;                    /* (the interface too: it is added again) */
    sim->bssid_told = false;
    sim->have_tk = false;
    memset(sim->have_gtk, 0, sizeof sim->have_gtk);
    memset(sim->aifs, 0, sizeof sim->aifs);
    ieee80211_restart_hw(sim_hw);
}

/* How many of the rules above the core broke (0: none). */
int sim_wifi_violations(void)
{
    return sim_hw ? ((struct sim_priv *)sim_hw->priv)->violations : -1;
}

/* The AIFS the core gave an access category (0: never set). */
int sim_wifi_aifs(int ac)
{
    return sim_hw ? ((struct sim_priv *)sim_hw->priv)->aifs[ac] : -1;
}

bool sim_wifi_started(void)
{
    return sim_hw && ((struct sim_priv *)sim_hw->priv)->started;
}

/* Which AP has us associated (NULL: none), and the AP peer's state as the
   core last told the driver. */
const char *sim_wifi_assoc(void)
{
    struct sim_priv *sim = sim_hw ? sim_hw->priv : NULL;
    return sim && sim->assoc ? sim->assoc->ssid : NULL;
}

int sim_wifi_sta_state(void)
{
    return sim_hw ? (int)((struct sim_priv *)sim_hw->priv)->sta_state : -1;
}

void sim_wifi_heard(int *data, int *arp, int *ping)
{
    struct sim_priv *sim = sim_hw ? sim_hw->priv : NULL;
    *data = sim ? sim->heard_data : -1;
    *arp = sim ? sim->heard_arp : -1;
    *ping = sim ? sim->heard_ping : -1;
}
