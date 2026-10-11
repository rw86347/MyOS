/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * util.c — 802.11 helpers the core and drivers share (Linux has them in
 * net/wireless/util.c): header lengths, channel numbers <-> frequencies,
 * finding and ordering elements. All bounded by their input.
 */
#include "core.h"

/* The LLC/SNAP header before the Ethernet type in a data frame (RFC 1042),
   and the 802.1H one used for AppleTalk ARP and IPX. */
const unsigned char rfc1042_header[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
const unsigned char bridge_tunnel_header[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0xf8 };

unsigned int ieee80211_hdrlen(__le16 fc)
{
    if (ieee80211_is_data(fc)) {
        unsigned int len = ieee80211_has_a4(fc) ? 30 : 24;
        if (ieee80211_is_data_qos(fc)) {
            len += IEEE80211_QOS_CTL_LEN;
            if (ieee80211_has_order(fc))
                len += IEEE80211_HT_CTL_LEN;
        }
        return len;
    }
    if (ieee80211_is_mgmt(fc))
        return ieee80211_has_order(fc) ? 24 + IEEE80211_HT_CTL_LEN : 24;
    if (ieee80211_is_ctl(fc))
        /* CTS (0xc0) and ACK (0xd0) carry one address: 10 bytes; the rest two. */
        return (fc & cpu_to_le16(0x00e0)) == cpu_to_le16(0x00c0) ? 10 : 16;
    return 24;
}

/* --- channel numbers ------------------------------------------------------------- */

/* 802.11 numbers channels in 5 MHz steps from a base per band. */
u32 ieee80211_channel_to_freq_khz(int chan, enum nl80211_band band)
{
    if (chan <= 0)
        return 0;
    switch (band) {
    case NL80211_BAND_2GHZ:
        if (chan == 14)
            return MHZ_TO_KHZ(2484);    /* (Japan, 802.11b only) */
        if (chan < 14)
            return MHZ_TO_KHZ(2407 + chan * 5);
        break;
    case NL80211_BAND_5GHZ:
        if (chan >= 182 && chan <= 196)
            return MHZ_TO_KHZ(4000 + chan * 5);    /* 4.9 GHz (Japan) */
        return MHZ_TO_KHZ(5000 + chan * 5);
    case NL80211_BAND_6GHZ:
        if (chan == 2)
            return MHZ_TO_KHZ(5935);
        if (chan <= 233)
            return MHZ_TO_KHZ(5950 + chan * 5);
        break;
    default:
        break;
    }
    return 0;
}

int ieee80211_freq_khz_to_channel(u32 freq)
{
    u32 mhz = KHZ_TO_MHZ(freq);
    if (mhz == 2484)
        return 14;
    if (mhz >= 2412 && mhz < 2484)
        return (int)(mhz - 2407) / 5;
    if (mhz >= 4910 && mhz <= 4980)
        return (int)(mhz - 4000) / 5;
    if (mhz >= 5000 && mhz < 5925)
        return (int)(mhz - 5000) / 5;
    if (mhz == 5935)
        return 2;
    if (mhz > 5950 && mhz <= 7125)
        return (int)(mhz - 5950) / 5;
    return 0;
}

/* The radio's channel at this frequency, or NULL. Walks every band's
   channel list (about 60 entries on a dual-band card). */
struct ieee80211_channel *ieee80211_get_channel_khz(struct wiphy *wiphy, u32 freq)
{
    for (int b = 0; b < NUM_NL80211_BANDS; b++) {
        struct ieee80211_supported_band *sband = wiphy->bands[b];
        if (!sband)
            continue;
        for (int i = 0; i < sband->n_channels; i++) {
            struct ieee80211_channel *c = &sband->channels[i];
            if (MHZ_TO_KHZ(c->center_freq) + c->freq_offset == freq)
                return c;
        }
    }
    return NULL;
}

void cfg80211_chandef_create(struct cfg80211_chan_def *chandef, struct ieee80211_channel *chan,
                             enum nl80211_channel_type chantype)
{
    memset(chandef, 0, sizeof *chandef);
    chandef->chan = chan;
    chandef->center_freq1 = chan->center_freq;
    chandef->freq1_offset = chan->freq_offset;
    switch (chantype) {
    case NL80211_CHAN_NO_HT:
        chandef->width = NL80211_CHAN_WIDTH_20_NOHT;
        break;
    case NL80211_CHAN_HT20:
        chandef->width = NL80211_CHAN_WIDTH_20;
        break;
    case NL80211_CHAN_HT40PLUS:         /* the second 20 MHz above the primary */
        chandef->width = NL80211_CHAN_WIDTH_40;
        chandef->center_freq1 += 10;
        break;
    case NL80211_CHAN_HT40MINUS:
        chandef->width = NL80211_CHAN_WIDTH_40;
        chandef->center_freq1 -= 10;
        break;
    default:
        WARN_ON(1);
    }
}

/* --- elements ------------------------------------------------------------------------ */

const struct element *cfg80211_find_elem_match(u8 eid, const u8 *ies, unsigned int len,
                                               const u8 *match, unsigned int match_len,
                                               unsigned int match_offset)
{
    const struct element *e;
    for_each_element_id(e, eid, ies, len)
        if (e->datalen >= match_offset + match_len &&
            !memcmp(e->data + match_offset, match, match_len))
            return e;
    return NULL;
}

/* Is the element at pos named in ids? An extension element (id 255) is
   named by 255 followed by its extension id. */
static bool id_in_list(const u8 *ids, int n_ids, const u8 *ies, size_t ielen, size_t pos)
{
    bool ext = ies[pos] == WLAN_EID_EXTENSION && pos + 2 < ielen && ies[pos + 1] >= 1;
    u8 id = ext ? ies[pos + 2] : ies[pos];
    for (int i = 0; i < n_ids; i++) {
        if (ids[i] == WLAN_EID_EXTENSION) {
            if (i + 1 < n_ids && ext && ids[i + 1] == id)
                return true;
            i++;                        /* skip the extension id */
        } else if (!ext && ids[i] == id) {
            return true;
        }
    }
    return false;
}

/* Past the element at pos, and past any fragments continuing it. */
static size_t skip_elem(const u8 *ies, size_t ielen, size_t pos)
{
    u8 len = ies[pos + 1];
    pos += 2 + len;
    if (len == 255)
        while (pos + 1 < ielen && ies[pos] == WLAN_EID_FRAGMENT)
            pos += 2 + ies[pos + 1];
    return pos;
}

/* From offset, step over the elements named in ids: the place to split a
   list so the named ones stay first. After a RIC element, everything up to
   one named in after_ric belongs with it. */
size_t ieee80211_ie_split_ric(const u8 *ies, size_t ielen, const u8 *ids, int n_ids,
                              const u8 *after_ric, int n_after_ric, size_t offset)
{
    size_t pos = offset;
    while (pos + 1 < ielen && id_in_list(ids, n_ids, ies, ielen, pos)) {
        bool ric = ies[pos] == WLAN_EID_RIC_DATA && n_after_ric;
        pos = skip_elem(ies, ielen, pos);
        if (ric)
            while (pos + 1 < ielen && !id_in_list(after_ric, n_after_ric, ies, ielen, pos))
                pos = skip_elem(ies, ielen, pos);
    }
    return pos < ielen ? pos : ielen;
}
