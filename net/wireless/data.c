/*
 * data.c — Ethernet frames over Wi-Fi (Linux: mac80211's tx.c and rx.c,
 * the data parts). The network stack sends and receives Ethernet frames;
 * on the air they are 802.11 data frames:
 *
 *   Ethernet:  dest(6) src(6) type(2) payload
 *   802.11:    header(24: to-DS, addr1 = the AP, addr2 = us, addr3 = dest)
 *              LLC/SNAP(8: aa aa 03 00 00 00, then the type) payload
 *
 * and back (from-DS: addr1 = us or a group, addr2 = the AP, addr3 = the
 * sender). Until wpa_supplicant authorizes the link only its EAPOL frames
 * pass, both ways (the "control port").
 *
 * Sending is O(1) per frame: convert in place (the device asks for the
 * headroom), hand to the driver's queue for that priority, or its tx().
 */
#include "core.h"

/* Make room for `need` more bytes in front, without a copy when the
   network stack left enough headroom (it does: dev->needed_headroom). */
static int headroom(struct sk_buff *skb, unsigned int need)
{
    if (skb_headroom(skb) >= need && !skb_cloned(skb))
        return 0;
    return pskb_expand_head(skb, need > skb_headroom(skb) ? need - skb_headroom(skb) : 0, 0,
                            GFP_ATOMIC);
}

int wifi_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct ieee80211_sub_if_data *sdata = netdev_priv(dev);
    struct ieee80211_local *local = sdata->local;
    struct wifi_conn *c = &sdata->conn;
    struct wifi_sta *ap = c->ap;

    if (c->state != MLME_CONNECTED || !ap || skb->len < ETH_HLEN)
        goto drop;
    u8 da[ETH_ALEN], sa[ETH_ALEN];
    memcpy(da, skb->data, ETH_ALEN);
    memcpy(sa, skb->data + ETH_ALEN, ETH_ALEN);
    u16 type = (u16)(skb->data[12] << 8 | skb->data[13]);
    bool eapol = type == c->control_port_ethertype;
    if (ap->state < IEEE80211_STA_AUTHORIZED && !eapol)
        goto drop;                      /* the port is closed: EAPOL only */

    unsigned int hdrlen = ap->sta.wme ? 26 : 24;
    if (headroom(skb, local->hw.extra_tx_headroom + hdrlen + 8 - ETH_HLEN))
        goto drop;
    skb_pull(skb, ETH_HLEN);
    u8 *llc = skb_push(skb, 8);
    memcpy(llc, rfc1042_header, 6);
    llc[6] = (u8)(type >> 8);
    llc[7] = (u8)type;

    struct ieee80211_qos_hdr *h = skb_push(skb, hdrlen);
    memset(h, 0, hdrlen);
    u16 fc = IEEE80211_FTYPE_DATA | IEEE80211_FCTL_TODS |
             (ap->sta.wme ? IEEE80211_STYPE_QOS_DATA : IEEE80211_STYPE_DATA);
    h->frame_control = cpu_to_le16(fc);
    ether_addr_copy(h->addr1, c->bssid);
    ether_addr_copy(h->addr2, sa);
    ether_addr_copy(h->addr3, da);
    int tid = ap->sta.wme ? (int)(skb->priority & 7) : 0;
    if (ap->sta.wme)
        h->qos_ctrl = cpu_to_le16((u16)tid);
    h->seq_ctrl = cpu_to_le16(IEEE80211_SN_TO_SEQ(c->seq++));

    struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
    memset(info, 0, sizeof *info);
    info->band = c->chan->band;
    info->hw_queue = sdata->vif.hw_queue[ap->sta.wme ? tid_ac(tid) : IEEE80211_AC_BE];
    info->control.vif = &sdata->vif;
    info->control.rates[0].idx = 0;
    info->control.rates[0].count = 1;
    info->control.rates[1].idx = -1;
    if (eapol) {
        info->control.flags |= IEEE80211_TX_CTRL_PORT_CTRL_PROTO;
        info->flags |= IEEE80211_TX_CTL_REQ_TX_STATUS;
        if (c->control_port_no_encrypt)
            info->flags |= IEEE80211_TX_INTFL_DONT_ENCRYPT;
    }
    if (c->ptk && !(info->flags & IEEE80211_TX_INTFL_DONT_ENCRYPT) &&
        wifi_key_protect(sdata, skb, hdrlen))
        goto drop;

    ap->tx_packets++;
    ap->tx_bytes += skb->len;
    dev->stats.tx_packets++;
    dev->stats.tx_bytes += skb->len;
    if (local->ops->wake_tx_queue && ap->txq[tid]) {
        skb_queue_tail(&ap->txq[tid]->frames, skb);
        local->ops->wake_tx_queue(&local->hw, &ap->txq[tid]->txq);
    } else {
        struct ieee80211_tx_control control = { .sta = &ap->sta };
        local->ops->tx(&local->hw, &control, skb);
    }
    return NETDEV_TX_OK;
drop:
    dev->stats.tx_dropped++;
    kfree_skb(skb);
    return NETDEV_TX_OK;
}

/* The driver pulls the next frame of a queue it was woken for. */
struct sk_buff *ieee80211_tx_dequeue(struct ieee80211_hw *hw, struct ieee80211_txq *txq)
{
    (void)hw;
    return skb_dequeue(&container_of(txq, struct wifi_txq, txq)->frames);
}

/* A data frame from our AP: back to Ethernet, up to the network stack. */
void wifi_rx_data(struct ieee80211_local *local, struct sk_buff *skb)
{
    struct ieee80211_rx_status *st = IEEE80211_SKB_RXCB(skb);
    struct ieee80211_hdr *h = (void *)skb->data;
    __le16 fc = h->frame_control;
    if (!ieee80211_has_fromds(fc) || ieee80211_has_tods(fc) || !ieee80211_is_data_present(fc))
        goto drop;                      /* (only AP -> station frames that carry data) */

    /* From our AP: once connected, or already while associating — an AP
       sends the handshake's first EAPOL frame right behind its association
       answer, and the data path may see it before the MLME work has handled
       that answer (Linux's mac80211 lets it through the same way). Only
       EAPOL passes then: the port check below. */
    struct ieee80211_sub_if_data *sdata, *to = NULL;
    list_for_each_entry(sdata, &local->interfaces, list)
        if (sdata->conn.state >= MLME_ASSOC && sdata->conn.ap &&
            ether_addr_equal(h->addr2, sdata->conn.bssid) &&
            (ether_addr_equal(h->addr1, sdata->vif.addr) || is_multicast_ether_addr(h->addr1)))
            to = sdata;
    if (!to)
        goto drop;
    struct wifi_conn *c = &to->conn;
    struct wifi_sta *ap = c->ap;

    /* Protected frames: the hardware decrypted them (we have no software
       crypto); the cipher's header and trailer may still be there. */
    unsigned int hdrlen = ieee80211_hdrlen(fc), iv = 0, mic = 0;
    if (ieee80211_has_protected(fc)) {
        if (!(st->flag & RX_FLAG_DECRYPTED))
            goto drop;
        /* The key it used: the pairwise one, or the group key named in the
           header (its key-id bits), which tells the header and trailer sizes. */
        bool group = is_multicast_ether_addr(h->addr1);
        int idx = skb->len > hdrlen + 3 && !(st->flag & RX_FLAG_IV_STRIPPED) ?
                  skb->data[hdrlen + 3] >> 6 : 1;
        const struct ieee80211_key_conf *k = wifi_key_for_rx(to, group, idx);
        iv = st->flag & RX_FLAG_IV_STRIPPED ? 0 : k ? k->iv_len : IEEE80211_CCMP_HDR_LEN;
        mic = st->flag & RX_FLAG_MIC_STRIPPED ? 0 : k ? k->icv_len : IEEE80211_CCMP_MIC_LEN;
    }
    if (skb->len < hdrlen + iv + 8 + mic)
        goto drop;
    const u8 *llc = skb->data + hdrlen + iv;
    if (memcmp(llc, rfc1042_header, 6) && memcmp(llc, bridge_tunnel_header, 6))
        goto drop;                      /* (not an Ethernet type: nothing for the stack) */
    u16 type = (u16)(llc[6] << 8 | llc[7]);
    if (ap->state < IEEE80211_STA_AUTHORIZED && type != c->control_port_ethertype)
        goto drop;                      /* the port is closed: EAPOL only */

    u8 da[ETH_ALEN], sa[ETH_ALEN];
    ether_addr_copy(da, h->addr1);
    ether_addr_copy(sa, h->addr3);
    if (mic)
        skb_trim(skb, skb->len - mic);
    skb_pull(skb, hdrlen + iv + 6);     /* to the type */
    u8 *eth = skb_push(skb, 2 * ETH_ALEN);
    memcpy(eth, da, ETH_ALEN);
    memcpy(eth + ETH_ALEN, sa, ETH_ALEN);

    ap->rx_packets++;
    ap->rx_bytes += skb->len;
    ap->last_rx = jiffies;
    ap->last_signal = st->signal;
    to->dev->stats.rx_packets++;
    to->dev->stats.rx_bytes += skb->len;
    memset(skb->cb, 0, sizeof skb->cb);
    skb->protocol = eth_type_trans(skb, to->dev);
    netif_rx(skb);
    return;
drop:
    kfree_skb(skb);
}
