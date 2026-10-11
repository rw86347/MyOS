/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * wifi_test.c — the 802.11 core (W2b), checked at boot in `make test`
 * builds on the simulated radio (drivers/net/wireless/sim.c): it registers
 * and wlan0 appears; bringing wlan0 up starts the driver; a scan of every
 * channel finds the two simulated access points with their SSIDs,
 * channels, signal and security; a second scan while one runs is refused;
 * taking wlan0 down stops the driver. The radio stays registered for the
 * user-space tests that follow.
 */
#include <linux/delay.h>
#include <interrupts.h>
#include <ipv4.h>
#include <kprintf.h>
#include <linux/netdevice.h>
#include <selftest.h>
#include "../net/wireless/core.h"

int sim_wifi_init(void);
int sim_wifi_scans(void);
bool sim_wifi_started(void);
int sim_wifi_aifs(int ac);
void sim_wifi_crash(void);
int sim_wifi_violations(void);
const char *sim_wifi_assoc(void);
int sim_wifi_sta_state(void);
void sim_wifi_heard(int *data, int *arp, int *ping);
void sim_pbkdf2_sha1(const char *pass, const u8 *ssid, size_t ssid_len, u8 pmk[32]);
void sim_prf_sha1(const u8 *key, size_t klen, const char *label, const u8 *data, size_t dlen,
                  u8 *out, size_t n);
void sim_aes_wrap(const u8 kek[16], const u8 *plain, size_t n, u8 *out);
void sim_aes_block(const u8 key[16], const u8 in[16], u8 out[16]);

static void unhex(const char *h, u8 *out)
{
    for (; h[0] && h[1]; h += 2) {
        int a = h[0] <= '9' ? h[0] - '0' : (h[0] | 0x20) - 'a' + 10;
        int b = h[1] <= '9' ? h[1] - '0' : (h[1] | 0x20) - 'a' + 10;
        *out++ = (u8)(a << 4 | b);
    }
}

/* The simulated AP's WPA2 crypto against published test vectors: if these
   are right, a failed handshake is the kernel's fault, not the test AP's. */
static void crypto_selftest(void)
{
    u8 out[64], want[64], in[32];
    sim_pbkdf2_sha1("password", (const u8 *)"IEEE", 4, out);
    unhex("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", want);
    check(!memcmp(out, want, 32), "wifi: test AP crypto: PBKDF2-SHA1 (IEEE 802.11 J.4)");

    memset(in, 0x0b, 20);
    sim_prf_sha1(in, 20, "prefix", (const u8 *)"Hi There", 8, out, 64);
    unhex("bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
          "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a", want);
    check(!memcmp(out, want, 64), "wifi: test AP crypto: PRF-512 (IEEE 802.11 J.3)");

    unhex("000102030405060708090a0b0c0d0e0f", in);
    unhex("00112233445566778899aabbccddeeff", in + 16);
    sim_aes_block(in, in + 16, out);
    unhex("69c4e0d86a7b0430d8cdb78070b4c55a", want);
    check(!memcmp(out, want, 16), "wifi: test AP crypto: AES-128 (FIPS-197 C.1)");
    sim_aes_wrap(in, in + 16, 2, out);
    unhex("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", want);
    check(!memcmp(out, want, 24), "wifi: test AP crypto: AES key wrap (RFC 3394 4.1)");
}

struct found {
    int n;
    bool test_ap, open_5g;
};

/* What one found network looks like through the public structures. */
static void each_bss(struct wiphy *wiphy, struct cfg80211_bss *bss, void *data)
{
    struct found *f = data;
    (void)wiphy;
    f->n++;
    const struct element *ssid = cfg80211_find_elem(WLAN_EID_SSID, bss->ies->data, bss->ies->len);
    if (!ssid)
        return;
    if (ssid->datalen == 9 && !memcmp(ssid->data, "MyOS-Test", 9))
        f->test_ap = bss->channel->center_freq == 2437 && bss->signal == -4200 &&
                     (bss->capability & 0x0010) && bss->beacon_interval == 100 &&
                     cfg80211_find_elem(WLAN_EID_RSN, bss->ies->data, bss->ies->len);
    if (ssid->datalen == 12 && !memcmp(ssid->data, "MyOS-Open-5G", 12))
        f->open_5g = bss->channel->center_freq == 5180 && bss->signal == -6100 &&
                     !(bss->capability & 0x0010);
}

/* A scan request for every channel of the radio, one wildcard SSID. */
static struct cfg80211_scan_request *all_channels(struct wiphy *wiphy)
{
    int n = 0;
    for (int b = 0; b < NUM_NL80211_BANDS; b++)
        if (wiphy->bands[b])
            n += wiphy->bands[b]->n_channels;
    struct cfg80211_scan_request *req = kzalloc(sizeof(*req) + n * sizeof(req->channels[0]) +
                                                sizeof(struct cfg80211_ssid), GFP_KERNEL);
    if (!req)
        return NULL;
    for (int b = 0; b < NUM_NL80211_BANDS; b++)
        for (int i = 0; wiphy->bands[b] && i < wiphy->bands[b]->n_channels; i++)
            req->channels[req->n_channels++] = &wiphy->bands[b]->channels[i];
    req->ssids = (struct cfg80211_ssid *)&req->channels[n];    /* one, empty: any network */
    req->n_ssids = 1;
    return req;
}

void wifi_selftest(void)
{
    crypto_selftest();
    check(sim_wifi_init() == 0, "wifi: the simulated radio registers");
    struct net_device *dev = dev_get_by_name("wlan0");
    struct ieee80211_sub_if_data *sdata = wifi_sdata_by_dev(dev);
    check(sdata != NULL, "wifi: wlan0 appears for it");
    if (!sdata)
        return;
    struct ieee80211_local *local = sdata->local;
    struct wiphy *wiphy = local->hw.wiphy;
    check(!strcmp(wiphy_name(wiphy), "phy0") && dev->dev_addr[5] == 0x01,
          "wifi: phy0, wlan0 has the radio's address");

    check(dev_change_flags(dev, dev->flags | IFF_UP) == 0 && sim_wifi_started() && sdata->in_driver,
          "wifi: wlan0 up starts the driver and adds the interface");
    /* What mac80211 always gives a driver, and what was missing when the
       MSI's firmware asserted activating a link (docs/LOG.md): the link's
       own address, and real EDCA timing for every access category. */
    check(ether_addr_equal(sdata->vif.bss_conf.addr, sdata->vif.addr) &&
          sim_wifi_aifs(IEEE80211_AC_VO) == 2 && sim_wifi_aifs(IEEE80211_AC_BE) == 3 &&
          sim_wifi_aifs(IEEE80211_AC_BK) == 7,
          "wifi: the driver gets the link's address and default EDCA (802.11 Table 9-155)");

    struct cfg80211_scan_request *req = all_channels(wiphy), *again = all_channels(wiphy);
    wiphy_lock(wiphy);
    int err = req ? wifi_scan(sdata, req) : -ENOMEM;
    int busy = again ? wifi_scan(sdata, again) : -ENOMEM;
    wiphy_unlock(wiphy);
    if (err)
        kfree(req);
    kfree(again);                       /* (refused, so still ours) */
    check(err == 0, "wifi: a scan of all 17 channels starts");
    check(busy == -EBUSY, "wifi: a second scan while one runs is refused");

    bool done = err == 0 && wait_for_completion_timeout(&local->scan_done, 2 * HZ);
    check(done && sim_wifi_scans() == 1 && local->scan_generation == 1 && !local->scan_req,
          "wifi: the scan ends (the driver reports it done)");
    flush_work(&local->rx_work);        /* (frames heard during it may still be queued) */

    struct found f = { 0 };
    cfg80211_bss_iter(wiphy, NULL, each_bss, &f);
    check(f.n == 2, "wifi: two networks found");
    check(f.test_ap, "wifi: MyOS-Test: channel 6, -42 dBm, WPA2 (RSN), beacon every 100 TU");
    check(f.open_5g, "wifi: MyOS-Open-5G: channel 36, -61 dBm, open");

    /* Join the open network, as `iw dev wlan0 connect MyOS-Open-5G` asks. */
    struct wifi_connect_params cp = { .ssid = (const u8 *)"MyOS-Open-5G", .ssid_len = 12 };
    wiphy_lock(wiphy);
    err = wifi_connect(sdata, &cp);
    wiphy_unlock(wiphy);
    for (int i = 0; i < 200 && !err && sdata->conn.state != MLME_CONNECTED &&
                    sdata->conn.state != MLME_IDLE; i++)
        msleep(10);
    struct wifi_conn *c = &sdata->conn;
    check(err == 0 && c->state == MLME_CONNECTED && c->chan->center_freq == 5180 &&
          sdata->vif.cfg.assoc && sdata->vif.cfg.aid == 1 && netif_carrier_ok(dev),
          "wifi: connect to MyOS-Open-5G: authenticated, associated (AID 1), carrier on");
    check(sim_wifi_assoc() && !strcmp(sim_wifi_assoc(), "MyOS-Open-5G") &&
          sim_wifi_sta_state() == IEEE80211_STA_AUTHORIZED,
          "wifi: the driver saw the AP peer through to authorized (open network)");

    /* IPv4 moves to wlan0 and asks the AP's DHCP server for a lease (W3b). */
    struct net_device *eth = dev_get_by_name("eth0");      /* (where IPv4 should return) */
    uint32_t eth_addr = eth ? eth->in4.addr : 0;
    for (int i = 0; i < 300 && ipv4_addr != 0xc0a84d0au; i++)
        msleep(10);
    check(ipv4_dev() == dev && ipv4_addr == 0xc0a84d0au && ipv4_gw == 0xc0a84d01u,
          "wifi: IPv4 moves to wlan0; DHCP: 192.168.77.10, gateway 192.168.77.1");
    /* With interrupts off, as a syscall runs: net_sleep then really sleeps,
       and the threads that carry Wi-Fi frames (work items) get to run. */
    unsigned long irqs = irq_save();
    int pong = net_ping(0xc0a84d01u);
    irq_restore(irqs);
    if (pong) {
        int data, arp, ping;
        sim_wifi_heard(&data, &arp, &ping);
        kprintf("wifi: ping %d; the AP heard %d data frames, %d ARP, %d pings\n", pong, data, arp, ping);
    }
    check(pong == 0, "wifi: ping 192.168.77.1 across the air (ARP, ICMP)");

    /* The firmware crashes and the driver asks for a restart: the core
       tells it everything again (the peer, channel, timing) and the
       connection carries on, as with mac80211 — no new handshake. */
    sim_wifi_crash();
    flush_work(&local->restart_work);
    irqs = irq_save();
    pong = net_ping(0xc0a84d01u);
    irq_restore(irqs);
    check(c->state == MLME_CONNECTED && sim_wifi_sta_state() == IEEE80211_STA_AUTHORIZED &&
          sim_wifi_aifs(IEEE80211_AC_BE) == 2 && pong == 0,
          "wifi: a firmware restart: the driver gets the connection back; ping still answers");

    wiphy_lock(wiphy);
    err = wifi_disconnect(sdata, WLAN_REASON_DEAUTH_LEAVING, true);
    wiphy_unlock(wiphy);
    msleep(10);                         /* (the deauth reaches the simulated AP) */
    check(err == 0 && c->state == MLME_IDLE && !sdata->vif.cfg.assoc && !netif_carrier_ok(dev) &&
          !sim_wifi_assoc() && sim_wifi_sta_state() == IEEE80211_STA_NOTEXIST,
          "wifi: disconnect: deauthenticated, the peer gone from the driver");
    check(eth ? ipv4_dev() == eth && ipv4_addr == eth_addr : ipv4_dev() == NULL,
          "wifi: IPv4 back on eth0 with its own lease");

    check(sim_wifi_violations() == 0,
          "wifi: the core kept iwlwifi's rules all along (bssid, ERP after assoc, link address, EDCA)");
    check(dev_change_flags(dev, dev->flags & ~IFF_UP) == 0 && !sim_wifi_started() && !sdata->in_driver,
          "wifi: wlan0 down stops the driver");
}
