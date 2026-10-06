/*
 * core.c — radios and interfaces: what a Wi-Fi driver registers, and the
 * wlan0 network device that appears for it (Linux: mac80211's main.c and
 * iface.c, cfg80211's core.c). See core.h for the layout and the rules.
 *
 * Bringing wlan0 up starts the driver (ops->start) and adds the interface
 * to it (ops->add_interface); taking it down leaves any network and
 * undoes both. Joining networks is mlme.c, scanning scan.c, the frames
 * data.c.
 */
#include <interrupts.h>
#include <linux/etherdevice.h>
#include <linux/rfkill.h>
#include "core.h"

static struct ieee80211_local *radios[WIFI_MAX_RADIOS];

struct ieee80211_local *wifi_radio(int index)
{
    return index >= 0 && index < WIFI_MAX_RADIOS ? radios[index] : NULL;
}

struct ieee80211_hw *wiphy_to_ieee80211_hw(struct wiphy *wiphy)
{
    return &wiphy_to_local(wiphy)->hw;
}

struct wireless_dev *ieee80211_vif_to_wdev(struct ieee80211_vif *vif)
{
    return &vif_to_sdata(vif)->wdev;
}

/* --- work with the wiphy lock held ----------------------------------------------------- */

static void wiphy_work_run(struct work_struct *w)
{
    struct ieee80211_local *local = container_of(w, struct ieee80211_local, wiphy_work);
    struct wiphy *wiphy = local->hw.wiphy;
    wiphy_lock(wiphy);
    while (!list_empty(&local->wiphy_work_list)) {
        struct wiphy_work *work = list_first_entry(&local->wiphy_work_list, struct wiphy_work, entry);
        list_del_init(&work->entry);
        work->func(wiphy, work);        /* (it may queue itself again) */
    }
    wiphy_unlock(wiphy);
}

void wiphy_work_queue(struct wiphy *wiphy, struct wiphy_work *work)
{
    struct ieee80211_local *local = wiphy_to_local(wiphy);
    unsigned long f = irq_save();      /* a driver may queue from its interrupt */
    if (list_empty(&work->entry))
        list_add_tail(&work->entry, &local->wiphy_work_list);
    irq_restore(f);
    queue_work(system_wq, &local->wiphy_work);
}

/* Called with the wiphy lock held, so the item is not running now. */
void wiphy_work_cancel(struct wiphy *wiphy, struct wiphy_work *work)
{
    (void)wiphy;
    unsigned long f = irq_save();
    if (!list_empty(&work->entry))
        list_del_init(&work->entry);
    irq_restore(f);
}

/* Run it now if it is waiting (the caller holds the wiphy lock). */
void wiphy_work_flush(struct wiphy *wiphy, struct wiphy_work *work)
{
    unsigned long f = irq_save();
    bool queued = !list_empty(&work->entry);
    if (queued)
        list_del_init(&work->entry);
    irq_restore(f);
    if (queued)
        work->func(wiphy, work);
}

/* --- the radio switch --------------------------------------------------------------------- */

/* No software switch yet (no rfkill device to flip): never blocked. */
bool rfkill_soft_blocked(struct rfkill *rfkill) { (void)rfkill; return false; }
bool rfkill_blocked(struct rfkill *rfkill) { (void)rfkill; return false; }

void wiphy_rfkill_set_hw_state_reason(struct wiphy *wiphy, bool blocked,
                                      enum rfkill_hard_block_reasons reason)
{
    (void)reason;
    printk(KERN_INFO "wifi: %s: radio switch %s\n", wiphy_name(wiphy), blocked ? "off" : "on");
}

/* --- interfaces ----------------------------------------------------------------------------- */

/* The standard's default EDCA parameters — how long each access category
   waits and backs off before sending (IEEE 802.11-2020 Table 9-155, OFDM:
   aCWmin 15, aCWmax 1023; TXOP in units of 32 us) — which mac80211 gives
   a driver when an interface comes up, so its firmware never sees zeros
   (AIFS 0 is no valid setting). With qos false, as after joining an AP
   without WMM: plain DCF, the same for all four. */
void wifi_default_edca(struct ieee80211_sub_if_data *sdata, bool qos)
{
    static const struct { u16 cw_min, cw_max, txop; u8 aifs; } edca[IEEE80211_NUM_ACS] = {
        [IEEE80211_AC_VO] = { 3, 7, 47, 2 },        /* 1.504 ms */
        [IEEE80211_AC_VI] = { 7, 15, 94, 2 },       /* 3.008 ms */
        [IEEE80211_AC_BE] = { 15, 1023, 0, 3 },
        [IEEE80211_AC_BK] = { 15, 1023, 0, 7 },
    };
    struct ieee80211_local *local = sdata->local;
    if (!local->ops->conf_tx || local->hw.queues < IEEE80211_NUM_ACS)
        return;                         /* (one queue: nothing to set apart) */
    for (int ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
        struct ieee80211_tx_queue_params p = {
            .cw_min = qos ? edca[ac].cw_min : 15, .cw_max = qos ? edca[ac].cw_max : 1023,
            .txop = qos ? edca[ac].txop : 0, .aifs = qos ? edca[ac].aifs : 2,
        };
        local->ops->conf_tx(&local->hw, &sdata->vif, 0, (u16)ac, &p);
    }
    sdata->vif.bss_conf.qos = qos;
}

/* Hand the interface to the driver (starting the driver first if it is
   the first): on ifup, after a firmware restart, or when a scan finds it
   left out (a restart that failed). The wiphy lock is held. */
int wifi_driver_up(struct ieee80211_sub_if_data *sdata)
{
    struct ieee80211_local *local = sdata->local;
    struct ieee80211_hw *hw = &local->hw;
    if (sdata->in_driver)
        return 0;
    if (!local->started) {
        int err = local->ops->start(hw);
        if (err)
            return err;
        local->started = true;
    }
    int err = local->ops->add_interface(hw, &sdata->vif);
    if (err)
        return err;
    sdata->in_driver = true;
    wifi_default_edca(sdata, true);
    /* Our own frames, broadcasts, and beacons and probe responses (for
       the list of networks); the driver says which it can filter. */
    unsigned int filter = FIF_BCN_PRBRESP_PROMISC;
    local->ops->configure_filter(hw, ~0u, &filter, 0);
    return 0;
}

static int wifi_open(struct net_device *dev)
{
    struct ieee80211_sub_if_data *sdata = netdev_priv(dev);
    struct ieee80211_local *local = sdata->local;
    struct ieee80211_hw *hw = &local->hw;

    wiphy_lock(hw->wiphy);
    int err = wifi_driver_up(sdata);
    if (err) {
        if (!local->open_count && local->started) {
            local->ops->stop(hw, false);
            local->started = false;
        }
    } else {
        local->open_count++;
        netif_carrier_off(dev);         /* not connected to a network yet */
        netif_tx_start_all_queues(dev);
    }
    wiphy_unlock(hw->wiphy);
    return err;
}

static int wifi_stop(struct net_device *dev)
{
    struct ieee80211_sub_if_data *sdata = netdev_priv(dev);
    struct ieee80211_local *local = sdata->local;
    struct ieee80211_hw *hw = &local->hw;

    wiphy_lock(hw->wiphy);
    netif_tx_stop_all_queues(dev);
    wifi_disconnect(sdata, WLAN_REASON_DEAUTH_LEAVING, true);   /* (-ENOTCONN if not) */
    wifi_scan_cancel(sdata);
    if (sdata->in_driver) {
        local->ops->remove_interface(hw, &sdata->vif);
        sdata->in_driver = false;
        if (--local->open_count == 0 && local->started) {
            local->ops->stop(hw, false);
            local->started = false;
        }
    }
    wiphy_unlock(hw->wiphy);
    return 0;
}

static const struct net_device_ops wifi_netdev_ops = {
    .ndo_open = wifi_open,
    .ndo_stop = wifi_stop,
    .ndo_start_xmit = wifi_xmit,
};

struct ieee80211_sub_if_data *wifi_sdata_by_dev(struct net_device *dev)
{
    return dev && dev->netdev_ops == &wifi_netdev_ops ? netdev_priv(dev) : NULL;
}

static int if_add(struct ieee80211_local *local, const char *name, enum nl80211_iftype type)
{
    struct wiphy *wiphy = local->hw.wiphy;
    struct net_device *dev = alloc_netdev_mqs(sizeof(struct ieee80211_sub_if_data) +
                                              local->hw.vif_data_size,
                                              name, 0, ether_setup, 1, 1);
    if (!dev)
        return -ENOMEM;
    struct ieee80211_sub_if_data *sdata = netdev_priv(dev);
    sdata->local = local;
    sdata->dev = dev;
    sdata->wdev.wiphy = wiphy;
    sdata->wdev.iftype = type;
    sdata->wdev.netdev = dev;
    sdata->wdev.identifier = ++local->wdev_ids;
    sdata->vif.type = type;
    sdata->vif.bss_conf.vif = &sdata->vif;
    sdata->vif.link_conf[0] = &sdata->vif.bss_conf;
    sdata->vif.bss_conf.bssid = sdata->link_bssid;     /* never NULL, as in mac80211 */

    const u8 *mac = wiphy->perm_addr;
    if (is_zero_ether_addr(mac) && wiphy->n_addresses)
        mac = wiphy->addresses[0].addr;
    ether_addr_copy(dev->dev_addr, mac);
    ether_addr_copy(dev->perm_addr, mac);
    ether_addr_copy(sdata->vif.addr, mac);
    ether_addr_copy(sdata->vif.bss_conf.addr, mac);    /* the link's own address (one link) */
    ether_addr_copy(sdata->wdev.address, mac);
    sdata->vif.addr_valid = true;

    int err = wifi_mlme_init(sdata);
    if (err) {
        free_netdev(dev);
        return err;
    }
    dev->netdev_ops = &wifi_netdev_ops;
    dev->ieee80211_ptr = &sdata->wdev;
    dev->needed_headroom = local->hw.extra_tx_headroom + 32;   /* room for the 802.11 header */
    dev->dev.parent = wiphy->dev.parent;
    err = register_netdevice(dev);
    if (err) {
        wifi_mlme_free(sdata);
        free_netdev(dev);
        return err;
    }
    list_add_tail(&sdata->list, &local->interfaces);
    return 0;
}

static void if_remove(struct ieee80211_sub_if_data *sdata)
{
    if (sdata->dev->flags & IFF_UP)
        dev_change_flags(sdata->dev, sdata->dev->flags & ~IFF_UP);
    list_del(&sdata->list);
    wifi_mlme_free(sdata);
    unregister_netdevice(sdata->dev);
    free_netdev(sdata->dev);
}

/* --- radios ---------------------------------------------------------------------------------- */

struct ieee80211_hw *ieee80211_alloc_hw_nm(size_t priv_data_len, const struct ieee80211_ops *ops,
                                           const char *requested_name)
{
    (void)requested_name;
    if (!ops->tx || !ops->start || !ops->stop || !ops->add_interface ||
        !ops->remove_interface || !ops->configure_filter)
        return NULL;                    /* what every driver must provide */

    size_t size = sizeof(struct wiphy) + sizeof(struct ieee80211_local) + priv_data_len;
    struct wiphy *wiphy = kzalloc(size, GFP_KERNEL);
    if (!wiphy)
        return NULL;
    struct ieee80211_local *local = wiphy_priv(wiphy);
    struct ieee80211_hw *hw = &local->hw;
    hw->wiphy = wiphy;
    hw->priv = local->drv_priv;
    local->ops = ops;
    local->index = -1;

    mutex_init(&wiphy->mtx);
    wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
    wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
    wiphy->max_scan_ssids = 4;
    wiphy->max_scan_ie_len = IEEE80211_MAX_DATA_LEN;
    wiphy->rts_threshold = (u32)-1;
    hw->queues = 1;
    hw->max_rates = 1;
    hw->max_rate_tries = 1;
    hw->conf.listen_interval = 1;
    hw->max_rx_aggregation_subframes = IEEE80211_MAX_AMPDU_BUF_HT;
    hw->max_tx_aggregation_subframes = IEEE80211_MAX_AMPDU_BUF_HT;

    INIT_LIST_HEAD(&local->interfaces);
    INIT_LIST_HEAD(&local->dead_stas);
    INIT_LIST_HEAD(&local->wiphy_work_list);
    skb_queue_head_init(&local->rx_queue);
    init_completion(&local->scan_done);
    return hw;
}

static void rx_work_run(struct work_struct *w);

int ieee80211_register_hw(struct ieee80211_hw *hw)
{
    struct ieee80211_local *local = hw_to_local(hw);
    struct wiphy *wiphy = hw->wiphy;
    int n_chan = 0;

    for (int b = 0; b < NUM_NL80211_BANDS; b++) {
        struct ieee80211_supported_band *sband = wiphy->bands[b];
        if (!sband)
            continue;
        sband->band = (enum nl80211_band)b;
        for (int i = 0; i < sband->n_channels; i++) {
            sband->channels[i].band = (enum nl80211_band)b;
            sband->channels[i].orig_flags = sband->channels[i].flags;
            sband->channels[i].orig_mag = sband->channels[i].max_antenna_gain;
            sband->channels[i].orig_mpwr = sband->channels[i].max_power;
        }
        n_chan += sband->n_channels;
    }
    if (!n_chan)
        return -EINVAL;

    int index = -1;
    for (int i = 0; i < WIFI_MAX_RADIOS && index < 0; i++)
        if (!radios[i])
            index = i;
    if (index < 0)
        return -ENOSPC;
    local->index = index;
    dev_set_name(&wiphy->dev, "phy%d", index);

    INIT_WORK(&local->rx_work, rx_work_run);
    INIT_WORK(&local->scan_done_work, wifi_scan_done_work);
    INIT_WORK(&local->wiphy_work, wiphy_work_run);
    INIT_WORK(&local->restart_work, wifi_restart_work);
    radios[index] = local;
    local->registered = true;
    wiphy->registered = true;

    if (!ieee80211_hw_check(hw, NO_AUTO_VIF)) {
        int err = if_add(local, "wlan%d", NL80211_IFTYPE_STATION);
        if (err) {
            radios[index] = NULL;
            local->registered = wiphy->registered = false;
            return err;
        }
    }
    struct ieee80211_sub_if_data *first = list_empty(&local->interfaces) ? NULL :
        list_first_entry(&local->interfaces, struct ieee80211_sub_if_data, list);
    printk(KERN_INFO "wifi: %s: %d channels%s%s, address %pM\n", wiphy_name(wiphy), n_chan,
            first ? ", interface " : "", first ? first->dev->name : "",
            first ? first->dev->dev_addr : wiphy->perm_addr);
    return 0;
}

void ieee80211_unregister_hw(struct ieee80211_hw *hw)
{
    struct ieee80211_local *local = hw_to_local(hw);
    cancel_work_sync(&local->restart_work);
    while (!list_empty(&local->interfaces))
        if_remove(list_first_entry(&local->interfaces, struct ieee80211_sub_if_data, list));
    cancel_work_sync(&local->rx_work);
    cancel_work_sync(&local->scan_done_work);
    cancel_work_sync(&local->wiphy_work);
    skb_queue_purge(&local->rx_queue);
    if (local->index >= 0)
        radios[local->index] = NULL;
    local->registered = hw->wiphy->registered = false;
}

void ieee80211_free_hw(struct ieee80211_hw *hw)
{
    struct ieee80211_local *local = hw_to_local(hw);
    wifi_bss_free_all(local);
    wifi_dead_stas_free(local);         /* (the driver is gone: nothing points at them) */
    kfree(hw->wiphy->regd);
    kfree(hw->wiphy);
}

/* --- receiving ------------------------------------------------------------------------------- */

/* Received frames: beacons and probe responses feed the list of networks,
   data goes to the network stack (data.c), other management frames to the
   MLME (mlme.c). */
void ieee80211_rx_napi(struct ieee80211_hw *hw, struct ieee80211_sta *pubsta, struct sk_buff *skb,
                       struct napi_struct *napi)
{
    struct ieee80211_local *local = hw_to_local(hw);
    struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
    (void)pubsta;
    (void)napi;

    if (status->flag & (RX_FLAG_FAILED_FCS_CRC | RX_FLAG_FAILED_PLCP_CRC | RX_FLAG_NO_PSDU))
        goto drop;
    if (ieee80211_hw_check(hw, RX_INCLUDES_FCS)) {
        if (skb->len < FCS_LEN)
            goto drop;
        skb_trim(skb, skb->len - FCS_LEN);
    }
    if (skb->len < 24 || skb_linearize(skb))
        goto drop;

    struct ieee80211_hdr *hdr = (void *)skb->data;
    if (ieee80211_is_beacon(hdr->frame_control) || ieee80211_is_probe_resp(hdr->frame_control)) {
        wifi_bss_update(local, skb);
    } else if (ieee80211_is_data(hdr->frame_control)) {
        wifi_rx_data(local, skb);
        return;
    } else if (ieee80211_is_mgmt(hdr->frame_control)) {
        /* For one of our interfaces: its MLME, with the wiphy lock held. */
        struct ieee80211_sub_if_data *sdata = wifi_sdata_by_addr(local, hdr->addr1);
        if (sdata) {
            skb_queue_tail(&sdata->mlme_queue, skb);
            wiphy_work_queue(hw->wiphy, &sdata->mlme_work);
            return;
        }
    }
drop:
    kfree_skb(skb);
}

/* From any context, the driver's interrupt included: queue it for the
   rx work thread (O(1) here). */
void ieee80211_rx_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    struct ieee80211_local *local = hw_to_local(hw);
    skb_queue_tail(&local->rx_queue, skb);
    queue_work(system_wq, &local->rx_work);
}

static void rx_work_run(struct work_struct *w)
{
    struct ieee80211_local *local = container_of(w, struct ieee80211_local, rx_work);
    struct sk_buff *skb;
    while ((skb = skb_dequeue(&local->rx_queue)))
        ieee80211_rx_napi(&local->hw, NULL, skb, NULL);
}

/* --- sending (W3) --------------------------------------------------------------------------- */

void ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    (void)hw;
    kfree_skb(skb);
}

void ieee80211_tx_status_skb(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    (void)hw;
    kfree_skb(skb);
}

void ieee80211_stop_queues(struct ieee80211_hw *hw)
{
    struct ieee80211_sub_if_data *sdata;
    list_for_each_entry(sdata, &hw_to_local(hw)->interfaces, list)
        netif_tx_stop_all_queues(sdata->dev);
}

void ieee80211_wake_queues(struct ieee80211_hw *hw)
{
    struct ieee80211_sub_if_data *sdata;
    list_for_each_entry(sdata, &hw_to_local(hw)->interfaces, list)
        netif_tx_wake_all_queues(sdata->dev);
}

/* --- walking interfaces ----------------------------------------------------------------------- */

void ieee80211_iterate_interfaces(struct ieee80211_hw *hw, u32 iter_flags,
                                  void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif),
                                  void *data)
{
    struct ieee80211_sub_if_data *sdata;
    list_for_each_entry(sdata, &hw_to_local(hw)->interfaces, list) {
        if ((iter_flags & IEEE80211_IFACE_ITER_ACTIVE) && !sdata->in_driver)
            continue;
        iterator(data, sdata->vif.addr, &sdata->vif);
    }
}

void ieee80211_iterate_active_interfaces_atomic(struct ieee80211_hw *hw, u32 iter_flags,
        void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif), void *data)
{
    ieee80211_iterate_interfaces(hw, iter_flags | IEEE80211_IFACE_ITER_ACTIVE, iterator, data);
}
