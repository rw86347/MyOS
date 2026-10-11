/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * scan.c — finding networks, and the list of the ones found (Linux:
 * mac80211's scan.c and cfg80211's scan.c).
 *
 * Scans are the driver's: hw_scan hands it the channels, the SSIDs to ask
 * for and the elements for its probe requests, and the radio (iwlwifi's
 * firmware, or the simulated radio) does the channel hopping itself. The
 * beacons and probe responses it hears come up as ordinary received
 * frames and land in the table here; ieee80211_scan_completed ends it.
 *
 * The table is a fixed array of WIFI_BSS_MAX networks per radio: an update
 * looks a network up by BSSID and channel (a bounded walk), and when the
 * table is full the network heard least recently makes room.
 */
#include "core.h"

/* --- starting a scan ------------------------------------------------------------------------ */

/* The elements of a probe request on one band: the rates we speak (8 in
   Supported Rates, the rest in Extended Supported Rates), and HT/VHT
   abilities where the band has them. Returns the length, at most room. */
static size_t band_ies(u8 *p, size_t room, const struct ieee80211_supported_band *sband)
{
    u8 rates[32];
    int n = 0;
    for (int i = 0; i < sband->n_bitrates && n < (int)sizeof rates; i++)
        rates[n++] = (u8)(sband->bitrates[i].bitrate / 5);    /* units of 500 kbit/s */

    size_t need = 2 + (size_t)min(n, 8) + (n > 8 ? 2 + (size_t)(n - 8) : 0) +
                  (sband->ht_cap.ht_supported ? 2 + sizeof(struct ieee80211_ht_cap) : 0) +
                  (sband->vht_cap.vht_supported ? 2 + sizeof(struct ieee80211_vht_cap) : 0);
    if (need > room)
        return 0;

    u8 *start = p;
    *p++ = WLAN_EID_SUPP_RATES;
    *p++ = (u8)min(n, 8);
    memcpy(p, rates, (size_t)min(n, 8));
    p += min(n, 8);
    if (n > 8) {
        *p++ = WLAN_EID_EXT_SUPP_RATES;
        *p++ = (u8)(n - 8);
        memcpy(p, rates + 8, (size_t)(n - 8));
        p += n - 8;
    }
    if (sband->ht_cap.ht_supported) {
        struct ieee80211_ht_cap ht = { 0 };
        ht.cap_info = cpu_to_le16(sband->ht_cap.cap);
        ht.ampdu_params_info = (u8)(sband->ht_cap.ampdu_factor |
                                    sband->ht_cap.ampdu_density << 2);
        ht.mcs = sband->ht_cap.mcs;
        *p++ = WLAN_EID_HT_CAPABILITY;
        *p++ = sizeof ht;
        memcpy(p, &ht, sizeof ht);
        p += sizeof ht;
    }
    if (sband->vht_cap.vht_supported) {
        struct ieee80211_vht_cap vht = { 0 };
        vht.vht_cap_info = cpu_to_le32(sband->vht_cap.cap);
        vht.supp_mcs = sband->vht_cap.vht_mcs;
        *p++ = WLAN_EID_VHT_CAPABILITY;
        *p++ = sizeof vht;
        memcpy(p, &vht, sizeof vht);
        p += sizeof vht;
    }
    return (size_t)(p - start);
}

#define BAND_IES_MAX 128                /* rates, HT and VHT fit easily */

/* Start a scan (the caller holds the wiphy lock). req was allocated with
   kmalloc; from here on the core owns it and frees it when the scan ends.
   On an error the caller still owns it. */
int wifi_scan(struct ieee80211_sub_if_data *sdata, struct cfg80211_scan_request *req)
{
    struct ieee80211_local *local = sdata->local;
    struct wiphy *wiphy = local->hw.wiphy;

    if (!local->ops->hw_scan)
        return -EOPNOTSUPP;             /* both our drivers scan in hardware */
    if (!sdata->in_driver) {
        /* Up, but the driver doesn't have it: a firmware restart that
           failed. Try again now, rather than refusing every scan forever. */
        if (!netif_running(sdata->dev) || wifi_driver_up(sdata))
            return -ENETDOWN;
        printk(KERN_INFO "wifi: %s: back in the driver\n", sdata->dev->name);
    }
    if (local->scan_req)
        return -EBUSY;
    if (!req->n_channels || req->n_ssids > wiphy->max_scan_ssids ||
        req->ie_len > wiphy->max_scan_ie_len)
        return -EINVAL;

    /* What cfg80211 fills in when nobody said: probe requests to the
       wildcard BSSID (the driver copies it into the frame; all zeros is a
       BSSID no AP answers), every rate of every band, no particular link
       to report times for. */
    if (is_zero_ether_addr(req->bssid))
        eth_broadcast_addr(req->bssid);
    for (int b = 0; b < NUM_NL80211_BANDS; b++)
        if (wiphy->bands[b] && !req->rates[b])
            req->rates[b] = (u32)((1ull << wiphy->bands[b]->n_bitrates) - 1);
    req->tsf_report_link_id = -1;

    size_t size = sizeof(struct ieee80211_scan_request) +
                  req->n_channels * sizeof(struct ieee80211_channel *) +
                  NUM_NL80211_BANDS * BAND_IES_MAX;
    struct ieee80211_scan_request *hw_req = kzalloc(size, GFP_KERNEL);
    if (!hw_req)
        return -ENOMEM;

    /* The driver's copy of the request, its channels, then the elements. */
    hw_req->req = *req;
    memcpy(hw_req->req.channels, req->channels, req->n_channels * sizeof(req->channels[0]));
    u8 *ies = (u8 *)&hw_req->req.channels[req->n_channels];
    for (int b = 0; b < NUM_NL80211_BANDS; b++) {
        if (!wiphy->bands[b])
            continue;
        hw_req->ies.ies[b] = ies;
        hw_req->ies.len[b] = band_ies(ies, BAND_IES_MAX, wiphy->bands[b]);
        ies += hw_req->ies.len[b];
    }
    hw_req->ies.common_ies = req->ie;   /* (the caller's, e.g. from wpa_supplicant) */
    hw_req->ies.common_ie_len = req->ie_len;
    hw_req->req.ie = NULL;
    hw_req->req.ie_len = 0;

    req->wiphy = wiphy;
    req->wdev = &sdata->wdev;
    req->scan_start = jiffies;
    hw_req->req.scan_start = req->scan_start;
    local->scan_req = req;
    local->hw_scan_req = hw_req;
    local->scan_sdata = sdata;
    reinit_completion(&local->scan_done);

    int err = local->ops->hw_scan(&local->hw, &sdata->vif, hw_req);
    if (err) {
        local->scan_req = NULL;
        local->hw_scan_req = NULL;
        local->scan_sdata = NULL;
        kfree(hw_req);
    }
    return err;
}

/* End the scan: free the driver's copy and the request, wake waiters.
   The wiphy lock is held. */
static void scan_finish(struct ieee80211_local *local)
{
    struct cfg80211_scan_request *req = local->scan_req;
    struct ieee80211_sub_if_data *sdata = local->scan_sdata;
    if (!req)
        return;                         /* (already finished: cancelled) */
    kfree(local->hw_scan_req);
    local->hw_scan_req = NULL;
    local->scan_req = NULL;
    local->scan_sdata = NULL;
    req->info = local->scan_info;
    req->notified = true;
    local->scan_generation++;
    nl80211_scan_done(sdata, req, req->info.aborted);
    wifi_mlme_scan_done(sdata, req->info.aborted);
    kfree(req);
    complete_all(&local->scan_done);
}

/* Taking the interface down: stop its scan and report it aborted. */
void wifi_scan_cancel(struct ieee80211_sub_if_data *sdata)
{
    struct ieee80211_local *local = sdata->local;
    if (local->scan_sdata != sdata)
        return;
    if (local->ops->cancel_hw_scan && !local->hw_lost)
        local->ops->cancel_hw_scan(&local->hw, &sdata->vif);
    local->scan_info.aborted = true;
    scan_finish(local);                 /* now: the lock is held, the driver has stopped */
}

/* The driver: the scan is over (any context). */
void ieee80211_scan_completed(struct ieee80211_hw *hw, struct cfg80211_scan_info *info)
{
    struct ieee80211_local *local = hw_to_local(hw);
    local->scan_info = *info;
    queue_work(system_wq, &local->scan_done_work);
}

void wifi_scan_done_work(struct work_struct *w)
{
    struct ieee80211_local *local = container_of(w, struct ieee80211_local, scan_done_work);
    wiphy_lock(local->hw.wiphy);
    scan_finish(local);
    wiphy_unlock(local->hw.wiphy);
}

void ieee80211_sched_scan_results(struct ieee80211_hw *hw) { (void)hw; }
void ieee80211_sched_scan_stopped(struct ieee80211_hw *hw) { (void)hw; }

/* --- the table of networks -------------------------------------------------------------------- */

/* The channel a beacon is really about: the one the AP names in its DS
   Parameter Set (2.4 GHz) or HT Operation element, which can differ from
   the one we heard it on (adjacent 2.4 GHz channels overlap). */
static struct ieee80211_channel *bss_channel(struct wiphy *wiphy, const u8 *ies, size_t len,
                                             struct ieee80211_channel *heard)
{
    const struct element *e = cfg80211_find_elem(WLAN_EID_DS_PARAMS, ies, (int)len);
    int chan = 0;
    if (e && e->datalen == 1)
        chan = e->data[0];
    else if ((e = cfg80211_find_elem(WLAN_EID_HT_OPERATION, ies, (int)len)) && e->datalen >= 1)
        chan = e->data[0];              /* (its first byte: the primary channel) */
    if (!chan)
        return heard;
    struct ieee80211_channel *c = ieee80211_get_channel_khz(wiphy,
        ieee80211_channel_to_freq_khz(chan, heard->band));
    return c ? c : heard;
}

void wifi_bss_update(struct ieee80211_local *local, struct sk_buff *skb)
{
    struct wiphy *wiphy = local->hw.wiphy;
    struct ieee80211_rx_status *st = IEEE80211_SKB_RXCB(skb);
    struct ieee80211_mgmt *mgmt = (void *)skb->data;
    size_t base = offsetof(struct ieee80211_mgmt, u.beacon.variable);
    if (skb->len < base)
        return;
    const u8 *ies = mgmt->u.beacon.variable;
    size_t ielen = skb->len - base;

    struct ieee80211_channel *heard = ieee80211_get_channel_khz(wiphy, ieee80211_rx_status_to_khz(st));
    if (!heard || (heard->flags & IEEE80211_CHAN_DISABLED))
        return;
    struct ieee80211_channel *chan = bss_channel(wiphy, ies, ielen, heard);

    /* This network's entry, or a free one, or the one heard least recently. */
    struct wifi_bss *slot = NULL, *oldest = NULL;
    for (int i = 0; i < WIFI_BSS_MAX; i++) {
        struct wifi_bss *b = &local->bss[i];
        if (!b->used) {
            if (!slot)
                slot = b;
            continue;
        }
        if (b->pub.channel == chan && ether_addr_equal(b->pub.bssid, mgmt->bssid)) {
            slot = b;
            break;
        }
        if (!oldest || time_before(b->last_seen, oldest->last_seen))
            oldest = b;
    }
    if (!slot)
        slot = oldest;

    struct cfg80211_bss_ies *new_ies = kmalloc(sizeof(*new_ies) + ielen, GFP_ATOMIC);
    if (!new_ies)
        return;
    new_ies->tsf = le64_to_cpu(mgmt->u.beacon.timestamp);
    new_ies->len = (int)ielen;
    new_ies->from_beacon = ieee80211_is_beacon(mgmt->frame_control);
    memcpy(new_ies->data, ies, ielen);

    if (slot->used && !ether_addr_equal(slot->pub.bssid, mgmt->bssid))
        slot->used = false;             /* (evicting another network) */
    if (slot->used)
        kfree((void *)slot->pub.ies);
    else
        memset(&slot->pub, 0, sizeof slot->pub);
    slot->pub.ies = new_ies;
    slot->pub.channel = chan;
    ether_addr_copy(slot->pub.bssid, mgmt->bssid);
    slot->pub.beacon_interval = le16_to_cpu(mgmt->u.beacon.beacon_int);
    slot->pub.capability = le16_to_cpu(mgmt->u.beacon.capab_info);
    slot->pub.signal = ieee80211_hw_check(&local->hw, SIGNAL_DBM) ? DBM_TO_MBM(st->signal) : 0;
    slot->pub.chains = st->chains;
    memcpy(slot->pub.chain_signal, st->chain_signal, sizeof slot->pub.chain_signal);
    slot->last_seen = jiffies;
    slot->device_ts = st->device_timestamp;
    slot->used = true;
    local->bss_generation++;
}

void wifi_bss_free_all(struct ieee80211_local *local)
{
    for (int i = 0; i < WIFI_BSS_MAX; i++)
        if (local->bss[i].used) {
            kfree((void *)local->bss[i].pub.ies);
            local->bss[i].used = false;
        }
}

/* Every network heard within the last WIFI_BSS_EXPIRE. */
void cfg80211_bss_iter(struct wiphy *wiphy, struct cfg80211_chan_def *chandef,
                       void (*iter)(struct wiphy *wiphy, struct cfg80211_bss *bss, void *data),
                       void *iter_data)
{
    struct ieee80211_local *local = wiphy_to_local(wiphy);
    for (int i = 0; i < WIFI_BSS_MAX; i++) {
        struct wifi_bss *b = &local->bss[i];
        if (!b->used || time_after(jiffies, b->last_seen + WIFI_BSS_EXPIRE))
            continue;
        if (chandef && b->pub.channel != chandef->chan)
            continue;
        iter(wiphy, &b->pub, iter_data);
    }
}

void cfg80211_bss_flush(struct wiphy *wiphy)
{
    wifi_bss_free_all(wiphy_to_local(wiphy));
}
