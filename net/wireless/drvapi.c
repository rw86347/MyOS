/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * drvapi.c — the rest of what a mac80211 driver may call (Linux's
 * include/net/mac80211.h), beyond receiving, sending and scanning (core.c,
 * data.c, scan.c): finding our peers and keys, losing the AP, a firmware
 * restart, and the calls of features we don't use.
 *
 * We are a station with one peer per interface: the AP. So "find the
 * station with this address" or "every station" is a look at that one
 * peer, not a search. Keys are the AP's pairwise key and up to four group
 * keys.
 *
 * Several calls belong to modes MyOS doesn't offer: being an access point
 * (beacons, sleeping clients, channel-switch announcements), TDLS, MU-MIMO
 * groups, remain-on-channel, block-ack sessions we never start, TKIP
 * (key.c refuses its keys). A driver calls them only after we asked for
 * that mode, so here they do nothing — each says why.
 */
#include "core.h"

/* --- our peers ---------------------------------------------------------------------- */

/* The peer the driver knows about (added: NONE and up), or NULL. */
static struct wifi_sta *ap_of(struct ieee80211_sub_if_data *sdata)
{
    struct wifi_sta *ap = sdata->conn.ap;
    return ap && ap->state >= IEEE80211_STA_NONE ? ap : NULL;
}

struct ieee80211_sta *ieee80211_find_sta(struct ieee80211_vif *vif, const u8 *addr)
{
    struct wifi_sta *ap = ap_of(vif_to_sdata(vif));
    return ap && ether_addr_equal(ap->sta.addr, addr) ? &ap->sta : NULL;
}

/* The peer `addr` of our interface `localaddr` (any interface if NULL). */
struct ieee80211_sta *ieee80211_find_sta_by_ifaddr(struct ieee80211_hw *hw, const u8 *addr,
                                                   const u8 *localaddr)
{
    struct ieee80211_sub_if_data *sdata;
    list_for_each_entry(sdata, &hw_to_local(hw)->interfaces, list) {
        if (localaddr && !ether_addr_equal(sdata->vif.addr, localaddr))
            continue;
        struct wifi_sta *ap = ap_of(sdata);
        if (ap && ether_addr_equal(ap->sta.addr, addr))
            return &ap->sta;
    }
    return NULL;
}

void ieee80211_iterate_stations_atomic(struct ieee80211_hw *hw,
                                       void (*iterator)(void *data, struct ieee80211_sta *sta),
                                       void *data)
{
    struct ieee80211_sub_if_data *sdata;
    list_for_each_entry(sdata, &hw_to_local(hw)->interfaces, list) {
        struct wifi_sta *ap = ap_of(sdata);
        if (ap)
            iterator(data, &ap->sta);
    }
}

/* One link: its aggregate limits are the link's own (Linux takes the
   smallest over a multi-link peer's links). */
void ieee80211_sta_recalc_aggregates(struct ieee80211_sta *pubsta)
{
    pubsta->cur = &pubsta->deflink.agg;
}

/* --- keys --------------------------------------------------------------------------------- */

static void keys_of(struct ieee80211_sub_if_data *sdata,
                    void (*iter)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                                 struct ieee80211_sta *sta, struct ieee80211_key_conf *key,
                                 void *data),
                    void *data)
{
    struct wifi_conn *c = &sdata->conn;
    struct ieee80211_hw *hw = &sdata->local->hw;
    if (c->ptk && c->ptk->in_driver && c->ap)
        iter(hw, &sdata->vif, &c->ap->sta, &c->ptk->conf, data);
    for (int i = 0; i < 4; i++)
        if (c->gtk[i] && c->gtk[i]->in_driver)
            iter(hw, &sdata->vif, NULL, &c->gtk[i]->conf, data);
}

/* Every key the driver holds: of one interface, or all (vif NULL). */
void ieee80211_iter_keys(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                         void (*iter)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                                      struct ieee80211_sta *sta, struct ieee80211_key_conf *key,
                                      void *data),
                         void *iter_data)
{
    if (vif) {
        keys_of(vif_to_sdata(vif), iter, iter_data);
        return;
    }
    struct ieee80211_sub_if_data *sdata;
    list_for_each_entry(sdata, &hw_to_local(hw)->interfaces, list)
        keys_of(sdata, iter, iter_data);
}

void ieee80211_iter_keys_rcu(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                             void (*iter)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                                          struct ieee80211_sta *sta, struct ieee80211_key_conf *key,
                                          void *data),
                             void *iter_data)
{
    ieee80211_iter_keys(hw, vif, iter, iter_data);
}

/* Where a key's receive counter starts (what wpa_supplicant told us), so
   the hardware can drop replays. The PN is kept most significant byte
   first, as the driver expects. */
void ieee80211_get_key_rx_seq(struct ieee80211_key_conf *keyconf, int tid, struct ieee80211_key_seq *seq)
{
    (void)tid;                          /* (one counter per key, not per TID) */
    const struct wifi_key *k = container_of(keyconf, struct wifi_key, conf);
    for (int i = 0; i < 6; i++)
        seq->ccmp.pn[i] = (u8)(k->rx_pn >> (8 * (5 - i)));
}

/* Counted by Linux for statistics; nothing reads them here. */
void ieee80211_key_replay(struct ieee80211_key_conf *keyconf) { (void)keyconf; }
void ieee80211_key_mic_failure(struct ieee80211_key_conf *keyconf) { (void)keyconf; }

/* TKIP: key.c never installs a TKIP key, so the driver never asks. */
void ieee80211_get_tkip_rx_p1k(struct ieee80211_key_conf *keyconf, const u8 *ta, u32 iv32, u16 *p1k)
{
    (void)keyconf; (void)ta; (void)iv32;
    WARN_ON_ONCE(1);
    memset(p1k, 0, 5 * sizeof(u16));
}

void ieee80211_get_tkip_p2k(struct ieee80211_key_conf *keyconf, struct sk_buff *skb, u8 *p2k)
{
    (void)keyconf; (void)skb;
    WARN_ON_ONCE(1);
    memset(p2k, 0, 16);
}

u8 *ieee80211_tkip_add_iv(u8 *pos, struct ieee80211_key_conf *keyconf, u64 pn)
{
    (void)keyconf; (void)pn;
    WARN_ON_ONCE(1);
    return pos;
}

/* --- losing the AP ------------------------------------------------------------------------ */

/* The firmware lost the connection: we leave, and wpa_supplicant looks
   for the network again. */
void ieee80211_connection_loss(struct ieee80211_vif *vif)
{
    wifi_connection_lost(vif_to_sdata(vif), WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY);
}

void ieee80211_disconnect(struct ieee80211_vif *vif, bool reconnect)
{
    (void)reconnect;                    /* (wpa_supplicant reconnects either way) */
    wifi_connection_lost(vif_to_sdata(vif), WLAN_REASON_UNSPECIFIED);
}

void ieee80211_hw_restart_disconnect(struct ieee80211_vif *vif)
{
    wifi_connection_lost(vif_to_sdata(vif), WLAN_REASON_UNSPECIFIED);
}

/* The AP moved channel and the driver couldn't follow: leave. */
void ieee80211_channel_switch_disconnect(struct ieee80211_vif *vif)
{
    wifi_connection_lost(vif_to_sdata(vif), WLAN_REASON_UNSPECIFIED);
}

/* Some beacons missed. Linux probes the AP first; we wait for the
   driver's connection_loss (iwlwifi calls it when many more are missed). */
void ieee80211_beacon_loss(struct ieee80211_vif *vif) { (void)vif; }

/* We never ask the driver to follow a channel switch (no channel_switch
   handling in our MLME), so it never reports one finished. */
void ieee80211_chswitch_done(struct ieee80211_vif *vif, bool success, unsigned int link_id)
{
    (void)vif; (void)success; (void)link_id;
}

/* Signal-quality events for wpa_supplicant's roaming (CQM): we accept
   SET_CQM but don't pass these on yet. */
void ieee80211_cqm_rssi_notify(struct ieee80211_vif *vif, enum nl80211_cqm_rssi_threshold_event rssi_event,
                               s32 rssi_level, gfp_t gfp)
{
    (void)vif; (void)rssi_event; (void)rssi_level; (void)gfp;
}

void ieee80211_cqm_beacon_loss_notify(struct ieee80211_vif *vif, gfp_t gfp) { (void)vif; (void)gfp; }

/* The driver's wish to save power with one receive chain: we don't send
   the SM power save action frame, so the AP keeps sending on all. */
void ieee80211_request_smps(struct ieee80211_vif *vif, unsigned int link_id, enum ieee80211_smps_mode smps_mode)
{
    (void)vif; (void)link_id; (void)smps_mode;
}

/* --- a firmware restart ---------------------------------------------------------------------- */

/* The firmware crashed and the driver has forgotten everything we told it;
   it asks to be started again. As mac80211 does it, the connection
   survives: the driver is started, gets its interfaces back (still marked
   as in the driver, so iwlwifi finds them and keeps its IDs), then each
   connection is told again (mlme.c: channel, link settings, the AP's peer)
   and its keys, and it carries on. Only a scan the firmware was running is
   lost. If the driver can't be brought back, the connections are dropped
   without telling it (it's lost), and wpa_supplicant hears it. */
void wifi_restart_work(struct work_struct *w)
{
    struct ieee80211_local *local = container_of(w, struct ieee80211_local, restart_work);
    struct ieee80211_hw *hw = &local->hw;
    struct ieee80211_sub_if_data *sdata;
    wiphy_lock(hw->wiphy);
    if (!local->started)
        goto out;
    local->hw_lost = true;              /* (the scan went with the firmware) */
    list_for_each_entry(sdata, &local->interfaces, list)
        wifi_scan_cancel(sdata);
    local->hw_lost = false;

    int err = local->ops->start(hw);
    list_for_each_entry(sdata, &local->interfaces, list)
        if (!err && sdata->in_driver)
            err = local->ops->add_interface(hw, &sdata->vif);
    if (!err)
        wifi_configure_filter(local);
    list_for_each_entry(sdata, &local->interfaces, list)
        if (!err && sdata->in_driver)
            err = wifi_mlme_replay(sdata);
    if (err) {
        printk(KERN_ERR "wifi: %s: the driver didn't come back after a firmware error (%d)\n",
               wiphy_name(hw->wiphy), err);
        local->hw_lost = true;
        list_for_each_entry(sdata, &local->interfaces, list) {
            wifi_disconnect(sdata, WLAN_REASON_UNSPECIFIED, true);
            sdata->in_driver = false;
        }
        local->hw_lost = false;
        local->started = false;         /* (a scan tries to bring it up again) */
        goto out;
    }
    list_for_each_entry(sdata, &local->interfaces, list)
        if (sdata->in_driver)
            wifi_keys_replay(sdata);
    if (local->ops->reconfig_complete)
        local->ops->reconfig_complete(hw, IEEE80211_RECONFIG_TYPE_RESTART);
    list_for_each_entry(sdata, &local->interfaces, list)
        if (sdata->in_driver)
            wifi_mlme_wake(sdata);
    printk(KERN_INFO "wifi: %s: restarted after a firmware error; connections kept\n",
           wiphy_name(hw->wiphy));
out:
    wiphy_unlock(hw->wiphy);
}

void ieee80211_restart_hw(struct ieee80211_hw *hw)
{
    queue_work(system_wq, &hw_to_local(hw)->restart_work);
}

/* --- frames ------------------------------------------------------------------------------ */

/* A sent frame's report with extra details (rates, timestamps): only the
   frame matters to us, freed as ieee80211_tx_status_skb does. */
void ieee80211_tx_status_ext(struct ieee80211_hw *hw, struct ieee80211_tx_status *status)
{
    if (status->skb)
        ieee80211_tx_status_skb(hw, status->skb);
}

/* --- VHT: how many streams at a channel width ------------------------------------------------ */

/* 802.11-2020 Table 9-273: from a VHT station's Supported Channel Width
   Set and Extended NSS BW Support, the streams it can use at 80 MHz (and
   below), 160 MHz and 80+80 MHz, in quarters of its Max VHT NSS (0: not
   at that width). Rows: width set 0-2; columns: extended support 0-3. */
static const u8 nss_quarters[3][4][3] = {
    { { 4, 0, 0 }, { 4, 2, 0 }, { 4, 2, 2 }, { 4, 3, 3 } },
    { { 4, 4, 0 }, { 4, 4, 2 }, { 4, 4, 3 }, { 8, 8, 4 } },
    { { 4, 4, 4 }, { 0, 0, 0 }, { 0, 0, 0 }, { 8, 4, 4 } },  /* (1, 2: reserved) */
};

int ieee80211_get_vht_max_nss(struct ieee80211_vht_cap *cap, enum ieee80211_vht_chanwidth bw, int mcs,
                              bool ext_nss_bw_capable, unsigned int max_vht_nss)
{
    u32 info = le32_to_cpu(cap->vht_cap_info);
    int width = (info & IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_MASK) >> IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_SHIFT;
    int ext = ext_nss_bw_capable ? (int)((info & IEEE80211_VHT_CAP_EXT_NSS_BW_MASK) >>
                                         IEEE80211_VHT_CAP_EXT_NSS_BW_SHIFT) : 0;
    if (!max_vht_nss) {
        /* Not given: the most streams whose MCS set reaches `mcs`
           (2 bits per stream: 0 = MCS 0-7, 1 = 0-8, 2 = 0-9, 3 = none). */
        u16 map = le16_to_cpu(cap->supp_mcs.rx_mcs_map);
        int need = mcs <= 7 ? 0 : mcs == 8 ? 1 : 2;
        for (int i = 7; i >= 0 && !max_vht_nss; i--) {
            int s = (map >> (2 * i)) & 3;
            if (s != 3 && s >= need)
                max_vht_nss = (unsigned int)i + 1;
        }
    }
    if (width > 2)
        return 0;
    int col = bw == IEEE80211_VHT_CHANWIDTH_160MHZ ? 1 : bw == IEEE80211_VHT_CHANWIDTH_80P80MHZ ? 2 : 0;
    return (int)(nss_quarters[width][ext][col] * max_vht_nss / 4);
}

/* --- modes we don't offer -------------------------------------------------------------------- */

/* Access point: beacons, channel-switch countdowns, sleeping clients.
   Our interfaces are stations, so the driver has no beacon to ask for. */
struct sk_buff *ieee80211_beacon_get_template(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                                              struct ieee80211_mutable_offsets *offs, unsigned int link_id)
{
    (void)hw; (void)vif; (void)offs; (void)link_id;
    return NULL;
}

u8 ieee80211_beacon_update_cntdwn(struct ieee80211_vif *vif, unsigned int link_id)
{
    (void)vif; (void)link_id;
    return 0;
}

void ieee80211_beacon_set_cntdwn(struct ieee80211_vif *vif, u8 counter) { (void)vif; (void)counter; }

bool ieee80211_beacon_cntdwn_is_complete(struct ieee80211_vif *vif, unsigned int link_id)
{
    (void)vif; (void)link_id;
    return false;
}

void ieee80211_csa_finish(struct ieee80211_vif *vif, unsigned int link_id) { (void)vif; (void)link_id; }

int ieee80211_sta_ps_transition(struct ieee80211_sta *sta, bool start)
{
    (void)sta; (void)start;
    return 0;
}

void ieee80211_sta_pspoll(struct ieee80211_sta *sta) { (void)sta; }
void ieee80211_sta_uapsd_trigger(struct ieee80211_sta *sta, u8 tid) { (void)sta; (void)tid; }
void ieee80211_sta_set_buffered(struct ieee80211_sta *sta, u8 tid, bool buffered)
{
    (void)sta; (void)tid; (void)buffered;
}
void ieee80211_sta_block_awake(struct ieee80211_hw *hw, struct ieee80211_sta *pubsta, bool block)
{
    (void)hw; (void)pubsta; (void)block;
}
void ieee80211_sta_eosp(struct ieee80211_sta *pubsta) { (void)pubsta; }
void ieee80211_send_eosp_nullfunc(struct ieee80211_sta *pubsta, int tid) { (void)pubsta; (void)tid; }

/* Block-ack sessions: we never start one (no ampdu_action calls), so the
   driver never reports one started or stopped. */
void ieee80211_start_tx_ba_cb_irqsafe(struct ieee80211_vif *vif, const u8 *ra, u16 tid)
{
    (void)vif; (void)ra; (void)tid;
}
void ieee80211_stop_tx_ba_cb_irqsafe(struct ieee80211_vif *vif, const u8 *ra, u16 tid)
{
    (void)vif; (void)ra; (void)tid;
}
void ieee80211_mark_rx_ba_filtered_frames(struct ieee80211_sta *pubsta, u8 tid, u16 ssn, u64 filtered,
                                          u16 received_mpdus)
{
    (void)pubsta; (void)tid; (void)ssn; (void)filtered; (void)received_mpdus;
}
void ieee80211_rx_ba_timer_expired(struct ieee80211_vif *vif, const u8 *addr, unsigned int tid)
{
    (void)vif; (void)addr; (void)tid;
}

/* TDLS (direct links between stations): not offered. */
int ieee80211_reserve_tid(struct ieee80211_sta *sta, u8 tid)
{
    (void)sta; (void)tid;
    return -EOPNOTSUPP;
}
void ieee80211_unreserve_tid(struct ieee80211_sta *sta, u8 tid) { (void)sta; (void)tid; }
void ieee80211_tdls_oper_request(struct ieee80211_vif *vif, const u8 *peer, enum nl80211_tdls_operation oper,
                                 u16 reason_code, gfp_t gfp)
{
    (void)vif; (void)peer; (void)oper; (void)reason_code; (void)gfp;
}

/* MU-MIMO groups (an AP's beamforming to several clients): informational. */
void ieee80211_update_mu_groups(struct ieee80211_vif *vif, unsigned int link_id, const u8 *membership,
                                const u8 *position)
{
    (void)vif; (void)link_id; (void)membership; (void)position;
}

/* Remain-on-channel (P2P, off-channel frames): we never ask for it. */
void ieee80211_ready_on_channel(struct ieee80211_hw *hw) { (void)hw; }
void ieee80211_remain_on_channel_expired(struct ieee80211_hw *hw) { (void)hw; }

/* cfg80211: peer measurements (Wi-Fi ranging) are never requested, and
   unprotected management frames matter only with 802.11w, which we don't
   offer. */
void cfg80211_pmsr_report(struct wireless_dev *wdev, struct cfg80211_pmsr_request *req,
                          struct cfg80211_pmsr_result *result, gfp_t gfp)
{
    (void)wdev; (void)req; (void)result; (void)gfp;
}
void cfg80211_pmsr_complete(struct wireless_dev *wdev, struct cfg80211_pmsr_request *req, gfp_t gfp)
{
    (void)wdev; (void)req; (void)gfp;
}
void cfg80211_rx_unprot_mlme_mgmt(struct net_device *dev, const u8 *buf, size_t len)
{
    (void)dev; (void)buf; (void)len;
}
