/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * key.c — the keys wpa_supplicant installs after the 4-way handshake
 * (Linux: mac80211's key.c): the pairwise key (PTK's temporal key, for
 * frames between us and the AP) and group keys (for the AP's broadcasts).
 *
 * The hardware encrypts and decrypts: each key goes to the driver
 * (set_key), and frames then carry a pointer to it (tx info hw_key). MyOS
 * has no software crypto — a driver that can't take a key can't do WPA2.
 * If the driver asks (GENERATE_IV / PUT_IV_SPACE), we put the CCMP header
 * in front of the payload ourselves, numbered from the key's packet number.
 */
#include "core.h"

/* A cipher we can use: the driver's, and one of the AES ones. TKIP (WPA1)
   and WEP would need per-frame help from us (TKIP's phase keys, MIC
   failures) that we don't have; wpa_supplicant isn't offered them. */
bool wifi_cipher_ok(struct wiphy *wiphy, u32 cipher)
{
    if (cipher != WLAN_CIPHER_SUITE_CCMP && cipher != WLAN_CIPHER_SUITE_CCMP_256 &&
        cipher != WLAN_CIPHER_SUITE_GCMP && cipher != WLAN_CIPHER_SUITE_GCMP_256)
        return false;
    for (int i = 0; i < wiphy->n_cipher_suites; i++)
        if (wiphy->cipher_suites[i] == cipher)
            return true;
    return false;
}

/* The cipher's header and trailer, in bytes. */
static void cipher_sizes(u32 cipher, u8 *iv, u8 *icv)
{
    switch (cipher) {
    case WLAN_CIPHER_SUITE_CCMP:     *iv = 8; *icv = 8; break;
    case WLAN_CIPHER_SUITE_CCMP_256:
    case WLAN_CIPHER_SUITE_GCMP:
    case WLAN_CIPHER_SUITE_GCMP_256: *iv = 8; *icv = 16; break;
    case WLAN_CIPHER_SUITE_TKIP:     *iv = 8; *icv = 4; break;
    case WLAN_CIPHER_SUITE_WEP40:
    case WLAN_CIPHER_SUITE_WEP104:   *iv = 4; *icv = 4; break;
    default:                         *iv = 0; *icv = 0; break;     /* (management-frame ciphers) */
    }
}

static void key_remove(struct ieee80211_sub_if_data *sdata, struct wifi_key **slot)
{
    struct wifi_key *k = *slot;
    struct ieee80211_local *local = sdata->local;
    if (!k)
        return;
    if (k->in_driver && !local->hw_lost)
        local->ops->set_key(&local->hw, DISABLE_KEY, &sdata->vif,
                            k->pairwise && sdata->conn.ap ? &sdata->conn.ap->sta : NULL, &k->conf);
    *slot = NULL;
    memset(k->conf.key, 0, k->conf.keylen);     /* (keys don't linger in freed memory) */
    kfree(k);
}

void wifi_keys_free(struct ieee80211_sub_if_data *sdata)
{
    key_remove(sdata, &sdata->conn.ptk);
    for (int i = 0; i < 4; i++)
        key_remove(sdata, &sdata->conn.gtk[i]);
}

/* Install a key: pairwise (mac given: the AP's) or group (index 1-3). */
int wifi_key_add(struct ieee80211_sub_if_data *sdata, const u8 *mac, int idx, u32 cipher,
                 const u8 *key, size_t keylen, const u8 *seq, size_t seqlen)
{
    struct ieee80211_local *local = sdata->local;
    struct wifi_conn *c = &sdata->conn;
    bool pairwise = mac != NULL;
    if (c->state != MLME_CONNECTED || !local->ops->set_key)
        return -EOPNOTSUPP;
    if (idx < 0 || idx > 3 || keylen > 32 || (pairwise && !ether_addr_equal(mac, c->bssid)))
        return -EINVAL;
    if (!wifi_cipher_ok(local->hw.wiphy, cipher))
        return -EOPNOTSUPP;

    struct wifi_key *k = kzalloc(sizeof(*k) + keylen, GFP_KERNEL);
    if (!k)
        return -ENOMEM;
    k->pairwise = pairwise;
    k->conf.cipher = cipher;
    k->conf.keyidx = (s8)idx;
    k->conf.keylen = (u8)keylen;
    k->conf.link_id = -1;
    k->conf.flags = pairwise ? IEEE80211_KEY_FLAG_PAIRWISE : 0;
    cipher_sizes(cipher, &k->conf.iv_len, &k->conf.icv_len);
    memcpy(k->conf.key, key, keylen);
    /* The receive counter the AP said it is at (group keys): bytes PN0...PN5. */
    for (size_t i = 0; seq && i < seqlen && i < 6; i++)
        k->rx_pn |= (u64)seq[i] << (8 * i);

    struct wifi_key **slot = pairwise ? &c->ptk : &c->gtk[idx];
    key_remove(sdata, slot);            /* (a new key replaces the old one) */
    int err = local->ops->set_key(&local->hw, SET_KEY, &sdata->vif,
                                  pairwise ? &c->ap->sta : NULL, &k->conf);
    if (err) {
        kfree(k);
        return err;
    }
    k->in_driver = true;
    *slot = k;
    return 0;
}

int wifi_key_del(struct ieee80211_sub_if_data *sdata, const u8 *mac, int idx)
{
    if (idx < 0 || idx > 3)
        return -EINVAL;
    struct wifi_key **slot = mac ? &sdata->conn.ptk : &sdata->conn.gtk[idx];
    if (!*slot)
        return -ENOENT;
    key_remove(sdata, slot);
    return 0;
}

/* data.c: protect an outgoing data frame whose 802.11 header (hdrlen bytes)
   is at skb->data. Inserts the CCMP header if the driver wants it from us.
   0, or -errno (no headroom). */
int wifi_key_protect(struct ieee80211_sub_if_data *sdata, struct sk_buff *skb, unsigned int hdrlen)
{
    struct wifi_key *k = sdata->conn.ptk;
    struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
    struct ieee80211_hdr *h = (void *)skb->data;
    h->frame_control |= cpu_to_le16(IEEE80211_FCTL_PROTECTED);
    info->control.hw_key = &k->conf;
    if (!(k->conf.flags & (IEEE80211_KEY_FLAG_GENERATE_IV | IEEE80211_KEY_FLAG_PUT_IV_SPACE)))
        return 0;                       /* the hardware writes the header itself */
    u8 iv = k->conf.iv_len;
    if (skb_headroom(skb) < iv)
        return -ENOBUFS;
    u8 *p = skb_push(skb, iv);
    memmove(p, p + iv, hdrlen);         /* header first, then room for the CCMP header */
    u8 *ccmp = p + hdrlen;
    if (k->conf.flags & IEEE80211_KEY_FLAG_GENERATE_IV) {
        u64 pn = (u64)atomic64_inc_return(&k->conf.tx_pn);
        ccmp[0] = (u8)pn;
        ccmp[1] = (u8)(pn >> 8);
        ccmp[2] = 0;
        ccmp[3] = (u8)(0x20 | (k->conf.keyidx << 6));     /* ExtIV, key id */
        for (int i = 0; i < 4; i++)
            ccmp[4 + i] = (u8)(pn >> (16 + 8 * i));
    } else {
        memset(ccmp, 0, iv);            /* (PUT_IV_SPACE: the hardware fills it) */
    }
    return 0;
}

/* The key a received protected frame used (for its header/trailer sizes). */
const struct ieee80211_key_conf *wifi_key_for_rx(struct ieee80211_sub_if_data *sdata, bool group,
                                                 int idx)
{
    struct wifi_key *k = group ? (idx >= 0 && idx < 4 ? sdata->conn.gtk[idx] : NULL) : sdata->conn.ptk;
    return k ? &k->conf : NULL;
}

/* After a firmware restart (drvapi.c): the keys the driver had, again. One
   it refuses now is dropped from the driver's list (its frames won't be
   decrypted until wpa_supplicant sets a new one). */
void wifi_keys_replay(struct ieee80211_sub_if_data *sdata)
{
    struct ieee80211_local *local = sdata->local;
    struct wifi_conn *c = &sdata->conn;
    struct wifi_key *keys[5] = { c->ptk, c->gtk[0], c->gtk[1], c->gtk[2], c->gtk[3] };
    for (int i = 0; i < 5; i++) {
        struct wifi_key *k = keys[i];
        if (!k || !k->in_driver)
            continue;
        if (local->ops->set_key(&local->hw, SET_KEY, &sdata->vif,
                                k->pairwise && c->ap ? &c->ap->sta : NULL, &k->conf)) {
            printk(KERN_ERR "wifi: %s: key %d not taken back after the restart\n",
                   sdata->dev->name, k->conf.keyidx);
            k->in_driver = false;
        }
    }
}
