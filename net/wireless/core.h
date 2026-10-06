/*
 * net/wireless/core.h — inside MyOS's 802.11 core (docs/WIFI-PLAN.md).
 *
 * Linux splits this work in two: cfg80211 (configuration, the list of
 * networks, nl80211) and mac80211 (the frame-level stack under "soft MAC"
 * drivers). We have only soft-MAC drivers, so it is one core with one set
 * of structures, and the cfg80211 half calls the mac80211 half directly.
 * Drivers see only <net/mac80211.h> and <net/cfg80211.h>.
 *
 * Memory, per radio, in one block:
 *
 *   struct wiphy | struct ieee80211_local | the driver's private area
 *                  (wiphy->priv)            (hw.priv, 32-byte aligned)
 *
 * Each interface (wlan0) is a net_device whose private area is a
 * struct ieee80211_sub_if_data, ending in the vif and the driver's
 * per-vif area.
 *
 * Who runs what: driver callbacks that may sleep (most of them: iwlwifi
 * waits for its firmware) run in threads with the wiphy mutex held — the
 * syscalls that configure Wi-Fi, and work items. Received frames are
 * handled on a work thread too. Threads are never preempted inside the
 * kernel, so a stretch of code that doesn't sleep needs no other lock;
 * interrupt handlers (the driver's) only queue things for these threads.
 */
#ifndef NET_WIRELESS_CORE_H
#define NET_WIRELESS_CORE_H

#include <linux/completion.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>

#define WIFI_MAX_RADIOS     2       /* phy0, phy1 */
#define WIFI_BSS_MAX        64      /* networks remembered per radio */
#define WIFI_BSS_EXPIRE     (30 * HZ)   /* not heard for 30 s: forgotten */

/* A network a scan found (Linux: struct cfg80211_internal_bss). */
struct wifi_bss {
    bool used;
    unsigned long last_seen;        /* jiffies */
    u32 device_ts;                  /* the card's clock when the frame came (rx_status) */
    struct cfg80211_bss pub;        /* last: it ends in priv[] (unused) */
};

struct ieee80211_sub_if_data;

/* A transmit queue: frames for one peer and TID, which the driver pulls
   (Linux: struct txq_info). */
struct wifi_txq {
    struct sk_buff_head frames;
    struct ieee80211_txq txq;       /* last: it ends in drv_priv[] */
};

/* A peer: as a station, our access point (Linux: struct sta_info). */
struct wifi_sta {
    struct ieee80211_sub_if_data *sdata;
    enum ieee80211_sta_state state; /* as far as the driver knows */
    struct wifi_txq *txq[IEEE80211_NUM_TIDS + 1];
    u64 rx_bytes, tx_bytes;
    u32 rx_packets, tx_packets;
    s8 last_signal;
    unsigned long last_rx, connected_at;    /* jiffies */
    struct list_head dead;          /* (dropped while the driver was lost: see sta_free) */
    struct ieee80211_sta sta;       /* last: it ends in drv_priv[] */
};

/* A key the hardware encrypts with (Linux: struct ieee80211_key). */
struct wifi_key {
    bool pairwise;
    bool in_driver;                 /* set_key accepted it */
    u64 rx_pn;                      /* (group keys: where the AP said its counter was) */
    struct ieee80211_key_conf conf; /* last: it ends in key[] */
};

/* A channel the radio is tuned to (Linux: struct ieee80211_chanctx). */
struct wifi_chanctx {
    struct ieee80211_chanctx_conf conf;     /* last: it ends in drv_priv[] */
};

/* A station interface's connection (Linux: mac80211's ifmgd and
   cfg80211's SME, in one). */
enum wifi_mlme { MLME_IDLE, MLME_SCAN, MLME_AUTH, MLME_ASSOC, MLME_CONNECTED };

struct wifi_conn {
    enum wifi_mlme state;
    u8 ssid[IEEE80211_MAX_SSID_LEN];
    u8 ssid_len;
    u8 bssid[ETH_ALEN];             /* asked for (if bssid_fixed), then chosen */
    bool bssid_fixed;
    u32 freq;                       /* asked for; 0: any */
    u8 *ie;                         /* elements to add to the association request */
    size_t ie_len;
    struct ieee80211_channel *chan;
    u16 capability, beacon_int;
    u8 dtim_period;
    u32 basic_rates;                /* bitmap into the band's bitrates */
    int tries;                      /* of the current frame */
    struct delayed_work timeout;
    struct wifi_sta *ap;
    struct wifi_chanctx *ctx;
    u8 *req_ies, *resp_ies;         /* for the CONNECT event */
    size_t req_ies_len, resp_ies_len;
    bool privacy;                   /* RSN asked for: keys come from wpa_supplicant */
    bool control_port;              /* only EAPOL until wpa_supplicant authorizes */
    bool control_port_no_encrypt;
    bool control_port_over_nl80211; /* EAPOL to and from user space over nl80211 */
    u16 control_port_ethertype;
    u32 owner;                      /* the netlink port that connected (SOCKET_OWNER) */
    u16 seq;                        /* our next management frame's sequence number */
    struct wifi_key *ptk;           /* the pairwise key (frames to and from the AP) */
    struct wifi_key *gtk[4];        /* group keys, by index (the AP's broadcasts) */
};

/* One radio (Linux: struct ieee80211_local + struct cfg80211_registered_device). */
struct ieee80211_local {
    struct ieee80211_hw hw;
    const struct ieee80211_ops *ops;
    int index;                      /* phyN */
    bool registered;
    bool started;                   /* ops->start succeeded */
    /* The firmware crashed (ieee80211_restart_hw): the driver has forgotten
       our interfaces, peers, keys and channel, so taking them down must not
       tell it; then it is started again and gets the interfaces back. */
    bool hw_lost;
    struct work_struct restart_work;
    struct list_head dead_stas;     /* peers dropped while hw_lost: freed with the radio */
    int open_count;                 /* interfaces up */
    struct list_head interfaces;    /* ieee80211_sub_if_data */

    /* Frames from ieee80211_rx_irqsafe, handled by rx_work. */
    struct sk_buff_head rx_queue;
    struct work_struct rx_work;

    /* The scan in progress (both NULL when none). */
    struct cfg80211_scan_request *scan_req;
    struct ieee80211_scan_request *hw_scan_req;
    struct ieee80211_sub_if_data *scan_sdata;
    struct cfg80211_scan_info scan_info;
    struct work_struct scan_done_work;
    struct completion scan_done;    /* (for whoever waits: the self-test) */
    unsigned int scan_generation;   /* finished scans, ever */

    /* wiphy_work items, run in order with the wiphy lock held. */
    struct list_head wiphy_work_list;
    struct work_struct wiphy_work;

    struct wifi_bss bss[WIFI_BSS_MAX];
    u32 bss_generation;             /* changes whenever the table does (nl80211) */
    u32 wdev_ids;                   /* the last interface identifier handed out */

    u8 drv_priv[] __aligned(32);    /* hw.priv */
};

/* One interface (Linux: struct ieee80211_sub_if_data). */
struct ieee80211_sub_if_data {
    struct list_head list;
    struct ieee80211_local *local;
    struct net_device *dev;
    struct wireless_dev wdev;
    bool in_driver;                 /* add_interface done */
    struct wifi_conn conn;          /* (station interfaces) */
    struct sk_buff_head mlme_queue; /* received management frames for the MLME */
    struct wiphy_work mlme_work;
    struct wiphy_work lost_work;    /* the driver lost the AP (beacons, firmware) */
    u8 link_bssid[ETH_ALEN];        /* what bss_conf.bssid points at, always (zeros:
                                       none): iwlwifi copies it on every change */
    u16 lost_reason;
    struct ieee80211_vif vif;       /* last: it ends in drv_priv[] */
};

/* 802.1D priority (TID 0-7) -> WMM access category. */
static inline u8 tid_ac(int tid)
{
    static const u8 ac[8] = { IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_BK, IEEE80211_AC_BE,
                              IEEE80211_AC_VI, IEEE80211_AC_VI, IEEE80211_AC_VO, IEEE80211_AC_VO };
    return ac[tid & 7];
}

static inline struct ieee80211_local *hw_to_local(struct ieee80211_hw *hw)
{
    return container_of(hw, struct ieee80211_local, hw);
}
static inline struct ieee80211_local *wiphy_to_local(struct wiphy *wiphy)
{
    return wiphy_priv(wiphy);
}
static inline struct ieee80211_sub_if_data *vif_to_sdata(struct ieee80211_vif *vif)
{
    return container_of(vif, struct ieee80211_sub_if_data, vif);
}

/* core.c */
struct ieee80211_local *wifi_radio(int index);  /* NULL if none */
struct ieee80211_sub_if_data *wifi_sdata_by_dev(struct net_device *dev);

/* scan.c */
int wifi_scan(struct ieee80211_sub_if_data *sdata, struct cfg80211_scan_request *req);
void wifi_scan_cancel(struct ieee80211_sub_if_data *sdata);
void wifi_scan_done_work(struct work_struct *w);
void wifi_bss_update(struct ieee80211_local *local, struct sk_buff *skb);
void wifi_bss_free_all(struct ieee80211_local *local);

/* data.c */
int wifi_xmit(struct sk_buff *skb, struct net_device *dev);
void wifi_rx_data(struct ieee80211_local *local, struct sk_buff *skb);

/* mlme.c */
struct wifi_connect_params {
    const u8 *ssid;
    u8 ssid_len;
    const u8 *bssid;                /* NULL: any */
    u32 freq;                       /* 0: any */
    const u8 *ie;
    size_t ie_len;
    bool privacy, control_port, control_port_no_encrypt, control_port_over_nl80211;
    u16 control_port_ethertype;
    u32 owner;
};
int wifi_mlme_init(struct ieee80211_sub_if_data *sdata);
void wifi_mlme_free(struct ieee80211_sub_if_data *sdata);
int wifi_connect(struct ieee80211_sub_if_data *sdata, const struct wifi_connect_params *p);
int wifi_disconnect(struct ieee80211_sub_if_data *sdata, u16 reason, bool locally);
void wifi_mlme_rx(struct ieee80211_sub_if_data *sdata, struct sk_buff *skb);
void wifi_mlme_scan_done(struct ieee80211_sub_if_data *sdata, bool aborted);
void wifi_bss_changed(struct ieee80211_sub_if_data *sdata, u64 changed);
struct ieee80211_sub_if_data *wifi_sdata_by_addr(struct ieee80211_local *local, const u8 *addr);
const struct cfg80211_bss *wifi_bss_find(struct ieee80211_local *local, const u8 *ssid, u8 ssid_len,
                                         const u8 *bssid, u32 freq);

int wifi_authorize(struct ieee80211_sub_if_data *sdata, bool on);
void wifi_connection_lost(struct ieee80211_sub_if_data *sdata, u16 reason);  /* any context */
int wifi_driver_up(struct ieee80211_sub_if_data *sdata);
int wifi_mlme_replay(struct ieee80211_sub_if_data *sdata);
void wifi_mlme_wake(struct ieee80211_sub_if_data *sdata);
void wifi_keys_replay(struct ieee80211_sub_if_data *sdata);
void wifi_dead_stas_free(struct ieee80211_local *local);
void wifi_default_edca(struct ieee80211_sub_if_data *sdata, bool qos);
void wifi_restart_work(struct work_struct *w);

/* key.c */
int wifi_key_add(struct ieee80211_sub_if_data *sdata, const u8 *mac, int idx, u32 cipher,
                 const u8 *key, size_t keylen, const u8 *seq, size_t seqlen);
int wifi_key_del(struct ieee80211_sub_if_data *sdata, const u8 *mac, int idx);
void wifi_keys_free(struct ieee80211_sub_if_data *sdata);
bool wifi_cipher_ok(struct wiphy *wiphy, u32 cipher);
int wifi_key_protect(struct ieee80211_sub_if_data *sdata, struct sk_buff *skb, unsigned int hdrlen);
const struct ieee80211_key_conf *wifi_key_for_rx(struct ieee80211_sub_if_data *sdata, bool group,
                                                 int idx);

/* nl80211.c */
void nl80211_connect_result(struct ieee80211_sub_if_data *sdata, u16 status, bool timed_out);
void nl80211_disconnected(struct ieee80211_sub_if_data *sdata, u16 reason, bool by_ap);
void nl80211_scan_done(struct ieee80211_sub_if_data *sdata, const struct cfg80211_scan_request *req,
                       bool aborted);

#endif
