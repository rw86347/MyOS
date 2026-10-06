/*
 * nl80211.c — Wi-Fi's netlink interface: the generic-netlink family
 * "nl80211" that `iw` and `wpa_supplicant` speak (Linux: net/wireless/
 * nl80211.c). Messages, attributes and their numbers are Linux's
 * (include/uapi/linux/nl80211.h); the code is ours.
 *
 * So far (W2c): the protocol features, the radios (GET_WIPHY, split into
 * several messages when the program can take that), the interfaces
 * (GET_INTERFACE), scanning (TRIGGER_SCAN, ABORT_SCAN) and its results
 * (GET_SCAN), and the "scan" events a scan sends when it starts and ends.
 *
 * A request is answered inside the sendmsg (net/netlink.c), in the
 * sender's thread; anything that reaches a driver takes the wiphy lock.
 */
#include <genetlink.h>
#include <linux/nl80211.h>
#include "core.h"

#define NL80211_FAMILY_ID   0x13        /* (programs look it up by name) */
#define MSG_BUF             4000        /* one message: fits a netlink datagram */

static const char *const nl80211_groups[] = {
    NL80211_MULTICAST_GROUP_CONFIG, NL80211_MULTICAST_GROUP_SCAN,
    NL80211_MULTICAST_GROUP_REG, NL80211_MULTICAST_GROUP_MLME,
    NL80211_MULTICAST_GROUP_VENDOR, NL80211_MULTICAST_GROUP_NAN, NULL,
};
enum { GROUP_CONFIG = 17, GROUP_SCAN, GROUP_REG, GROUP_MLME, GROUP_VENDOR, GROUP_NAN };

static int nl80211_request(struct nl_reply *r, const uint8_t *p, uint32_t plen);

const struct genl_family_k nl80211_family = {
    NL80211_GENL_NAME, NL80211_FAMILY_ID, 1, 0, NL80211_ATTR_MAX,
    nl80211_groups, GROUP_CONFIG, nl80211_request,
};

/* A message starts with the generic-netlink header: the command. */
static void begin(struct nl_msg *m, u8 cmd)
{
    struct genlmsghdr g = { cmd, 1, 0 };
    nl_put(m, &g, sizeof g);
}

static struct nl_msg *msg_new(void)
{
    struct nl_msg *m = kmalloc(sizeof *m + MSG_BUF, GFP_KERNEL);
    if (m) {
        m->b = (uint8_t *)(m + 1);
        m->len = 0;
        m->cap = MSG_BUF;
        m->over = 0;
    }
    return m;
}

static void msg_reset(struct nl_msg *m, u8 cmd)
{
    m->len = 0;
    m->over = 0;
    begin(m, cmd);
}

static u64 wdev_id(struct ieee80211_sub_if_data *sdata)
{
    return (u64)sdata->local->index << 32 | sdata->wdev.identifier;
}

/* --- the radio (wiphy) ------------------------------------------------------------- */

static void put_freq(struct nl_msg *m, int i, const struct ieee80211_channel *c)
{
    uint32_t at = nla_nest_start(m, (uint16_t)i);
    nla_put_u32(m, NL80211_FREQUENCY_ATTR_FREQ, c->center_freq);
    if (c->freq_offset)
        nla_put_u32(m, NL80211_FREQUENCY_ATTR_OFFSET, c->freq_offset);
    if (c->flags & IEEE80211_CHAN_DISABLED)
        nla_put_flag(m, NL80211_FREQUENCY_ATTR_DISABLED);
    if (c->flags & IEEE80211_CHAN_NO_IR)
        nla_put_flag(m, NL80211_FREQUENCY_ATTR_NO_IR);
    if (c->flags & IEEE80211_CHAN_RADAR)
        nla_put_flag(m, NL80211_FREQUENCY_ATTR_RADAR);
    if (c->flags & IEEE80211_CHAN_NO_HT40MINUS)
        nla_put_flag(m, NL80211_FREQUENCY_ATTR_NO_HT40_MINUS);
    if (c->flags & IEEE80211_CHAN_NO_HT40PLUS)
        nla_put_flag(m, NL80211_FREQUENCY_ATTR_NO_HT40_PLUS);
    if (c->flags & IEEE80211_CHAN_NO_80MHZ)
        nla_put_flag(m, NL80211_FREQUENCY_ATTR_NO_80MHZ);
    if (c->flags & IEEE80211_CHAN_NO_160MHZ)
        nla_put_flag(m, NL80211_FREQUENCY_ATTR_NO_160MHZ);
    if (c->flags & IEEE80211_CHAN_INDOOR_ONLY)
        nla_put_flag(m, NL80211_FREQUENCY_ATTR_INDOOR_ONLY);
    nla_put_u32(m, NL80211_FREQUENCY_ATTR_MAX_TX_POWER, (u32)DBM_TO_MBM(c->max_power));
    nla_nest_end(m, at);
}

/* One band: its channels, rates and HT/VHT abilities. */
static void put_band(struct nl_msg *m, int b, const struct ieee80211_supported_band *sb)
{
    uint32_t bands = nla_nest_start(m, NL80211_ATTR_WIPHY_BANDS);
    uint32_t band = nla_nest_start(m, (uint16_t)b);
    if (sb->ht_cap.ht_supported) {
        nla_put(m, NL80211_BAND_ATTR_HT_MCS_SET, &sb->ht_cap.mcs, sizeof sb->ht_cap.mcs);
        nla_put_u16(m, NL80211_BAND_ATTR_HT_CAPA, sb->ht_cap.cap);
        nla_put_u8(m, NL80211_BAND_ATTR_HT_AMPDU_FACTOR, sb->ht_cap.ampdu_factor);
        nla_put_u8(m, NL80211_BAND_ATTR_HT_AMPDU_DENSITY, sb->ht_cap.ampdu_density);
    }
    if (sb->vht_cap.vht_supported) {
        nla_put(m, NL80211_BAND_ATTR_VHT_MCS_SET, &sb->vht_cap.vht_mcs, sizeof sb->vht_cap.vht_mcs);
        nla_put_u32(m, NL80211_BAND_ATTR_VHT_CAPA, sb->vht_cap.cap);
    }
    uint32_t freqs = nla_nest_start(m, NL80211_BAND_ATTR_FREQS);
    for (int i = 0; i < sb->n_channels; i++)
        put_freq(m, i, &sb->channels[i]);
    nla_nest_end(m, freqs);
    uint32_t rates = nla_nest_start(m, NL80211_BAND_ATTR_RATES);
    for (int i = 0; i < sb->n_bitrates; i++) {
        uint32_t one = nla_nest_start(m, (uint16_t)i);
        nla_put_u32(m, NL80211_BITRATE_ATTR_RATE, sb->bitrates[i].bitrate);
        if (b == NL80211_BAND_2GHZ && (sb->bitrates[i].flags & IEEE80211_RATE_SHORT_PREAMBLE))
            nla_put_flag(m, NL80211_BITRATE_ATTR_2GHZ_SHORTPREAMBLE);
        nla_nest_end(m, one);
    }
    nla_nest_end(m, rates);
    nla_nest_end(m, band);
    nla_nest_end(m, bands);
}

static void put_iftypes(struct nl_msg *m, uint16_t attr, u16 modes)
{
    uint32_t at = nla_nest_start(m, attr);
    for (int t = 0; t < 16; t++)
        if (modes & BIT(t))
            nla_put_flag(m, (uint16_t)t);
    nla_nest_end(m, at);
}

/* The commands that reach a driver (and so may be missing): Linux lists
   these, and programs look for them before trying. */
static const u32 driver_cmds[] = { NL80211_CMD_TRIGGER_SCAN, NL80211_CMD_CONNECT,
                                   NL80211_CMD_DISCONNECT, NL80211_CMD_NEW_KEY };

/* Everything about one radio. In one message, or (split, when the program
   asked: its wiphy grew past a datagram) one part per message: the basics,
   then each band, then the commands. The program puts them back together. */
static int put_wiphy(struct nl_reply *r, struct nl_msg *m, struct ieee80211_local *local,
                     bool split, uint16_t flags)
{
    struct wiphy *wiphy = local->hw.wiphy;
    msg_reset(m, NL80211_CMD_NEW_WIPHY);
    nla_put_u32(m, NL80211_ATTR_WIPHY, (u32)local->index);
    nla_put_string(m, NL80211_ATTR_WIPHY_NAME, wiphy_name(wiphy));
    nla_put_u32(m, NL80211_ATTR_GENERATION, 1);
    nla_put_u8(m, NL80211_ATTR_WIPHY_RETRY_SHORT, 7);
    nla_put_u8(m, NL80211_ATTR_WIPHY_RETRY_LONG, 4);
    nla_put_u32(m, NL80211_ATTR_WIPHY_FRAG_THRESHOLD, (u32)-1);
    nla_put_u32(m, NL80211_ATTR_WIPHY_RTS_THRESHOLD, wiphy->rts_threshold);
    nla_put_u8(m, NL80211_ATTR_MAX_NUM_SCAN_SSIDS, wiphy->max_scan_ssids);
    nla_put_u16(m, NL80211_ATTR_MAX_SCAN_IE_LEN, wiphy->max_scan_ie_len);
    nla_put_u32(m, NL80211_ATTR_WIPHY_ANTENNA_AVAIL_TX, wiphy->available_antennas_tx);
    nla_put_u32(m, NL80211_ATTR_WIPHY_ANTENNA_AVAIL_RX, wiphy->available_antennas_rx);
    put_iftypes(m, NL80211_ATTR_SUPPORTED_IFTYPES, wiphy->interface_modes);
    u32 ciphers[8], nc = 0;             /* the ones we can use (key.c) */
    for (int i = 0; i < wiphy->n_cipher_suites && nc < ARRAY_SIZE(ciphers); i++)
        if (wifi_cipher_ok(wiphy, wiphy->cipher_suites[i]))
            ciphers[nc++] = wiphy->cipher_suites[i];
    if (nc)
        nla_put(m, NL80211_ATTR_CIPHER_SUITES, ciphers, nc * sizeof(u32));
    nla_put_u32(m, NL80211_ATTR_FEATURE_FLAGS, wiphy->features);
    nla_put(m, NL80211_ATTR_EXT_FEATURES, wiphy->ext_features, sizeof wiphy->ext_features);
    if (split)
        nl_reply(r, NL80211_FAMILY_ID, flags, m);
    for (int b = 0; b < NUM_NL80211_BANDS; b++) {
        if (!wiphy->bands[b])
            continue;
        if (split) {
            msg_reset(m, NL80211_CMD_NEW_WIPHY);
            nla_put_u32(m, NL80211_ATTR_WIPHY, (u32)local->index);
            nla_put_string(m, NL80211_ATTR_WIPHY_NAME, wiphy_name(wiphy));
        }
        put_band(m, b, wiphy->bands[b]);
        if (split)
            nl_reply(r, NL80211_FAMILY_ID, flags, m);
    }
    if (split) {
        msg_reset(m, NL80211_CMD_NEW_WIPHY);
        nla_put_u32(m, NL80211_ATTR_WIPHY, (u32)local->index);
        nla_put_string(m, NL80211_ATTR_WIPHY_NAME, wiphy_name(wiphy));
    }
    uint32_t cmds = nla_nest_start(m, NL80211_ATTR_SUPPORTED_COMMANDS);
    for (size_t i = 0; i < ARRAY_SIZE(driver_cmds); i++)
        nla_put_u32(m, (uint16_t)(i + 1), driver_cmds[i]);
    nla_nest_end(m, cmds);
    if (m->over)
        return -EMSGSIZE;
    nl_reply(r, NL80211_FAMILY_ID, flags, m);
    return 0;
}

/* --- interfaces ----------------------------------------------------------------------- */

static void put_iface(struct nl_msg *m, struct ieee80211_sub_if_data *sdata, u8 cmd)
{
    msg_reset(m, cmd);
    nla_put_u32(m, NL80211_ATTR_IFINDEX, (u32)sdata->dev->ifindex);
    nla_put_string(m, NL80211_ATTR_IFNAME, sdata->dev->name);
    nla_put_u32(m, NL80211_ATTR_IFTYPE, sdata->wdev.iftype);
    nla_put_u32(m, NL80211_ATTR_WIPHY, (u32)sdata->local->index);
    nla_put_u64(m, NL80211_ATTR_WDEV, wdev_id(sdata));
    nla_put(m, NL80211_ATTR_MAC, sdata->dev->dev_addr, ETH_ALEN);
    nla_put_u32(m, NL80211_ATTR_GENERATION, 1);
    nla_put_u8(m, NL80211_ATTR_4ADDR, 0);
    const struct wifi_conn *c = &sdata->conn;
    if (c->state == MLME_CONNECTED) {
        nla_put(m, NL80211_ATTR_SSID, c->ssid, c->ssid_len);
        nla_put_u32(m, NL80211_ATTR_WIPHY_FREQ, c->chan->center_freq);
        nla_put_u32(m, NL80211_ATTR_CHANNEL_WIDTH, NL80211_CHAN_WIDTH_20_NOHT);
        nla_put_u32(m, NL80211_ATTR_CENTER_FREQ1, c->chan->center_freq);
    }
}

/* The interface a request names: by ifindex, or by wdev id. */
static struct ieee80211_sub_if_data *req_sdata(const struct nlattr **tb)
{
    if (tb[NL80211_ATTR_IFINDEX] && nla_len(tb[NL80211_ATTR_IFINDEX]) >= 4)
        return wifi_sdata_by_dev(dev_get_by_index((int)nla_get_u32(tb[NL80211_ATTR_IFINDEX])));
    if (tb[NL80211_ATTR_WDEV] && nla_len(tb[NL80211_ATTR_WDEV]) >= 8) {
        u64 id = nla_get_u64(tb[NL80211_ATTR_WDEV]);
        struct ieee80211_local *local = wifi_radio((int)(id >> 32));
        struct ieee80211_sub_if_data *sdata;
        if (local)
            list_for_each_entry(sdata, &local->interfaces, list)
                if (sdata->wdev.identifier == (u32)id)
                    return sdata;
    }
    return NULL;
}

/* --- scanning ---------------------------------------------------------------------------- */

/* The scan's parameters, in an event: which channels and SSIDs. */
static void put_scan_params(struct nl_msg *m, const struct cfg80211_scan_request *req)
{
    uint32_t at = nla_nest_start(m, NL80211_ATTR_SCAN_FREQUENCIES);
    for (u32 i = 0; i < req->n_channels; i++)
        nla_put_u32(m, (uint16_t)i, req->channels[i]->center_freq);
    nla_nest_end(m, at);
    at = nla_nest_start(m, NL80211_ATTR_SCAN_SSIDS);
    for (int i = 0; i < req->n_ssids; i++)
        nla_put(m, (uint16_t)i, req->ssids[i].ssid, req->ssids[i].ssid_len);
    nla_nest_end(m, at);
}

static void scan_event(struct ieee80211_sub_if_data *sdata, const struct cfg80211_scan_request *req,
                       u8 cmd)
{
    struct nl_msg *m = msg_new();
    if (!m)
        return;
    begin(m, cmd);
    nla_put_u32(m, NL80211_ATTR_WIPHY, (u32)sdata->local->index);
    nla_put_u32(m, NL80211_ATTR_IFINDEX, (u32)sdata->dev->ifindex);
    nla_put_u64(m, NL80211_ATTR_WDEV, wdev_id(sdata));
    if (req)
        put_scan_params(m, req);
    genl_multicast(&nl80211_family, GROUP_SCAN, m);
    kfree(m);
}

/* scan.c: a scan ended (aborted or not). */
void nl80211_scan_done(struct ieee80211_sub_if_data *sdata, const struct cfg80211_scan_request *req,
                       bool aborted)
{
    scan_event(sdata, req, aborted ? NL80211_CMD_SCAN_ABORTED : NL80211_CMD_NEW_SCAN_RESULTS);
}

/* The scan flags a program may ask for: each only if the radio says it can
   (cfg80211's rule, so wpa_supplicant gets the answers it gets on Linux).
   "Co-located 6 GHz APs too" is accepted from anyone, as cfg80211 does (iw
   always asks; a radio without 6 GHz just doesn't). Random addresses,
   scanning as an AP and kHz frequencies aren't offered at all. */
static int scan_flags_ok(struct wiphy *wiphy, u32 f)
{
    static const struct { u32 flag; enum nl80211_ext_feature_index needs; } ext[] = {
        { NL80211_SCAN_FLAG_FILS_MAX_CHANNEL_TIME, NL80211_EXT_FEATURE_FILS_MAX_CHANNEL_TIME },
        { NL80211_SCAN_FLAG_ACCEPT_BCAST_PROBE_RESP, NL80211_EXT_FEATURE_ACCEPT_BCAST_PROBE_RESP },
        { NL80211_SCAN_FLAG_OCE_PROBE_REQ_HIGH_TX_RATE, NL80211_EXT_FEATURE_OCE_PROBE_REQ_HIGH_TX_RATE },
        { NL80211_SCAN_FLAG_OCE_PROBE_REQ_DEFERRAL_SUPPRESSION,
          NL80211_EXT_FEATURE_OCE_PROBE_REQ_DEFERRAL_SUPPRESSION },
        { NL80211_SCAN_FLAG_LOW_SPAN, NL80211_EXT_FEATURE_LOW_SPAN_SCAN },
        { NL80211_SCAN_FLAG_LOW_POWER, NL80211_EXT_FEATURE_LOW_POWER_SCAN },
        { NL80211_SCAN_FLAG_HIGH_ACCURACY, NL80211_EXT_FEATURE_HIGH_ACCURACY_SCAN },
        { NL80211_SCAN_FLAG_RANDOM_SN, NL80211_EXT_FEATURE_SCAN_RANDOM_SN },
        { NL80211_SCAN_FLAG_MIN_PREQ_CONTENT, NL80211_EXT_FEATURE_SCAN_MIN_PREQ_CONTENT },
    };
    u32 known = NL80211_SCAN_FLAG_LOW_PRIORITY | NL80211_SCAN_FLAG_FLUSH |
                NL80211_SCAN_FLAG_COLOCATED_6GHZ;
    for (size_t i = 0; i < ARRAY_SIZE(ext); i++)
        if (wiphy_ext_feature_isset(wiphy, ext[i].needs))
            known |= ext[i].flag;
    if ((f & NL80211_SCAN_FLAG_LOW_PRIORITY) && !(wiphy->features & NL80211_FEATURE_LOW_PRIORITY_SCAN))
        return 0;
    return !(f & ~known);
}

static int trigger_scan(const struct nlattr **tb)
{
    struct ieee80211_sub_if_data *sdata = req_sdata(tb);
    if (!sdata)
        return -ENODEV;
    struct wiphy *wiphy = sdata->local->hw.wiphy;
    const struct nlattr *a;
    int rem;

    u32 flags = tb[NL80211_ATTR_SCAN_FLAGS] && nla_len(tb[NL80211_ATTR_SCAN_FLAGS]) >= 4 ?
                nla_get_u32(tb[NL80211_ATTR_SCAN_FLAGS]) : 0;
    if (!scan_flags_ok(wiphy, flags))
        return -EOPNOTSUPP;

    /* How many channels and SSIDs, to size the request. */
    u32 n_chan = 0;
    if (tb[NL80211_ATTR_SCAN_FREQUENCIES]) {
        nla_for_each_nested(a, tb[NL80211_ATTR_SCAN_FREQUENCIES], rem)
            n_chan++;
        if (!n_chan)
            return -EINVAL;
    } else {
        for (int b = 0; b < NUM_NL80211_BANDS; b++)
            if (wiphy->bands[b])
                n_chan += (u32)wiphy->bands[b]->n_channels;
    }
    int n_ssids = 0;
    if (tb[NL80211_ATTR_SCAN_SSIDS])
        nla_for_each_nested(a, tb[NL80211_ATTR_SCAN_SSIDS], rem)
            n_ssids++;
    if (n_ssids > wiphy->max_scan_ssids)
        return -EINVAL;
    u32 ie_len = tb[NL80211_ATTR_IE] ? nla_len(tb[NL80211_ATTR_IE]) : 0;
    if (ie_len > wiphy->max_scan_ie_len)
        return -EINVAL;

    struct cfg80211_scan_request *req = kzalloc(sizeof(*req) + n_chan * sizeof(req->channels[0]) +
                                                (size_t)n_ssids * sizeof(struct cfg80211_ssid) +
                                                ie_len, GFP_KERNEL);
    if (!req)
        return -ENOMEM;
    if (tb[NL80211_ATTR_SCAN_FREQUENCIES]) {
        nla_for_each_nested(a, tb[NL80211_ATTR_SCAN_FREQUENCIES], rem) {
            struct ieee80211_channel *c = nla_len(a) >= 4 ?
                ieee80211_get_channel(wiphy, (int)nla_get_u32(a)) : NULL;
            if (c && !(c->flags & IEEE80211_CHAN_DISABLED))
                req->channels[req->n_channels++] = c;
        }
    } else {
        for (int b = 0; b < NUM_NL80211_BANDS; b++)
            for (int i = 0; wiphy->bands[b] && i < wiphy->bands[b]->n_channels; i++)
                if (!(wiphy->bands[b]->channels[i].flags & IEEE80211_CHAN_DISABLED))
                    req->channels[req->n_channels++] = &wiphy->bands[b]->channels[i];
    }
    req->ssids = (struct cfg80211_ssid *)&req->channels[n_chan];
    if (tb[NL80211_ATTR_SCAN_SSIDS])
        nla_for_each_nested(a, tb[NL80211_ATTR_SCAN_SSIDS], rem) {
            if (nla_len(a) > IEEE80211_MAX_SSID_LEN) {
                kfree(req);
                return -EINVAL;
            }
            memcpy(req->ssids[req->n_ssids].ssid, nla_data(a), nla_len(a));
            req->ssids[req->n_ssids++].ssid_len = (u8)nla_len(a);
        }
    if (ie_len) {
        u8 *ie = (u8 *)&req->ssids[n_ssids];
        memcpy(ie, nla_data(tb[NL80211_ATTR_IE]), ie_len);
        req->ie = ie;
        req->ie_len = ie_len;
    }
    req->flags = flags;
    /* One AP's probe responses only (else the wildcard: wifi_scan). */
    const struct nlattr *bssid = tb[NL80211_ATTR_BSSID] ? tb[NL80211_ATTR_BSSID] : tb[NL80211_ATTR_MAC];
    if (bssid && nla_len(bssid) == ETH_ALEN)
        memcpy(req->bssid, nla_data(bssid), ETH_ALEN);
    if (!req->n_channels) {
        kfree(req);
        return -EINVAL;
    }

    wiphy_lock(wiphy);
    int err = wifi_scan(sdata, req);
    if (!err)
        scan_event(sdata, req, NL80211_CMD_TRIGGER_SCAN);
    wiphy_unlock(wiphy);
    if (err)
        kfree(req);
    return err;
}

static int abort_scan(const struct nlattr **tb)
{
    struct ieee80211_sub_if_data *sdata = req_sdata(tb);
    if (!sdata)
        return -ENODEV;
    struct wiphy *wiphy = sdata->local->hw.wiphy;
    wiphy_lock(wiphy);
    int err = sdata->local->scan_sdata == sdata ? 0 : -ENOENT;
    if (!err)
        wifi_scan_cancel(sdata);
    wiphy_unlock(wiphy);
    return err;
}

struct bss_dump {
    struct nl_reply *r;
    struct nl_msg *m;
    struct ieee80211_sub_if_data *sdata;
};

/* One network, as NL80211_CMD_NEW_SCAN_RESULTS (what `iw scan` prints). */
static void put_bss(struct wiphy *wiphy, struct cfg80211_bss *bss, void *data)
{
    struct bss_dump *d = data;
    struct wifi_bss *b = container_of(bss, struct wifi_bss, pub);
    struct nl_msg *m = d->m;
    (void)wiphy;
    msg_reset(m, NL80211_CMD_NEW_SCAN_RESULTS);
    nla_put_u32(m, NL80211_ATTR_GENERATION, d->sdata->local->bss_generation);
    nla_put_u32(m, NL80211_ATTR_IFINDEX, (u32)d->sdata->dev->ifindex);
    nla_put_u64(m, NL80211_ATTR_WDEV, wdev_id(d->sdata));
    uint32_t at = nla_nest_start(m, NL80211_ATTR_BSS);
    nla_put(m, NL80211_BSS_BSSID, bss->bssid, ETH_ALEN);
    nla_put_u64(m, NL80211_BSS_TSF, bss->ies->tsf);
    if (bss->ies->from_beacon)
        nla_put_u64(m, NL80211_BSS_BEACON_TSF, bss->ies->tsf);
    else
        nla_put_flag(m, NL80211_BSS_PRESP_DATA);
    nla_put(m, NL80211_BSS_INFORMATION_ELEMENTS, bss->ies->data, (u32)bss->ies->len);
    nla_put_u16(m, NL80211_BSS_BEACON_INTERVAL, bss->beacon_interval);
    nla_put_u16(m, NL80211_BSS_CAPABILITY, bss->capability);
    nla_put_u32(m, NL80211_BSS_FREQUENCY, bss->channel->center_freq);
    nla_put_u32(m, NL80211_BSS_SEEN_MS_AGO, jiffies_to_msecs(jiffies - b->last_seen));
    nla_put_u64(m, NL80211_BSS_LAST_SEEN_BOOTTIME, (u64)jiffies_to_msecs(b->last_seen) * 1000000ull);
    nla_put_s32(m, NL80211_BSS_SIGNAL_MBM, bss->signal);
    const struct wifi_conn *c = &d->sdata->conn;
    if (c->state >= MLME_AUTH && c->chan == bss->channel && ether_addr_equal(c->bssid, bss->bssid))
        nla_put_u32(m, NL80211_BSS_STATUS, c->state == MLME_CONNECTED ?
                    NL80211_BSS_STATUS_ASSOCIATED : NL80211_BSS_STATUS_AUTHENTICATED);
    nla_nest_end(m, at);
    nl_reply(d->r, NL80211_FAMILY_ID, NLM_F_MULTI, m);
}

static int get_scan(struct nl_reply *r, struct nl_msg *m, const struct nlattr **tb)
{
    struct ieee80211_sub_if_data *sdata = req_sdata(tb);
    if (!sdata)
        return -ENODEV;
    struct bss_dump d = { r, m, sdata };
    cfg80211_bss_iter(sdata->local->hw.wiphy, NULL, put_bss, &d);
    nl_reply_done(r);
    return 0;
}

/* GET_SURVEY: what the driver measured on each channel (noise, busy time).
   wpa_supplicant asks for it with every SIGNAL_POLL (the noise beside the
   signal); a driver without get_survey gives an empty list, which it takes. */
static int get_survey(struct nl_reply *r, struct nl_msg *m, const struct nlattr **tb)
{
    struct ieee80211_sub_if_data *sdata = req_sdata(tb);
    if (!sdata)
        return -ENODEV;
    struct ieee80211_local *local = sdata->local;
    for (int idx = 0; local->ops->get_survey && idx < 64; idx++) {
        struct survey_info s;
        memset(&s, 0, sizeof s);
        if (local->ops->get_survey(&local->hw, idx, &s) || !s.channel)
            break;
        msg_reset(m, NL80211_CMD_NEW_SURVEY_RESULTS);
        nla_put_u32(m, NL80211_ATTR_IFINDEX, (u32)sdata->dev->ifindex);
        uint32_t at = nla_nest_start(m, NL80211_ATTR_SURVEY_INFO);
        nla_put_u32(m, NL80211_SURVEY_INFO_FREQUENCY, s.channel->center_freq);
        if (s.filled & SURVEY_INFO_NOISE_DBM)
            nla_put_u8(m, NL80211_SURVEY_INFO_NOISE, (u8)s.noise);
        if (s.filled & SURVEY_INFO_IN_USE)
            nla_put_flag(m, NL80211_SURVEY_INFO_IN_USE);
        if (s.filled & SURVEY_INFO_TIME)
            nla_put_u64(m, NL80211_SURVEY_INFO_TIME, s.time);
        if (s.filled & SURVEY_INFO_TIME_BUSY)
            nla_put_u64(m, NL80211_SURVEY_INFO_TIME_BUSY, s.time_busy);
        nla_nest_end(m, at);
        nl_reply(r, NL80211_FAMILY_ID, NLM_F_MULTI, m);
    }
    nl_reply_done(r);
    return 0;
}

/* --- connecting ------------------------------------------------------------------------------ */

static void mlme_event(struct ieee80211_sub_if_data *sdata, struct nl_msg *m)
{
    nla_put_u32(m, NL80211_ATTR_WIPHY, (u32)sdata->local->index);
    nla_put_u32(m, NL80211_ATTR_IFINDEX, (u32)sdata->dev->ifindex);
    nla_put_u64(m, NL80211_ATTR_WDEV, wdev_id(sdata));
}

/* mlme.c: a connection attempt ended (status 0: connected). */
void nl80211_connect_result(struct ieee80211_sub_if_data *sdata, u16 status, bool timed_out)
{
    const struct wifi_conn *c = &sdata->conn;
    struct nl_msg *m = msg_new();
    if (!m)
        return;
    begin(m, NL80211_CMD_CONNECT);
    mlme_event(sdata, m);
    if (!is_zero_ether_addr(c->bssid))
        nla_put(m, NL80211_ATTR_MAC, c->bssid, ETH_ALEN);
    nla_put_u16(m, NL80211_ATTR_STATUS_CODE, status);
    if (timed_out)
        nla_put_flag(m, NL80211_ATTR_TIMED_OUT);
    if (c->req_ies_len)
        nla_put(m, NL80211_ATTR_REQ_IE, c->req_ies, (u32)c->req_ies_len);
    if (c->resp_ies_len)
        nla_put(m, NL80211_ATTR_RESP_IE, c->resp_ies, (u32)c->resp_ies_len);
    genl_multicast(&nl80211_family, GROUP_MLME, m);
    kfree(m);
}

void nl80211_disconnected(struct ieee80211_sub_if_data *sdata, u16 reason, bool by_ap)
{
    struct nl_msg *m = msg_new();
    if (!m)
        return;
    begin(m, NL80211_CMD_DISCONNECT);
    mlme_event(sdata, m);
    if (reason)
        nla_put_u16(m, NL80211_ATTR_REASON_CODE, reason);
    if (by_ap)
        nla_put_flag(m, NL80211_ATTR_DISCONNECTED_BY_AP);
    genl_multicast(&nl80211_family, GROUP_MLME, m);
    kfree(m);
}

static int connect(struct nl_reply *r, const struct nlattr **tb)
{
    struct ieee80211_sub_if_data *sdata = req_sdata(tb);
    if (!sdata)
        return -ENODEV;
    const struct nlattr *ssid = tb[NL80211_ATTR_SSID];
    if (!ssid || !nla_len(ssid) || nla_len(ssid) > IEEE80211_MAX_SSID_LEN)
        return -EINVAL;
    if (tb[NL80211_ATTR_AUTH_TYPE]) {
        u32 t = nla_get_u32(tb[NL80211_ATTR_AUTH_TYPE]);
        if (t != NL80211_AUTHTYPE_OPEN_SYSTEM && t != NL80211_AUTHTYPE_AUTOMATIC)
            return -EOPNOTSUPP;         /* (open system only, so far) */
    }
    struct wifi_connect_params p = {
        .ssid = nla_data(ssid),
        .ssid_len = (u8)nla_len(ssid),
        .bssid = tb[NL80211_ATTR_MAC] && nla_len(tb[NL80211_ATTR_MAC]) == ETH_ALEN ?
                 nla_data(tb[NL80211_ATTR_MAC]) : NULL,
        .freq = tb[NL80211_ATTR_WIPHY_FREQ] ? nla_get_u32(tb[NL80211_ATTR_WIPHY_FREQ]) : 0,
        .ie = tb[NL80211_ATTR_IE] ? nla_data(tb[NL80211_ATTR_IE]) : NULL,
        .ie_len = tb[NL80211_ATTR_IE] ? nla_len(tb[NL80211_ATTR_IE]) : 0,
        .privacy = tb[NL80211_ATTR_PRIVACY] != NULL,
        .control_port = tb[NL80211_ATTR_CONTROL_PORT] != NULL,
        .control_port_no_encrypt = tb[NL80211_ATTR_CONTROL_PORT_NO_ENCRYPT] != NULL,
        .control_port_over_nl80211 = tb[NL80211_ATTR_CONTROL_PORT_OVER_NL80211] != NULL,
        .control_port_ethertype = tb[NL80211_ATTR_CONTROL_PORT_ETHERTYPE] ?      /* (host order) */
                                  nla_get_u16(tb[NL80211_ATTR_CONTROL_PORT_ETHERTYPE]) : 0,
        .owner = tb[NL80211_ATTR_SOCKET_OWNER] ? nl_reply_portid(r) : 0,
    };
    struct wiphy *wiphy = sdata->local->hw.wiphy;
    wiphy_lock(wiphy);
    int err = wifi_connect(sdata, &p);
    wiphy_unlock(wiphy);
    return err;
}

static int disconnect(const struct nlattr **tb)
{
    struct ieee80211_sub_if_data *sdata = req_sdata(tb);
    if (!sdata)
        return -ENODEV;
    u16 reason = tb[NL80211_ATTR_REASON_CODE] ? nla_get_u16(tb[NL80211_ATTR_REASON_CODE]) :
                 WLAN_REASON_DEAUTH_LEAVING;
    struct wiphy *wiphy = sdata->local->hw.wiphy;
    wiphy_lock(wiphy);
    int err = wifi_disconnect(sdata, reason ? reason : WLAN_REASON_DEAUTH_LEAVING, true);
    wiphy_unlock(wiphy);
    return err;
}

/* Our AP as a station entry (what `iw dev wlan0 link` and `station dump` show). */
static void put_station(struct nl_msg *m, struct ieee80211_sub_if_data *sdata, const struct wifi_sta *s)
{
    msg_reset(m, NL80211_CMD_NEW_STATION);
    nla_put_u32(m, NL80211_ATTR_IFINDEX, (u32)sdata->dev->ifindex);
    nla_put(m, NL80211_ATTR_MAC, s->sta.addr, ETH_ALEN);
    nla_put_u32(m, NL80211_ATTR_GENERATION, 1);
    uint32_t at = nla_nest_start(m, NL80211_ATTR_STA_INFO);
    nla_put_u32(m, NL80211_STA_INFO_CONNECTED_TIME,
                jiffies_to_msecs(jiffies - s->connected_at) / 1000);
    nla_put_u32(m, NL80211_STA_INFO_INACTIVE_TIME, jiffies_to_msecs(jiffies - s->last_rx));
    nla_put_u32(m, NL80211_STA_INFO_RX_BYTES, (u32)s->rx_bytes);
    nla_put_u64(m, NL80211_STA_INFO_RX_BYTES64, s->rx_bytes);
    nla_put_u32(m, NL80211_STA_INFO_TX_BYTES, (u32)s->tx_bytes);
    nla_put_u64(m, NL80211_STA_INFO_TX_BYTES64, s->tx_bytes);
    nla_put_u32(m, NL80211_STA_INFO_RX_PACKETS, s->rx_packets);
    nla_put_u32(m, NL80211_STA_INFO_TX_PACKETS, s->tx_packets);
    if (s->last_signal)
        nla_put_u8(m, NL80211_STA_INFO_SIGNAL, (u8)s->last_signal);
    struct nl80211_sta_flag_update f = { 0, 0 };
    f.mask = BIT(NL80211_STA_FLAG_AUTHORIZED) | BIT(NL80211_STA_FLAG_WME) |
             BIT(NL80211_STA_FLAG_AUTHENTICATED) | BIT(NL80211_STA_FLAG_ASSOCIATED);
    if (s->state >= IEEE80211_STA_AUTH)
        f.set |= BIT(NL80211_STA_FLAG_AUTHENTICATED);
    if (s->state >= IEEE80211_STA_ASSOC)
        f.set |= BIT(NL80211_STA_FLAG_ASSOCIATED);
    if (s->state >= IEEE80211_STA_AUTHORIZED)
        f.set |= BIT(NL80211_STA_FLAG_AUTHORIZED);
    if (s->sta.wme)
        f.set |= BIT(NL80211_STA_FLAG_WME);
    nla_put(m, NL80211_STA_INFO_STA_FLAGS, &f, sizeof f);
    nla_nest_end(m, at);
}

static int get_station(struct nl_reply *r, struct nl_msg *m, const struct nlattr **tb)
{
    struct ieee80211_sub_if_data *sdata = req_sdata(tb);
    if (!sdata)
        return -ENODEV;
    const struct wifi_sta *ap = sdata->conn.state >= MLME_AUTH ? sdata->conn.ap : NULL;
    if (nl_reply_is_dump(r)) {
        if (ap) {
            put_station(m, sdata, ap);
            nl_reply(r, NL80211_FAMILY_ID, NLM_F_MULTI, m);
        }
        nl_reply_done(r);
        return 0;
    }
    const struct nlattr *mac = tb[NL80211_ATTR_MAC];
    if (!ap || !mac || nla_len(mac) != ETH_ALEN || !ether_addr_equal(nla_data(mac), ap->sta.addr))
        return -ENOENT;
    put_station(m, sdata, ap);
    nl_reply(r, NL80211_FAMILY_ID, 0, m);
    return 0;
}

/* --- keys and the port (after wpa_supplicant's 4-way handshake) ------------------------------ */

/* A key request carries its fields nested in NL80211_ATTR_KEY (as
   wpa_supplicant sends them) or, older style, at the top level. */
struct key_req {
    const u8 *data, *seq;
    u32 data_len, seq_len, cipher;
    int idx;
    bool pairwise;
};

static int key_parse(const struct nlattr **tb, struct key_req *k)
{
    memset(k, 0, sizeof *k);
    k->idx = 0;
    if (tb[NL80211_ATTR_KEY]) {
        const struct nlattr *kt[NL80211_KEY_MAX + 1];
        if (nla_parse(kt, NL80211_KEY_MAX, nla_data(tb[NL80211_ATTR_KEY]), nla_len(tb[NL80211_ATTR_KEY])))
            return -EINVAL;
        if (kt[NL80211_KEY_DATA]) {
            k->data = nla_data(kt[NL80211_KEY_DATA]);
            k->data_len = nla_len(kt[NL80211_KEY_DATA]);
        }
        if (kt[NL80211_KEY_SEQ]) {
            k->seq = nla_data(kt[NL80211_KEY_SEQ]);
            k->seq_len = nla_len(kt[NL80211_KEY_SEQ]);
        }
        if (kt[NL80211_KEY_CIPHER])
            k->cipher = nla_get_u32(kt[NL80211_KEY_CIPHER]);
        if (kt[NL80211_KEY_IDX])
            k->idx = nla_get_u8(kt[NL80211_KEY_IDX]);
        if (kt[NL80211_KEY_TYPE])
            k->pairwise = nla_get_u32(kt[NL80211_KEY_TYPE]) == NL80211_KEYTYPE_PAIRWISE;
    } else {
        if (tb[NL80211_ATTR_KEY_DATA]) {
            k->data = nla_data(tb[NL80211_ATTR_KEY_DATA]);
            k->data_len = nla_len(tb[NL80211_ATTR_KEY_DATA]);
        }
        if (tb[NL80211_ATTR_KEY_SEQ]) {
            k->seq = nla_data(tb[NL80211_ATTR_KEY_SEQ]);
            k->seq_len = nla_len(tb[NL80211_ATTR_KEY_SEQ]);
        }
        if (tb[NL80211_ATTR_KEY_CIPHER])
            k->cipher = nla_get_u32(tb[NL80211_ATTR_KEY_CIPHER]);
        if (tb[NL80211_ATTR_KEY_IDX])
            k->idx = nla_get_u8(tb[NL80211_ATTR_KEY_IDX]);
        if (tb[NL80211_ATTR_KEY_TYPE])
            k->pairwise = nla_get_u32(tb[NL80211_ATTR_KEY_TYPE]) == NL80211_KEYTYPE_PAIRWISE;
    }
    /* A key for a station's address is pairwise (Linux's rule). */
    if (tb[NL80211_ATTR_MAC] && nla_len(tb[NL80211_ATTR_MAC]) == ETH_ALEN &&
        !is_multicast_ether_addr(nla_data(tb[NL80211_ATTR_MAC])))
        k->pairwise = true;
    return 0;
}

static int key_cmd(u8 cmd, const struct nlattr **tb)
{
    struct ieee80211_sub_if_data *sdata = req_sdata(tb);
    if (!sdata)
        return -ENODEV;
    struct key_req k;
    if (key_parse(tb, &k))
        return -EINVAL;
    const u8 *mac = k.pairwise && tb[NL80211_ATTR_MAC] ? nla_data(tb[NL80211_ATTR_MAC]) : NULL;
    if (k.pairwise && !mac)
        return -EINVAL;
    struct wiphy *wiphy = sdata->local->hw.wiphy;
    int err = 0;
    wiphy_lock(wiphy);
    switch (cmd) {
    case NL80211_CMD_NEW_KEY:
        err = !k.data || !k.cipher ? -EINVAL :
              wifi_key_add(sdata, mac, k.idx, k.cipher, k.data, k.data_len, k.seq, k.seq_len);
        break;
    case NL80211_CMD_DEL_KEY:
        err = wifi_key_del(sdata, mac, k.idx);
        break;
    case NL80211_CMD_SET_KEY:           /* "use this one by default": the only one there is */
        err = wifi_key_for_rx(sdata, !k.pairwise, k.idx) || sdata->conn.ptk ? 0 : -ENOENT;
        break;
    }
    wiphy_unlock(wiphy);
    return err;
}

/* wpa_supplicant opens (or closes) the port: STA_FLAGS2 authorized. */
static int set_station(const struct nlattr **tb)
{
    struct ieee80211_sub_if_data *sdata = req_sdata(tb);
    if (!sdata)
        return -ENODEV;
    const struct nlattr *mac = tb[NL80211_ATTR_MAC], *f = tb[NL80211_ATTR_STA_FLAGS2];
    if (!mac || nla_len(mac) != ETH_ALEN || !sdata->conn.ap ||
        !ether_addr_equal(nla_data(mac), sdata->conn.ap->sta.addr))
        return -ENOENT;
    if (!f || nla_len(f) < sizeof(struct nl80211_sta_flag_update))
        return 0;                       /* (nothing we keep) */
    struct nl80211_sta_flag_update upd;
    memcpy(&upd, nla_data(f), sizeof upd);
    if (!(upd.mask & BIT(NL80211_STA_FLAG_AUTHORIZED)))
        return 0;
    struct wiphy *wiphy = sdata->local->hw.wiphy;
    wiphy_lock(wiphy);
    int err = wifi_authorize(sdata, upd.set & BIT(NL80211_STA_FLAG_AUTHORIZED));
    wiphy_unlock(wiphy);
    return err;
}

/* --- requests --------------------------------------------------------------------------- */

static int nl80211_request(struct nl_reply *r, const uint8_t *p, uint32_t plen)
{
    u8 cmd = ((const struct genlmsghdr *)p)->cmd;
    const struct nlattr **tb = kmalloc(sizeof(*tb) * (NL80211_ATTR_MAX + 1), GFP_KERNEL);
    struct nl_msg *m = msg_new();
    int err = -ENOMEM;
    if (!tb || !m)
        goto out;
    err = nla_parse(tb, NL80211_ATTR_MAX, p + sizeof(struct genlmsghdr),
                    plen - (u32)sizeof(struct genlmsghdr));
    if (err)
        goto out;

    switch (cmd) {
    case NL80211_CMD_GET_PROTOCOL_FEATURES:
        begin(m, cmd);
        nla_put_u32(m, NL80211_ATTR_PROTOCOL_FEATURES, NL80211_PROTOCOL_FEATURE_SPLIT_WIPHY_DUMP);
        nl_reply(r, NL80211_FAMILY_ID, 0, m);
        break;

    case NL80211_CMD_GET_WIPHY: {
        bool split = tb[NL80211_ATTR_SPLIT_WIPHY_DUMP] != NULL;
        if (nl_reply_is_dump(r)) {
            int only = tb[NL80211_ATTR_WIPHY] ? (int)nla_get_u32(tb[NL80211_ATTR_WIPHY]) : -1;
            struct ieee80211_sub_if_data *s = req_sdata(tb);
            if (s)
                only = s->local->index;
            for (int i = 0; i < WIFI_MAX_RADIOS && !err; i++)
                if (wifi_radio(i) && (only < 0 || only == i))
                    err = put_wiphy(r, m, wifi_radio(i), split, NLM_F_MULTI);
            if (!err)
                nl_reply_done(r);
        } else {
            struct ieee80211_sub_if_data *s = req_sdata(tb);
            struct ieee80211_local *local = s ? s->local :
                tb[NL80211_ATTR_WIPHY] ? wifi_radio((int)nla_get_u32(tb[NL80211_ATTR_WIPHY])) : NULL;
            err = local ? put_wiphy(r, m, local, false, 0) : -ENODEV;
        }
        break;
    }

    case NL80211_CMD_GET_INTERFACE:
        if (nl_reply_is_dump(r)) {
            for (int i = 0; i < WIFI_MAX_RADIOS; i++) {
                struct ieee80211_local *local = wifi_radio(i);
                struct ieee80211_sub_if_data *sdata;
                if (!local || (tb[NL80211_ATTR_WIPHY] &&
                               nla_get_u32(tb[NL80211_ATTR_WIPHY]) != (u32)i))
                    continue;
                list_for_each_entry(sdata, &local->interfaces, list) {
                    put_iface(m, sdata, NL80211_CMD_NEW_INTERFACE);
                    nl_reply(r, NL80211_FAMILY_ID, NLM_F_MULTI, m);
                }
            }
            nl_reply_done(r);
        } else {
            struct ieee80211_sub_if_data *sdata = req_sdata(tb);
            if (!sdata) {
                err = -ENODEV;
                break;
            }
            put_iface(m, sdata, NL80211_CMD_NEW_INTERFACE);
            nl_reply(r, NL80211_FAMILY_ID, 0, m);
        }
        break;

    case NL80211_CMD_SET_INTERFACE: {
        /* Only "stay a station" for now: wpa_supplicant asks for that. */
        struct ieee80211_sub_if_data *sdata = req_sdata(tb);
        if (!sdata)
            err = -ENODEV;
        else if (tb[NL80211_ATTR_IFTYPE] &&
                 nla_get_u32(tb[NL80211_ATTR_IFTYPE]) != sdata->wdev.iftype)
            err = -EOPNOTSUPP;
        break;
    }

    case NL80211_CMD_TRIGGER_SCAN:
        err = trigger_scan(tb);
        break;
    case NL80211_CMD_ABORT_SCAN:
        err = abort_scan(tb);
        break;
    case NL80211_CMD_GET_SCAN:
        err = get_scan(r, m, tb);
        break;
    case NL80211_CMD_CONNECT:
        err = connect(r, tb);
        break;
    case NL80211_CMD_DISCONNECT:
        err = disconnect(tb);
        break;
    case NL80211_CMD_GET_STATION:
        err = get_station(r, m, tb);
        break;
    case NL80211_CMD_GET_SURVEY:
        err = get_survey(r, m, tb);
        break;
    case NL80211_CMD_NEW_KEY:
    case NL80211_CMD_DEL_KEY:
    case NL80211_CMD_SET_KEY:
        err = key_cmd(cmd, tb);
        break;
    case NL80211_CMD_SET_STATION:
        err = set_station(tb);
        break;
    /* Accepted and not acted on (yet): which management frames a program
       wants to see, power save, signal-quality reports. wpa_supplicant
       asks for them and carries on either way. */
    case NL80211_CMD_REGISTER_FRAME:
    case NL80211_CMD_SET_POWER_SAVE:
    case NL80211_CMD_SET_CQM:
    case NL80211_CMD_SET_MULTICAST_TO_UNICAST:
        err = req_sdata(tb) ? 0 : -ENODEV;
        break;
    default:
        err = -EOPNOTSUPP;
    }
out:
    kfree(m);
    kfree(tb);
    return err;
}
