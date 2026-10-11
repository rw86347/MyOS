/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * mlme.c — joining a network as a station (Linux: mac80211's mlme.c and
 * cfg80211's sme.c, in one).
 *
 *   connect:   find the network in the scan table (scan first if it isn't
 *              there), tune a channel context to it, create the AP as a
 *              peer ("sta") in the driver, then
 *   auth:      send Authentication (open system), wait for the answer;
 *   assoc:     send Association Request (our rates, plus the elements the
 *              program gave: wpa_supplicant's RSN), wait for the answer;
 *   connected: the AP gave us an association id. An open network is
 *              usable at once; a protected one only once wpa_supplicant has
 *              done the 4-way handshake and says so (W3c).
 *
 * Each frame is tried MLME_TRIES times, MLME_TIMEOUT_MS apart. Everything
 * here runs with the wiphy lock held (nl80211 requests, the timeout work,
 * and received management frames, which the receive path hands to a
 * wiphy work). The driver hears of each step through sta_state, the
 * channel-context calls and bss/vif_cfg/link_info_changed, in the order
 * Linux's mac80211 calls them.
 */
#include <ipv4.h>
#include "core.h"

#define MLME_TIMEOUT_MS 300
#define MLME_TRIES      3

/* --- telling the driver what changed ------------------------------------------------ */

/* The parts of a change that belong to the interface rather than the link
   (drivers with vif_cfg_changed/link_info_changed get them split). */
#define VIF_CFG_CHANGES (BSS_CHANGED_ASSOC | BSS_CHANGED_IDLE | BSS_CHANGED_PS | \
                         BSS_CHANGED_ARP_FILTER | BSS_CHANGED_SSID | \
                         BSS_CHANGED_MLD_VALID_LINKS | BSS_CHANGED_MLD_TTLM)

void wifi_bss_changed(struct ieee80211_sub_if_data *sdata, u64 changed)
{
    const struct ieee80211_ops *ops = sdata->local->ops;
    struct ieee80211_hw *hw = &sdata->local->hw;
    if (!sdata->in_driver || !changed || sdata->local->hw_lost)
        return;
    if (ops->vif_cfg_changed || ops->link_info_changed) {
        u64 vc = changed & VIF_CFG_CHANGES, lc = changed & ~(u64)VIF_CFG_CHANGES;
        if (vc && ops->vif_cfg_changed)
            ops->vif_cfg_changed(hw, &sdata->vif, vc);
        if (lc && ops->link_info_changed)
            ops->link_info_changed(hw, &sdata->vif, &sdata->vif.bss_conf, lc);
    } else if (ops->bss_info_changed) {
        ops->bss_info_changed(hw, &sdata->vif, &sdata->vif.bss_conf, changed);
    }
}

/* --- peers and their queues ----------------------------------------------------------- */

static struct wifi_txq *txq_new(struct ieee80211_sub_if_data *sdata, struct wifi_sta *sta, int tid)
{
    struct wifi_txq *q = kzalloc(sizeof(*q) + sdata->local->hw.txq_data_size, GFP_KERNEL);
    if (!q)
        return NULL;
    skb_queue_head_init(&q->frames);
    q->txq.vif = &sdata->vif;
    q->txq.sta = sta ? &sta->sta : NULL;
    q->txq.tid = (u8)tid;
    q->txq.ac = tid < IEEE80211_NUM_TIDS ? tid_ac(tid) : IEEE80211_AC_VO;
    return q;
}

static struct wifi_sta *sta_new(struct ieee80211_sub_if_data *sdata, const u8 *addr)
{
    struct ieee80211_local *local = sdata->local;
    struct wifi_sta *s = kzalloc(sizeof(*s) + local->hw.sta_data_size, GFP_KERNEL);
    if (!s)
        return NULL;
    s->sdata = sdata;
    s->state = IEEE80211_STA_NOTEXIST;
    ether_addr_copy(s->sta.addr, addr);
    s->sta.deflink.sta = &s->sta;
    ether_addr_copy(s->sta.deflink.addr, addr);
    s->sta.deflink.rx_nss = 1;
    s->sta.deflink.bandwidth = IEEE80211_STA_RX_BW_20;
    s->sta.deflink.smps_mode = IEEE80211_SMPS_OFF;
    s->sta.deflink.agg.max_amsdu_len = IEEE80211_MAX_MPDU_LEN_HT_3839;
    s->sta.deflink.agg.max_rc_amsdu_len = IEEE80211_MAX_MPDU_LEN_HT_3839;
    s->sta.cur = &s->sta.deflink.agg;
    s->sta.max_rx_aggregation_subframes = local->hw.max_rx_aggregation_subframes;
    s->sta.link[0] = &s->sta.deflink;
    if (local->ops->wake_tx_queue)
        for (int t = 0; t <= IEEE80211_NUM_TIDS; t++) {
            s->txq[t] = txq_new(sdata, s, t);
            if (!s->txq[t])
                goto fail;
            s->sta.txq[t] = &s->txq[t]->txq;
        }
    return s;
fail:
    for (int t = 0; t <= IEEE80211_NUM_TIDS; t++)
        kfree(s->txq[t]);
    kfree(s);
    return NULL;
}

/* Walk the driver's view of the peer to `to`, one state at a time, as
   Linux does (NOTEXIST <-> NONE <-> AUTH <-> ASSOC <-> AUTHORIZED). */
static int sta_move(struct wifi_sta *s, enum ieee80211_sta_state to)
{
    struct ieee80211_local *local = s->sdata->local;
    while (s->state != to) {
        enum ieee80211_sta_state next = s->state < to ? s->state + 1 : s->state - 1;
        int err = 0;
        if (local->ops->sta_state && !local->hw_lost)
            err = local->ops->sta_state(&local->hw, &s->sdata->vif, &s->sta, s->state, next);
        if (err && next > s->state)
            return err;                 /* (going down never fails) */
        s->state = next;
    }
    return 0;
}

static void sta_free_now(struct wifi_sta *s)
{
    for (int t = 0; t <= IEEE80211_NUM_TIDS; t++)
        if (s->txq[t]) {
            skb_queue_purge(&s->txq[t]->frames);
            kfree(s->txq[t]);
        }
    kfree(s);
}

static void sta_free(struct wifi_sta *s)
{
    if (!s)
        return;
    struct ieee80211_local *local = s->sdata->local;
    if (local->hw_lost) {
        /* The driver couldn't be told the peer is gone, so it may still
           point at it (iwlwifi keeps its queues on a to-do list): keep the
           memory until the driver itself is gone. Rare: a driver that
           didn't come back after a firmware error. */
        for (int t = 0; t <= IEEE80211_NUM_TIDS; t++)
            if (s->txq[t])
                skb_queue_purge(&s->txq[t]->frames);
        list_add(&s->dead, &local->dead_stas);
        return;
    }
    sta_move(s, IEEE80211_STA_NOTEXIST);
    sta_free_now(s);
}

void wifi_dead_stas_free(struct ieee80211_local *local)
{
    struct wifi_sta *s, *n;
    list_for_each_entry_safe(s, n, &local->dead_stas, dead) {
        list_del(&s->dead);
        sta_free_now(s);
    }
}

/* After a firmware restart (drvapi.c): the driver has forgotten the
   connection, and is told it again as mac80211 tells it — the channel
   context and the interface's place in it, the queues' timing, the link's
   and interface's settings, then the AP as a peer, one state at a time up
   to where it was. iwlwifi recognises a restart and keeps its own IDs. */
int wifi_mlme_replay(struct ieee80211_sub_if_data *sdata)
{
    struct ieee80211_local *local = sdata->local;
    struct ieee80211_hw *hw = &local->hw;
    struct wifi_conn *c = &sdata->conn;
    int err = 0;
    /* The timing first: activating the link sends it (a driver may have
       lost it with the firmware). */
    wifi_default_edca(sdata, sdata->vif.bss_conf.qos);
    if (c->ctx) {
        err = local->ops->add_chanctx(hw, &c->ctx->conf);
        if (!err)
            err = local->ops->assign_vif_chanctx(hw, &sdata->vif, &sdata->vif.bss_conf, &c->ctx->conf);
        if (err)
            return err;
    } else if (c->chan && local->ops->config) {
        local->ops->config(hw, -1, IEEE80211_CONF_CHANGE_CHANNEL);
    }
    if (c->ap)                          /* (slot and preamble: only once associated) */
        wifi_bss_changed(sdata, BSS_CHANGED_BASIC_RATES | BSS_CHANGED_BEACON_INT | BSS_CHANGED_BSSID |
                                (sdata->vif.cfg.assoc ? BSS_CHANGED_ASSOC | BSS_CHANGED_QOS |
                                 BSS_CHANGED_ERP_CTS_PROT | BSS_CHANGED_ERP_PREAMBLE |
                                 BSS_CHANGED_ERP_SLOT : 0));
    if (c->ap) {
        enum ieee80211_sta_state was = c->ap->state;
        c->ap->state = IEEE80211_STA_NOTEXIST;
        err = sta_move(c->ap, was);
    }
    return err;
}

/* Frames that waited in our queues while the driver restarted. */
void wifi_mlme_wake(struct ieee80211_sub_if_data *sdata)
{
    struct ieee80211_local *local = sdata->local;
    struct wifi_sta *ap = sdata->conn.ap;
    if (!ap || !local->ops->wake_tx_queue)
        return;
    for (int t = 0; t <= IEEE80211_NUM_TIDS; t++)
        if (ap->txq[t] && !skb_queue_empty(&ap->txq[t]->frames))
            local->ops->wake_tx_queue(&local->hw, &ap->txq[t]->txq);
}

/* --- the channel --------------------------------------------------------------------------- */

static int chan_assign(struct ieee80211_sub_if_data *sdata, struct ieee80211_channel *chan)
{
    struct ieee80211_local *local = sdata->local;
    struct ieee80211_hw *hw = &local->hw;
    struct cfg80211_chan_def def;
    cfg80211_chandef_create(&def, chan, NL80211_CHAN_NO_HT);
    sdata->vif.bss_conf.chanreq.oper = def;

    if (!local->ops->add_chanctx) {     /* a driver without channel contexts */
        hw->conf.chandef = def;
        return local->ops->config ? local->ops->config(hw, -1, IEEE80211_CONF_CHANGE_CHANNEL) : 0;
    }
    struct wifi_chanctx *ctx = kzalloc(sizeof(*ctx) + hw->chanctx_data_size, GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;
    ctx->conf.def = def;
    ctx->conf.min_def = def;
    ctx->conf.rx_chains_static = ctx->conf.rx_chains_dynamic = 1;
    int err = local->ops->add_chanctx(hw, &ctx->conf);
    if (err) {
        kfree(ctx);
        return err;
    }
    err = local->ops->assign_vif_chanctx(hw, &sdata->vif, &sdata->vif.bss_conf, &ctx->conf);
    if (err) {
        local->ops->remove_chanctx(hw, &ctx->conf);
        kfree(ctx);
        return err;
    }
    sdata->vif.bss_conf.chanctx_conf = &ctx->conf;
    sdata->conn.ctx = ctx;
    /* As mac80211 follows an assignment: the transmit power the channel
       allows, and the interface is no longer idle. */
    sdata->vif.bss_conf.txpower = chan->max_power;
    sdata->vif.cfg.idle = false;
    wifi_bss_changed(sdata, BSS_CHANGED_TXPOWER | BSS_CHANGED_IDLE);
    return 0;
}

static void chan_release(struct ieee80211_sub_if_data *sdata)
{
    struct ieee80211_local *local = sdata->local;
    struct wifi_chanctx *ctx = sdata->conn.ctx;
    if (!ctx)
        return;
    if (!local->hw_lost) {
        local->ops->unassign_vif_chanctx(&local->hw, &sdata->vif, &sdata->vif.bss_conf, &ctx->conf);
        local->ops->remove_chanctx(&local->hw, &ctx->conf);
    }
    sdata->vif.bss_conf.chanctx_conf = NULL;
    kfree(ctx);
    sdata->conn.ctx = NULL;
    sdata->vif.cfg.idle = true;
    wifi_bss_changed(sdata, BSS_CHANGED_IDLE);
}

/* --- management frames ----------------------------------------------------------------------- */

/* A management frame to the AP, with `body` bytes of room after the header. */
static struct sk_buff *mgmt_new(struct ieee80211_sub_if_data *sdata, u16 stype, size_t body)
{
    unsigned int head = sdata->local->hw.extra_tx_headroom;
    struct sk_buff *skb = alloc_skb(head + 24 + (unsigned int)body, GFP_KERNEL);
    if (!skb)
        return NULL;
    skb_reserve(skb, head);
    struct ieee80211_hdr_3addr *h = skb_put_zero(skb, 24);
    h->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | stype);
    ether_addr_copy(h->addr1, sdata->conn.bssid);
    ether_addr_copy(h->addr2, sdata->vif.addr);
    ether_addr_copy(h->addr3, sdata->conn.bssid);
    h->seq_ctrl = cpu_to_le16(IEEE80211_SN_TO_SEQ(sdata->conn.seq++));
    return skb;
}

/* Hand a management frame to the driver: the peer's management queue if
   the driver wants those queued, its tx() otherwise. */
static void mgmt_tx(struct ieee80211_sub_if_data *sdata, struct sk_buff *skb)
{
    struct ieee80211_local *local = sdata->local;
    struct wifi_sta *ap = sdata->conn.ap;
    struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
    memset(info, 0, sizeof *info);
    info->flags = IEEE80211_TX_CTL_REQ_TX_STATUS | IEEE80211_TX_INTFL_DONT_ENCRYPT;
    if (sdata->conn.chan && sdata->conn.chan->band != NL80211_BAND_2GHZ)
        info->flags |= IEEE80211_TX_CTL_NO_CCK_RATE;
    info->band = sdata->conn.chan ? sdata->conn.chan->band : 0;
    info->hw_queue = sdata->vif.hw_queue[IEEE80211_AC_VO];
    info->control.vif = &sdata->vif;
    info->control.rates[0].idx = 0;
    info->control.rates[0].count = 1;
    info->control.rates[1].idx = -1;

    if (ap && ap->state >= IEEE80211_STA_NONE && ap->txq[IEEE80211_NUM_TIDS] &&
        ieee80211_hw_check(&local->hw, STA_MMPDU_TXQ)) {
        skb_queue_tail(&ap->txq[IEEE80211_NUM_TIDS]->frames, skb);
        local->ops->wake_tx_queue(&local->hw, &ap->txq[IEEE80211_NUM_TIDS]->txq);
        return;
    }
    struct ieee80211_tx_control control = {
        .sta = ap && ap->state >= IEEE80211_STA_NONE ? &ap->sta : NULL,
    };
    local->ops->tx(&local->hw, &control, skb);
}

/* Before and after a frame exchange: the driver keeps the radio on the
   channel meanwhile (iwlwifi books air time with its firmware). */
static void prepare_tx(struct ieee80211_sub_if_data *sdata, u16 subtype)
{
    struct ieee80211_prep_tx_info info = { .subtype = subtype };   /* (as mac80211: no duration) */
    if (sdata->local->ops->mgd_prepare_tx)
        sdata->local->ops->mgd_prepare_tx(&sdata->local->hw, &sdata->vif, &info);
}

static void complete_tx(struct ieee80211_sub_if_data *sdata, u16 subtype, bool success)
{
    struct ieee80211_prep_tx_info info = { .subtype = subtype, .success = success };
    if (sdata->local->ops->mgd_complete_tx)
        sdata->local->ops->mgd_complete_tx(&sdata->local->hw, &sdata->vif, &info);
}

static void arm_timeout(struct ieee80211_sub_if_data *sdata)
{
    queue_delayed_work(system_wq, &sdata->conn.timeout, msecs_to_jiffies(MLME_TIMEOUT_MS));
}

static void send_auth(struct ieee80211_sub_if_data *sdata)
{
    struct sk_buff *skb = mgmt_new(sdata, IEEE80211_STYPE_AUTH, 6);
    if (skb) {
        struct ieee80211_mgmt *m = (void *)skb->data;
        skb_put_zero(skb, 6);
        m->u.auth.auth_alg = cpu_to_le16(WLAN_AUTH_OPEN);
        m->u.auth.auth_transaction = cpu_to_le16(1);
        mgmt_tx(sdata, skb);
    }
    arm_timeout(sdata);
}

/* Our rates on the AP's band, as Supported Rates (8) and Extended Supported
   Rates (the rest); a rate the AP calls basic keeps its "basic" bit. */
static void put_rates(struct sk_buff *skb, const struct ieee80211_supported_band *sb, u32 basic)
{
    u8 r[32];
    int n = 0;
    for (int i = 0; i < sb->n_bitrates && n < (int)sizeof r; i++)
        r[n++] = (u8)(sb->bitrates[i].bitrate / 5) | (basic & BIT(i) ? 0x80 : 0);
    u8 *p = skb_put(skb, 2 + (unsigned int)min(n, 8));
    p[0] = WLAN_EID_SUPP_RATES;
    p[1] = (u8)min(n, 8);
    memcpy(p + 2, r, (size_t)min(n, 8));
    if (n > 8) {
        p = skb_put(skb, 2 + (unsigned int)(n - 8));
        p[0] = WLAN_EID_EXT_SUPP_RATES;
        p[1] = (u8)(n - 8);
        memcpy(p + 2, r + 8, (size_t)(n - 8));
    }
}

static void send_assoc(struct ieee80211_sub_if_data *sdata)
{
    struct wifi_conn *c = &sdata->conn;
    const struct ieee80211_supported_band *sb = sdata->local->hw.wiphy->bands[c->chan->band];
    struct sk_buff *skb = mgmt_new(sdata, IEEE80211_STYPE_ASSOC_REQ, 4 + 2 + c->ssid_len + 40 + c->ie_len);
    if (!skb) {
        arm_timeout(sdata);
        return;
    }
    struct ieee80211_mgmt *m = (void *)skb->data;
    u16 capab = WLAN_CAPABILITY_ESS | (c->capability & WLAN_CAPABILITY_PRIVACY);
    if (c->chan->band == NL80211_BAND_2GHZ)
        capab |= WLAN_CAPABILITY_SHORT_SLOT_TIME | WLAN_CAPABILITY_SHORT_PREAMBLE;
    skb_put_zero(skb, 4);
    m->u.assoc_req.capab_info = cpu_to_le16(capab);
    m->u.assoc_req.listen_interval = cpu_to_le16(10);   /* beacon intervals we may sleep */
    size_t ies_at = skb->len;
    u8 *p = skb_put(skb, 2 + c->ssid_len);
    p[0] = WLAN_EID_SSID;
    p[1] = c->ssid_len;
    memcpy(p + 2, c->ssid, c->ssid_len);
    put_rates(skb, sb, c->basic_rates);
    if (c->ie_len)
        skb_put_data(skb, c->ie, (unsigned int)c->ie_len);
    kfree(c->req_ies);
    c->req_ies_len = skb->len - ies_at;
    c->req_ies = kmemdup(skb->data + ies_at, c->req_ies_len, GFP_KERNEL);
    mgmt_tx(sdata, skb);
    arm_timeout(sdata);
}

static void send_deauth(struct ieee80211_sub_if_data *sdata, u16 reason)
{
    struct sk_buff *skb = mgmt_new(sdata, IEEE80211_STYPE_DEAUTH, 2);
    if (!skb)
        return;
    struct ieee80211_mgmt *m = (void *)skb->data;
    skb_put_zero(skb, 2);
    m->u.deauth.reason_code = cpu_to_le16(reason);
    prepare_tx(sdata, IEEE80211_STYPE_DEAUTH);
    mgmt_tx(sdata, skb);
    complete_tx(sdata, IEEE80211_STYPE_DEAUTH, true);
}

/* --- the states ---------------------------------------------------------------------------- */

/* Undo whatever the connection had set up, back to idle. */
static void teardown(struct ieee80211_sub_if_data *sdata)
{
    struct wifi_conn *c = &sdata->conn;
    cancel_delayed_work(&c->timeout);   /* (not _sync: we may be that work) */
    netif_carrier_off(sdata->dev);
    ipv4_link_down(sdata->dev);         /* (IPv4 leaves the link, if it was on it) */
    wifi_keys_free(sdata);              /* (keys before the peer they belong to) */
    sta_free(c->ap);
    c->ap = NULL;
    u64 changed = 0;
    if (sdata->vif.cfg.assoc) {
        sdata->vif.cfg.assoc = false;
        sdata->vif.cfg.aid = 0;
        changed |= BSS_CHANGED_ASSOC;
    }
    if (!is_zero_ether_addr(sdata->link_bssid)) {
        eth_zero_addr(sdata->link_bssid);  /* (the pointer stays: drivers copy from it) */
        sdata->vif.bss_conf.use_short_slot = false;
        sdata->vif.bss_conf.use_short_preamble = false;
        changed |= BSS_CHANGED_BSSID;
    }
    wifi_bss_changed(sdata, changed);
    chan_release(sdata);
    kfree(c->ie);
    kfree(c->req_ies);
    kfree(c->resp_ies);
    c->ie = c->req_ies = c->resp_ies = NULL;
    c->ie_len = c->req_ies_len = c->resp_ies_len = 0;
    c->chan = NULL;
    c->state = MLME_IDLE;
}

/* The connection attempt failed: tidy up and say so. */
static void fail(struct ieee80211_sub_if_data *sdata, u16 status, bool timed_out)
{
    teardown(sdata);
    nl80211_connect_result(sdata, status, timed_out);
}

/* The rates an AP's Supported Rates elements list that our band has, as a
   bitmap into the band's list; *basic gets those marked basic (bit 7). */
static u32 ap_rates(const struct ieee80211_supported_band *sb, const u8 *ies, size_t len, u32 *basic)
{
    u32 rates = 0;
    if (basic)
        *basic = 0;
    for (int e = WLAN_EID_SUPP_RATES; e; e = e == WLAN_EID_SUPP_RATES ? WLAN_EID_EXT_SUPP_RATES : 0) {
        const struct element *el = cfg80211_find_elem((u8)e, ies, (int)len);
        for (int k = 0; el && k < el->datalen; k++)
            for (int i = 0; i < sb->n_bitrates; i++)
                if ((el->data[k] & 0x7f) * 5 == sb->bitrates[i].bitrate) {
                    rates |= BIT(i);
                    if (basic && (el->data[k] & 0x80))
                        *basic |= BIT(i);
                }
    }
    return rates;
}

/* Start joining bss: channel, the AP as a peer, then authentication. */
static int start_auth(struct ieee80211_sub_if_data *sdata, const struct cfg80211_bss *bss)
{
    struct wifi_conn *c = &sdata->conn;
    const struct ieee80211_supported_band *sb = sdata->local->hw.wiphy->bands[bss->channel->band];
    ether_addr_copy(c->bssid, bss->bssid);
    c->chan = bss->channel;
    c->capability = bss->capability;
    c->beacon_int = bss->beacon_interval;
    const struct element *tim = cfg80211_find_elem(WLAN_EID_TIM, bss->ies->data, bss->ies->len);
    c->dtim_period = tim && tim->datalen >= 2 ? tim->data[1] : 1;

    /* The AP's rates, and its basic ones (every station must speak those),
       as bitmaps into our list for the band. */
    u32 bss_rates = ap_rates(sb, bss->ies->data, bss->ies->len, &c->basic_rates);

    /* mac80211's order, which iwlwifi's firmware depends on (net/mac80211/
       mlme.c, ieee80211_prep_connection): the AP's BSSID, beacon interval,
       basic rates and beacon timing are filled in first, so the driver
       never sees half of it; then the channel (which activates the link);
       then those are announced, before the AP becomes a peer; and nothing
       more until we are associated. The DTIM period stays 0 ("no beacon
       heard yet") until then, and slot time and preamble come with the
       association: a change to them now is a link change iwlwifi's
       firmware rejected before association (docs/LOG.md). */
    struct ieee80211_bss_conf *bc = &sdata->vif.bss_conf;
    ether_addr_copy(sdata->link_bssid, c->bssid);
    bc->beacon_int = c->beacon_int;
    bc->basic_rates = c->basic_rates;
    bc->dtim_period = 0;
    bc->sync_tsf = bss->ies->tsf;
    bc->sync_device_ts = container_of(bss, struct wifi_bss, pub)->device_ts;
    bc->sync_dtim_count = tim && tim->datalen >= 1 ? tim->data[0] : 0;
    int err = chan_assign(sdata, c->chan);
    if (err) {
        eth_zero_addr(sdata->link_bssid);
        return err;
    }
    wifi_bss_changed(sdata, BSS_CHANGED_BSSID | BSS_CHANGED_BASIC_RATES | BSS_CHANGED_BEACON_INT);
    c->ap = sta_new(sdata, c->bssid);
    /* What the AP can receive, from its beacon, before it becomes a peer
       (mac80211: ieee80211_mgd_setup_link_sta): iwlwifi sets up the
       firmware's rate control as soon as the peer exists, and its
       firmware rejected an empty rate set (docs/LOG.md). Refined from the
       association answer. */
    if (c->ap)
        c->ap->sta.deflink.supp_rates[c->chan->band] = bss_rates ? bss_rates : 1;
    if (!c->ap || (err = sta_move(c->ap, IEEE80211_STA_NONE))) {
        sta_free(c->ap);
        c->ap = NULL;
        eth_zero_addr(sdata->link_bssid);
        wifi_bss_changed(sdata, BSS_CHANGED_BSSID);
        chan_release(sdata);
        return err ? err : -ENOMEM;
    }
    c->state = MLME_AUTH;
    c->tries = 1;
    prepare_tx(sdata, IEEE80211_STYPE_AUTH);
    send_auth(sdata);
    return 0;
}

const struct cfg80211_bss *wifi_bss_find(struct ieee80211_local *local, const u8 *ssid, u8 ssid_len,
                                         const u8 *bssid, u32 freq)
{
    const struct cfg80211_bss *best = NULL;
    for (int i = 0; i < WIFI_BSS_MAX; i++) {
        const struct wifi_bss *b = &local->bss[i];
        if (!b->used || time_after(jiffies, b->last_seen + WIFI_BSS_EXPIRE))
            continue;
        if ((bssid && !ether_addr_equal(bssid, b->pub.bssid)) ||
            (freq && b->pub.channel->center_freq != freq))
            continue;
        const struct element *s = cfg80211_find_elem(WLAN_EID_SSID, b->pub.ies->data, b->pub.ies->len);
        if (!s || s->datalen != ssid_len || memcmp(s->data, ssid, ssid_len))
            continue;
        if (!best || b->pub.signal > best->signal)
            best = &b->pub;             /* the strongest of its access points */
    }
    return best;
}

/* A scan of our own, for a network not (or no longer) in the table. */
static int scan_for(struct ieee80211_sub_if_data *sdata)
{
    struct wiphy *wiphy = sdata->local->hw.wiphy;
    u32 n = 0;
    for (int b = 0; b < NUM_NL80211_BANDS; b++)
        if (wiphy->bands[b])
            n += (u32)wiphy->bands[b]->n_channels;
    struct cfg80211_scan_request *req = kzalloc(sizeof(*req) + n * sizeof(req->channels[0]) +
                                                sizeof(struct cfg80211_ssid), GFP_KERNEL);
    if (!req)
        return -ENOMEM;
    for (int b = 0; b < NUM_NL80211_BANDS; b++)
        for (int i = 0; wiphy->bands[b] && i < wiphy->bands[b]->n_channels; i++) {
            struct ieee80211_channel *ch = &wiphy->bands[b]->channels[i];
            if (!(ch->flags & IEEE80211_CHAN_DISABLED) &&
                (!sdata->conn.freq || ch->center_freq == sdata->conn.freq))
                req->channels[req->n_channels++] = ch;
        }
    req->ssids = (struct cfg80211_ssid *)&req->channels[n];
    memcpy(req->ssids[0].ssid, sdata->conn.ssid, sdata->conn.ssid_len);    /* ask for it by name */
    req->ssids[0].ssid_len = sdata->conn.ssid_len;
    req->n_ssids = 1;
    int err = req->n_channels ? wifi_scan(sdata, req) : -EINVAL;
    if (err)
        kfree(req);
    return err;
}

int wifi_connect(struct ieee80211_sub_if_data *sdata, const struct wifi_connect_params *p)
{
    struct wifi_conn *c = &sdata->conn;
    if (!sdata->in_driver)
        return -ENETDOWN;
    if (c->state != MLME_IDLE)
        return -EALREADY;
    if (!p->ssid_len || p->ssid_len > IEEE80211_MAX_SSID_LEN)
        return -EINVAL;
    memcpy(c->ssid, p->ssid, p->ssid_len);
    c->ssid_len = p->ssid_len;
    c->bssid_fixed = p->bssid != NULL;
    if (p->bssid)
        ether_addr_copy(c->bssid, p->bssid);
    c->freq = p->freq;
    c->ie = p->ie_len ? kmemdup(p->ie, p->ie_len, GFP_KERNEL) : NULL;
    c->ie_len = c->ie ? p->ie_len : 0;
    c->privacy = p->privacy;
    c->control_port = p->control_port;
    c->control_port_no_encrypt = p->control_port_no_encrypt;
    c->control_port_over_nl80211 = p->control_port_over_nl80211;
    c->control_port_ethertype = p->control_port_ethertype ? p->control_port_ethertype : 0x888e;
    c->owner = p->owner;
    memcpy(sdata->vif.cfg.ssid, c->ssid, c->ssid_len);
    sdata->vif.cfg.ssid_len = c->ssid_len;

    const struct cfg80211_bss *bss = wifi_bss_find(sdata->local, c->ssid, c->ssid_len,
                                                   c->bssid_fixed ? c->bssid : NULL, c->freq);
    int err;
    if (bss) {
        err = start_auth(sdata, bss);
    } else {
        err = scan_for(sdata);
        if (!err)
            c->state = MLME_SCAN;
    }
    if (err) {
        kfree(c->ie);
        c->ie = NULL;
        c->ie_len = 0;
    }
    return err;
}

/* scan.c: a scan ended; ours, if we were waiting to connect. */
void wifi_mlme_scan_done(struct ieee80211_sub_if_data *sdata, bool aborted)
{
    struct wifi_conn *c = &sdata->conn;
    if (c->state != MLME_SCAN)
        return;
    const struct cfg80211_bss *bss = aborted ? NULL :
        wifi_bss_find(sdata->local, c->ssid, c->ssid_len, c->bssid_fixed ? c->bssid : NULL, c->freq);
    if (!bss || start_auth(sdata, bss))
        fail(sdata, WLAN_STATUS_UNSPECIFIED_FAILURE, false);
}

int wifi_disconnect(struct ieee80211_sub_if_data *sdata, u16 reason, bool locally)
{
    struct wifi_conn *c = &sdata->conn;
    if (c->state == MLME_IDLE)
        return -ENOTCONN;
    bool was_connected = c->state == MLME_CONNECTED;
    if (c->state == MLME_SCAN) {
        c->state = MLME_IDLE;           /* (so the scan's end doesn't carry on) */
        wifi_scan_cancel(sdata);
    }
    if (c->state >= MLME_AUTH && locally && !sdata->local->hw_lost)
        send_deauth(sdata, reason);
    teardown(sdata);
    if (was_connected)
        nl80211_disconnected(sdata, reason, !locally);
    else
        nl80211_connect_result(sdata, WLAN_STATUS_UNSPECIFIED_FAILURE, false);
    return 0;
}

/* wpa_supplicant says the 4-way handshake is done (or undone): the port
   opens for all traffic, and IPv4 may use the link. */
int wifi_authorize(struct ieee80211_sub_if_data *sdata, bool on)
{
    struct wifi_sta *ap = sdata->conn.ap;
    if (sdata->conn.state != MLME_CONNECTED || !ap)
        return -ENOLINK;
    bool was = ap->state == IEEE80211_STA_AUTHORIZED;
    int err = sta_move(ap, on ? IEEE80211_STA_AUTHORIZED : IEEE80211_STA_ASSOC);
    if (err)
        return err;
    if (on && !was)
        ipv4_link_up(sdata->dev);
    else if (!on && was)
        ipv4_link_down(sdata->dev);
    return 0;
}

/* The driver lost the AP: beacons stopped, or its firmware restarted.
   Leave, as if we had decided to (wpa_supplicant hears "disconnected,
   locally" and looks for the network again). The driver may call from any
   context, so the leaving happens in wiphy work. */
static void lost_work(struct wiphy *wiphy, struct wiphy_work *w)
{
    (void)wiphy;
    struct ieee80211_sub_if_data *sdata = container_of(w, struct ieee80211_sub_if_data, lost_work);
    wifi_disconnect(sdata, sdata->lost_reason, true);  /* (-ENOTCONN if already gone) */
}

void wifi_connection_lost(struct ieee80211_sub_if_data *sdata, u16 reason)
{
    sdata->lost_reason = reason;
    wiphy_work_queue(sdata->local->hw.wiphy, &sdata->lost_work);
}

/* No answer in time: try again, or give up. */
static void timeout_work(struct work_struct *w)
{
    struct wifi_conn *c = container_of(to_delayed_work(w), struct wifi_conn, timeout);
    struct ieee80211_sub_if_data *sdata = container_of(c, struct ieee80211_sub_if_data, conn);
    struct wiphy *wiphy = sdata->local->hw.wiphy;
    wiphy_lock(wiphy);
    if (c->state == MLME_AUTH || c->state == MLME_ASSOC) {
        bool auth = c->state == MLME_AUTH;
        if (c->tries++ < MLME_TRIES) {
            /* Air time again for each try, as mac80211 asks before each. */
            prepare_tx(sdata, auth ? IEEE80211_STYPE_AUTH : IEEE80211_STYPE_ASSOC_REQ);
            if (auth)
                send_auth(sdata);
            else
                send_assoc(sdata);
        } else {
            complete_tx(sdata, auth ? IEEE80211_STYPE_AUTH : IEEE80211_STYPE_ASSOC_REQ, false);
            fail(sdata, WLAN_STATUS_UNSPECIFIED_FAILURE, true);
        }
    }
    wiphy_unlock(wiphy);
}

/* The association answer: we are in. */
static void associated(struct ieee80211_sub_if_data *sdata, const struct ieee80211_mgmt *m, size_t len)
{
    struct wifi_conn *c = &sdata->conn;
    struct wifi_sta *ap = c->ap;
    size_t base = offsetof(struct ieee80211_mgmt, u.assoc_resp.variable);
    const u8 *ies = m->u.assoc_resp.variable;
    size_t ielen = len - base;
    const struct ieee80211_supported_band *sb = sdata->local->hw.wiphy->bands[c->chan->band];

    cancel_delayed_work(&c->timeout);
    complete_tx(sdata, IEEE80211_STYPE_ASSOC_REQ, true);
    kfree(c->resp_ies);
    c->resp_ies = ielen ? kmemdup(ies, ielen, GFP_KERNEL) : NULL;
    c->resp_ies_len = c->resp_ies ? ielen : 0;

    /* The AP's rates, now from its answer (kept from the beacon if none). */
    u32 rates = ap_rates(sb, ies, ielen, NULL);
    if (rates)
        ap->sta.deflink.supp_rates[c->chan->band] = rates;
    if (sta_move(ap, IEEE80211_STA_ASSOC)) {
        fail(sdata, WLAN_STATUS_UNSPECIFIED_FAILURE, false);
        return;
    }
    /* An open network is usable at once (no 802.1X): authorized now,
       before the association is announced, as mac80211 orders it. A
       protected one when wpa_supplicant has the keys in and says so
       (NL80211_CMD_SET_STATION). */
    bool open = !c->control_port && !c->privacy && !sta_move(ap, IEEE80211_STA_AUTHORIZED);
    sdata->vif.bss_conf.dtim_period = c->dtim_period ? c->dtim_period : 1;
    sdata->vif.cfg.assoc = true;
    sdata->vif.cfg.aid = le16_to_cpu(m->u.assoc_resp.aid) & 0x3fff;
    /* Our data frames are plain (no QoS: we don't use the AP's WMM yet),
       so the driver gets plain DCF timing, as mac80211 gives it for an AP
       without WMM. */
    wifi_default_edca(sdata, ap->sta.wme);
    /* Slot time and preamble from the AP's capabilities, now (mac80211
       sets them on association). */
    u16 capab = le16_to_cpu(m->u.assoc_resp.capab_info);
    sdata->vif.bss_conf.use_short_slot = c->chan->band != NL80211_BAND_2GHZ ||
                                         (capab & WLAN_CAPABILITY_SHORT_SLOT_TIME);
    sdata->vif.bss_conf.use_short_preamble = (capab & WLAN_CAPABILITY_SHORT_PREAMBLE) != 0;
    wifi_bss_changed(sdata, BSS_CHANGED_ASSOC | BSS_CHANGED_QOS | BSS_CHANGED_ERP_SLOT |
                            BSS_CHANGED_ERP_PREAMBLE);
    c->state = MLME_CONNECTED;
    ap->connected_at = ap->last_rx = jiffies;

    netif_carrier_on(sdata->dev);
    nl80211_connect_result(sdata, WLAN_STATUS_SUCCESS, false);
    if (open)
        ipv4_link_up(sdata->dev);       /* the link carries IP now */
}

/* A management frame from our AP (with the wiphy lock held). */
void wifi_mlme_rx(struct ieee80211_sub_if_data *sdata, struct sk_buff *skb)
{
    struct wifi_conn *c = &sdata->conn;
    const struct ieee80211_mgmt *m = (const void *)skb->data;
    __le16 fc = m->frame_control;
    if (c->state < MLME_AUTH || !ether_addr_equal(m->bssid, c->bssid) || skb->len < 24)
        return;
    if (c->ap)
        c->ap->last_rx = jiffies;

    if (ieee80211_is_auth(fc) && c->state == MLME_AUTH &&
        skb->len >= offsetof(struct ieee80211_mgmt, u.auth.variable)) {
        if (le16_to_cpu(m->u.auth.auth_alg) != WLAN_AUTH_OPEN ||
            le16_to_cpu(m->u.auth.auth_transaction) != 2)
            return;                     /* not the answer we wait for */
        cancel_delayed_work(&c->timeout);
        complete_tx(sdata, IEEE80211_STYPE_AUTH, le16_to_cpu(m->u.auth.status_code) == 0);
        if (m->u.auth.status_code) {
            fail(sdata, le16_to_cpu(m->u.auth.status_code), false);
            return;
        }
        if (sta_move(c->ap, IEEE80211_STA_AUTH)) {
            fail(sdata, WLAN_STATUS_UNSPECIFIED_FAILURE, false);
            return;
        }
        c->state = MLME_ASSOC;
        c->tries = 1;
        prepare_tx(sdata, IEEE80211_STYPE_ASSOC_REQ);
        send_assoc(sdata);
    } else if ((ieee80211_is_assoc_resp(fc) || ieee80211_is_reassoc_resp(fc)) &&
               c->state == MLME_ASSOC &&
               skb->len >= offsetof(struct ieee80211_mgmt, u.assoc_resp.variable)) {
        u16 status = le16_to_cpu(m->u.assoc_resp.status_code);
        if (status) {
            cancel_delayed_work(&c->timeout);
            complete_tx(sdata, IEEE80211_STYPE_ASSOC_REQ, false);
            fail(sdata, status, false);
            return;
        }
        associated(sdata, m, skb->len);
    } else if ((ieee80211_is_deauth(fc) || ieee80211_is_disassoc(fc)) && skb->len >= 26) {
        u16 reason = le16_to_cpu(m->u.deauth.reason_code);
        bool was_connected = c->state == MLME_CONNECTED;
        teardown(sdata);
        if (was_connected)
            nl80211_disconnected(sdata, reason, true);
        else
            nl80211_connect_result(sdata, WLAN_STATUS_UNSPECIFIED_FAILURE, false);
    }
}

static void mlme_work(struct wiphy *wiphy, struct wiphy_work *w)
{
    struct ieee80211_sub_if_data *sdata = container_of(w, struct ieee80211_sub_if_data, mlme_work);
    struct sk_buff *skb;
    (void)wiphy;
    while ((skb = skb_dequeue(&sdata->mlme_queue))) {
        wifi_mlme_rx(sdata, skb);
        kfree_skb(skb);
    }
}

int wifi_mlme_init(struct ieee80211_sub_if_data *sdata)
{
    INIT_DELAYED_WORK(&sdata->conn.timeout, timeout_work);
    skb_queue_head_init(&sdata->mlme_queue);
    wiphy_work_init(&sdata->mlme_work, mlme_work);
    wiphy_work_init(&sdata->lost_work, lost_work);
    if (sdata->local->ops->wake_tx_queue) {     /* the interface's own queue (multicast) */
        struct wifi_txq *q = txq_new(sdata, NULL, 0);
        if (!q)
            return -ENOMEM;
        sdata->vif.txq = &q->txq;
    }
    return 0;
}

void wifi_mlme_free(struct ieee80211_sub_if_data *sdata)
{
    cancel_delayed_work_sync(&sdata->conn.timeout);
    skb_queue_purge(&sdata->mlme_queue);
    if (sdata->vif.txq) {
        struct wifi_txq *q = container_of(sdata->vif.txq, struct wifi_txq, txq);
        skb_queue_purge(&q->frames);
        kfree(q);
        sdata->vif.txq = NULL;
    }
}

struct ieee80211_sub_if_data *wifi_sdata_by_addr(struct ieee80211_local *local, const u8 *addr)
{
    struct ieee80211_sub_if_data *sdata;
    list_for_each_entry(sdata, &local->interfaces, list)
        if (ether_addr_equal(sdata->vif.addr, addr))
            return sdata;
    return NULL;
}
