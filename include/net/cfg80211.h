/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef NET_CFG80211_H
#define NET_CFG80211_H

/*
 * net/cfg80211.h — the configuration side of MyOS's 802.11 core, with
 * Linux's names (docs/WIFI-PLAN.md). MyOS's own file, written for the
 * core and the drivers it carries; only what those use.
 *
 * The words:
 *   wiphy          one radio: its bands, channels, rates, what it can do.
 *                  A driver fills one in and registers it.
 *   band           2.4, 5 or 6 GHz (enum nl80211_band): a list of channels
 *                  and bitrates, and the HT/VHT/HE/EHT abilities there.
 *   wireless_dev   one interface on a radio (wlan0), in one mode.
 *   chandef        a channel plus its width (20..320 MHz) and centre.
 *   bss            a network a scan found (an access point).
 *
 * Everything that crosses to user space goes through nl80211 (the
 * numbers in linux/nl80211.h); these are the kernel-side structures.
 */
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/ieee80211.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/nl80211.h>
#include <linux/random.h>
#include <linux/rfkill.h>
#include <linux/workqueue.h>
#include <net/regulatory.h>

struct wiphy;

/* --- channels and bitrates -------------------------------------------------------- */

enum ieee80211_channel_flags {
	IEEE80211_CHAN_DISABLED		= BIT(0),
	IEEE80211_CHAN_NO_IR		= BIT(1),	/* may not start transmitting (no AP, no probe) */
	IEEE80211_CHAN_RADAR		= BIT(3),	/* radar detection required */
	IEEE80211_CHAN_NO_HT40PLUS	= BIT(4),
	IEEE80211_CHAN_NO_HT40MINUS	= BIT(5),
	IEEE80211_CHAN_NO_OFDM		= BIT(6),
	IEEE80211_CHAN_NO_80MHZ		= BIT(7),
	IEEE80211_CHAN_NO_160MHZ	= BIT(8),
	IEEE80211_CHAN_INDOOR_ONLY	= BIT(9),
	IEEE80211_CHAN_IR_CONCURRENT	= BIT(10),
	IEEE80211_CHAN_NO_20MHZ		= BIT(11),
	IEEE80211_CHAN_NO_10MHZ		= BIT(12),
	IEEE80211_CHAN_NO_HE		= BIT(13),
	IEEE80211_CHAN_NO_EHT		= BIT(20),
	IEEE80211_CHAN_NO_6GHZ_VLP_CLIENT = BIT(22),
	IEEE80211_CHAN_NO_6GHZ_AFC_CLIENT = BIT(23),
	IEEE80211_CHAN_CAN_MONITOR	= BIT(24),
	IEEE80211_CHAN_ALLOW_6GHZ_VLP_AP = BIT(25),
};
#define IEEE80211_CHAN_NO_HT40	(IEEE80211_CHAN_NO_HT40PLUS | IEEE80211_CHAN_NO_HT40MINUS)

struct ieee80211_channel {
	enum nl80211_band band;
	u32 center_freq;		/* MHz */
	u16 freq_offset;		/* kHz on top (sub-1 GHz bands only) */
	u16 hw_value;			/* the driver's own number for it */
	u32 flags;			/* enum ieee80211_channel_flags, after regulatory rules */
	int max_antenna_gain;		/* dBi */
	int max_power;			/* dBm */
	int max_reg_power;
	bool beacon_found;
	u32 orig_flags;			/* as the driver registered them */
	int orig_mag, orig_mpwr;
};

enum ieee80211_rate_flags {
	IEEE80211_RATE_SHORT_PREAMBLE	= BIT(0),
	IEEE80211_RATE_MANDATORY_A	= BIT(1),
	IEEE80211_RATE_MANDATORY_B	= BIT(2),
	IEEE80211_RATE_MANDATORY_G	= BIT(3),
	IEEE80211_RATE_ERP_G		= BIT(4),
};

/* A legacy (802.11a/b/g) bitrate. */
struct ieee80211_rate {
	u32 flags;
	u16 bitrate;			/* units of 100 kbit/s */
	u16 hw_value, hw_value_short;	/* the driver's numbers (short preamble: the second) */
};

/* --- what a station can do, per band ------------------------------------------------ */

struct ieee80211_sta_ht_cap {
	u16 cap;			/* IEEE80211_HT_CAP_* */
	bool ht_supported;
	u8 ampdu_factor;
	u8 ampdu_density;
	struct ieee80211_mcs_info mcs;
};

struct ieee80211_sta_vht_cap {
	bool vht_supported;
	u32 cap;			/* IEEE80211_VHT_CAP_* */
	struct ieee80211_vht_mcs_info vht_mcs;
};

struct ieee80211_sta_he_cap {
	bool has_he;
	struct ieee80211_he_cap_elem he_cap_elem;
	struct ieee80211_he_mcs_nss_supp he_mcs_nss_supp;
	u8 ppe_thres[IEEE80211_HE_PPE_THRES_MAX_LEN];
};

/* EHT streams per MCS: either one set for a 20 MHz-only device, or one
   per width. */
struct ieee80211_eht_mcs_nss_supp {
	union {
		struct ieee80211_eht_mcs_nss_supp_20mhz_only only_20mhz;
		struct {
			struct ieee80211_eht_mcs_nss_supp_bw _80;
			struct ieee80211_eht_mcs_nss_supp_bw _160;
			struct ieee80211_eht_mcs_nss_supp_bw _320;
		} __packed bw;
	} __packed;
} __packed;

struct ieee80211_sta_eht_cap {
	bool has_eht;
	struct ieee80211_eht_cap_elem_fixed eht_cap_elem;
	struct ieee80211_eht_mcs_nss_supp eht_mcs_nss_supp;
	u8 eht_ppe_thres[IEEE80211_EHT_PPE_THRES_MAX_LEN];
};

struct ieee80211_sta_uhr_cap {
	bool has_uhr;
	struct ieee80211_uhr_cap_mac mac;
	struct ieee80211_uhr_cap_phy phy;
};

/* HE and later abilities can differ by interface type (station, AP...):
   one entry per group of types. */
struct ieee80211_sband_iftype_data {
	u16 types_mask;			/* BIT(NL80211_IFTYPE_...) */
	struct ieee80211_sta_he_cap he_cap;
	struct ieee80211_he_6ghz_capa he_6ghz_capa;
	struct ieee80211_sta_eht_cap eht_cap;
	struct ieee80211_sta_uhr_cap uhr_cap;
	struct {
		const u8 *data;
		unsigned int len;
	} vendor_elems;
};

struct ieee80211_supported_band {
	struct ieee80211_channel *channels;
	struct ieee80211_rate *bitrates;
	enum nl80211_band band;
	int n_channels;
	int n_bitrates;
	struct ieee80211_sta_ht_cap ht_cap;
	struct ieee80211_sta_vht_cap vht_cap;
	u16 n_iftype_data;
	const struct ieee80211_sband_iftype_data *iftype_data;
};

static inline void _ieee80211_set_sband_iftype_data(struct ieee80211_supported_band *sband,
						    const struct ieee80211_sband_iftype_data *iftd,
						    u16 n_iftd)
{
	sband->iftype_data = iftd;
	sband->n_iftype_data = n_iftd;
}
#define ieee80211_set_sband_iftype_data(sband, iftd) \
	_ieee80211_set_sband_iftype_data((sband), (iftd), ARRAY_SIZE(iftd))

static inline const struct ieee80211_sband_iftype_data *
ieee80211_get_sband_iftype_data(const struct ieee80211_supported_band *sband, u8 iftype)
{
	if (WARN_ON(iftype >= NUM_NL80211_IFTYPES))
		return NULL;
	if (iftype == NL80211_IFTYPE_AP_VLAN)
		iftype = NL80211_IFTYPE_AP;
	for (int i = 0; i < sband->n_iftype_data; i++)
		if (sband->iftype_data[i].types_mask & BIT(iftype))
			return &sband->iftype_data[i];
	return NULL;
}

static inline const struct ieee80211_sta_he_cap *
ieee80211_get_he_iftype_cap(const struct ieee80211_supported_band *sband, u8 iftype)
{
	const struct ieee80211_sband_iftype_data *d = ieee80211_get_sband_iftype_data(sband, iftype);
	return d && d->he_cap.has_he ? &d->he_cap : NULL;
}

static inline const struct ieee80211_sta_eht_cap *
ieee80211_get_eht_iftype_cap(const struct ieee80211_supported_band *sband, enum nl80211_iftype iftype)
{
	const struct ieee80211_sband_iftype_data *d = ieee80211_get_sband_iftype_data(sband, iftype);
	return d && d->eht_cap.has_eht ? &d->eht_cap : NULL;
}

/* --- channel definitions ------------------------------------------------------------ */

struct cfg80211_chan_def {
	struct ieee80211_channel *chan;	/* the primary 20 MHz channel */
	enum nl80211_chan_width width;
	u32 center_freq1;		/* centre of the whole width */
	u32 center_freq2;		/* second segment (80+80 only) */
	u16 freq1_offset;
	u16 punctured;			/* 20 MHz subchannels left out (EHT) */
};

void cfg80211_chandef_create(struct cfg80211_chan_def *chandef, struct ieee80211_channel *chan,
			     enum nl80211_channel_type chantype);

/* Channel number <-> frequency (both directions per band, as 802.11 numbers them). */
u32 ieee80211_channel_to_freq_khz(int chan, enum nl80211_band band);
int ieee80211_freq_khz_to_channel(u32 freq);
static inline int ieee80211_channel_to_frequency(int chan, enum nl80211_band band)
{
	return KHZ_TO_MHZ(ieee80211_channel_to_freq_khz(chan, band));
}
static inline int ieee80211_frequency_to_channel(int freq)
{
	return ieee80211_freq_khz_to_channel(MHZ_TO_KHZ(freq));
}
struct ieee80211_channel *ieee80211_get_channel_khz(struct wiphy *wiphy, u32 freq);
static inline struct ieee80211_channel *ieee80211_get_channel(struct wiphy *wiphy, int freq)
{
	return ieee80211_get_channel_khz(wiphy, MHZ_TO_KHZ(freq));
}

/* A 6 GHz "preferred scanning channel": every fourth 20 MHz channel. */
static inline bool cfg80211_channel_is_psc(struct ieee80211_channel *chan)
{
	return chan->band == NL80211_BAND_6GHZ &&
	       ieee80211_frequency_to_channel(chan->center_freq) % 16 == 5;
}

/* The length of an 802.11 header with this frame control (24..32 bytes). */
unsigned int ieee80211_hdrlen(__le16 fc);

/* --- elements (in beacons, probe responses...) ------------------------------------------ */

/* The first element with this id (and, if match is given, these bytes at
   match_offset into its data). NULL if there is none or the list is cut. */
const struct element *cfg80211_find_elem_match(u8 eid, const u8 *ies, unsigned int len,
					       const u8 *match, unsigned int match_len,
					       unsigned int match_offset);
static inline const struct element *cfg80211_find_elem(u8 eid, const u8 *ies, int len)
{
	return cfg80211_find_elem_match(eid, ies, len, NULL, 0, 0);
}
static inline const struct element *cfg80211_find_ext_elem(u8 ext_eid, const u8 *ies, int len)
{
	return cfg80211_find_elem_match(WLAN_EID_EXTENSION, ies, len, &ext_eid, 1, 0);
}
/* The old form: offsets count the 2-byte element header, and the result
   points at it. */
static inline const u8 *cfg80211_find_ie_match(u8 eid, const u8 *ies, unsigned int len,
					       const u8 *match, unsigned int match_len,
					       unsigned int match_offset)
{
	if (WARN_ON((match_len && match_offset < 2) || (!match_len && match_offset)))
		return NULL;
	return (const u8 *)cfg80211_find_elem_match(eid, ies, len, match, match_len,
						    match_offset ? match_offset - 2 : 0);
}
static inline const u8 *cfg80211_find_ie(u8 eid, const u8 *ies, int len)
{
	return cfg80211_find_ie_match(eid, ies, len, NULL, 0, 0);
}

/* Where to cut an element list so the listed ids come first (offset:
   where to start looking). */
size_t ieee80211_ie_split_ric(const u8 *ies, size_t ielen, const u8 *ids, int n_ids,
			      const u8 *after_ric, int n_after_ric, size_t offset);
static inline size_t ieee80211_ie_split(const u8 *ies, size_t ielen, const u8 *ids, int n_ids,
					size_t offset)
{
	return ieee80211_ie_split_ric(ies, ielen, ids, n_ids, NULL, 0, offset);
}

/* --- rates a user allows, AP power class, BSS colour ----------------------------------- */

struct cfg80211_bitrate_mask {
	struct {
		u32 legacy;
		u8 ht_mcs[IEEE80211_HT_MCS_MASK_LEN];
		u16 vht_mcs[NL80211_VHT_NSS_MAX];
		u16 he_mcs[NL80211_HE_NSS_MAX];
		enum nl80211_txrate_gi gi;
		enum nl80211_he_gi he_gi;
		enum nl80211_he_ltf he_ltf;
	} control[NUM_NL80211_BANDS];
};

/* 6 GHz access points come in power classes (low-power indoor, standard
   power, very low power), each with its own limits. */
enum ieee80211_ap_reg_power {
	IEEE80211_REG_UNSET_AP,
	IEEE80211_REG_LPI_AP,
	IEEE80211_REG_SP_AP,
	IEEE80211_REG_VLP_AP,
};

struct cfg80211_he_bss_color {
	u8 color;
	bool enabled;
	bool partial;
};

/* The LLC/SNAP header in front of an Ethernet type in 802.11 data frames
   (RFC 1042), and the 802.1H variant for a few types. */
extern const unsigned char rfc1042_header[6];
extern const unsigned char bridge_tunnel_header[6];

/* --- statistics -------------------------------------------------------------------------- */

enum survey_info_flags {
	SURVEY_INFO_NOISE_DBM		= BIT(0),
	SURVEY_INFO_IN_USE		= BIT(1),
	SURVEY_INFO_TIME		= BIT(2),
	SURVEY_INFO_TIME_BUSY		= BIT(3),
	SURVEY_INFO_TIME_EXT_BUSY	= BIT(4),
	SURVEY_INFO_TIME_RX		= BIT(5),
	SURVEY_INFO_TIME_TX		= BIT(6),
	SURVEY_INFO_TIME_SCAN		= BIT(7),
};

/* How busy one channel is (times in ms). */
struct survey_info {
	struct ieee80211_channel *channel;
	u64 time, time_busy, time_ext_busy, time_rx, time_tx, time_scan, time_bss_rx;
	u32 filled;			/* which fields are valid: enum survey_info_flags */
	s8 noise;			/* dBm */
};

enum rate_info_flags {
	RATE_INFO_FLAGS_MCS		= BIT(0),
	RATE_INFO_FLAGS_VHT_MCS		= BIT(1),
	RATE_INFO_FLAGS_SHORT_GI	= BIT(2),
	RATE_INFO_FLAGS_DMG		= BIT(3),
	RATE_INFO_FLAGS_HE_MCS		= BIT(4),
	RATE_INFO_FLAGS_EDMG		= BIT(5),
	RATE_INFO_FLAGS_EXTENDED_SC_DMG	= BIT(6),
	RATE_INFO_FLAGS_EHT_MCS		= BIT(7),
};

enum rate_info_bw {
	RATE_INFO_BW_20 = 0,
	RATE_INFO_BW_5,
	RATE_INFO_BW_10,
	RATE_INFO_BW_40,
	RATE_INFO_BW_80,
	RATE_INFO_BW_160,
	RATE_INFO_BW_HE_RU,
	RATE_INFO_BW_320,
	RATE_INFO_BW_EHT_RU,
};

/* One transmit or receive rate, as reported to user space. */
struct rate_info {
	u16 flags;			/* enum rate_info_flags */
	u16 legacy;			/* 100 kbit/s, for non-MCS rates */
	u8 mcs;
	u8 nss;				/* spatial streams */
	u8 bw;				/* enum rate_info_bw */
	u8 he_gi, he_dcm, he_ru_alloc;
	u8 n_bonded_ch;
	u8 eht_gi, eht_ru_alloc;
};

/* What "iw dev wlan0 station dump" shows. filled: BIT_ULL(NL80211_STA_INFO_*). */
struct station_info {
	u64 filled;
	u32 connected_time, inactive_time;
	u64 rx_bytes, tx_bytes;
	s8 signal, signal_avg;		/* dBm */
	u8 chains;
	s8 chain_signal[IEEE80211_MAX_CHAINS];
	s8 chain_signal_avg[IEEE80211_MAX_CHAINS];
	struct rate_info txrate, rxrate;
	u32 rx_packets, tx_packets, tx_retries, tx_failed;
	u64 rx_beacon;
	u8 rx_beacon_signal_avg;
};

/* --- scanning ----------------------------------------------------------------------------- */

struct cfg80211_ssid {
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	u8 ssid_len;
};

struct cfg80211_scan_info {
	u64 scan_start_tsf;
	u8 tsf_bssid[ETH_ALEN] __aligned(2);
	bool aborted;
};

/* An AP on 6 GHz that another band's beacons told us about. */
struct cfg80211_scan_6ghz_params {
	u32 short_ssid;
	u32 channel_idx;
	u8 bssid[ETH_ALEN];
	bool unsolicited_probe;
	bool short_ssid_valid;
	bool psc_no_listen;
	s8 psd_20;
};

/* A scan to run: which SSIDs to probe for, which channels (the flexible
   array at the end), extra elements for the probe requests. */
struct cfg80211_scan_request {
	struct cfg80211_ssid *ssids;
	int n_ssids;
	u32 n_channels;
	const u8 *ie;
	size_t ie_len;
	u16 duration;
	bool duration_mandatory;
	u32 flags;			/* NL80211_SCAN_FLAG_* */
	u32 rates[NUM_NL80211_BANDS];
	struct wireless_dev *wdev;
	u8 mac_addr[ETH_ALEN] __aligned(2);
	u8 mac_addr_mask[ETH_ALEN] __aligned(2);
	u8 bssid[ETH_ALEN] __aligned(2);
	struct wiphy *wiphy;
	unsigned long scan_start;	/* jiffies */
	struct cfg80211_scan_info info;
	bool notified;
	bool no_cck;
	bool scan_6ghz;
	u32 n_6ghz_params;
	struct cfg80211_scan_6ghz_params *scan_6ghz_params;
	s8 tsf_report_link_id;
	struct ieee80211_channel *channels[];
};

struct cfg80211_match_set {
	struct cfg80211_ssid ssid;
	u8 bssid[ETH_ALEN];
	s32 rssi_thold;
};

struct cfg80211_sched_scan_plan {
	u32 interval;			/* seconds */
	u32 iterations;			/* 0: forever */
};

/* A scan the radio repeats on its own (to find a network while idle). */
struct cfg80211_sched_scan_request {
	u64 reqid;
	struct cfg80211_ssid *ssids;
	int n_ssids;
	u32 n_channels;
	const u8 *ie;
	size_t ie_len;
	u32 flags;
	struct cfg80211_match_set *match_sets;
	int n_match_sets;
	s32 min_rssi_thold;
	u32 delay;
	struct cfg80211_sched_scan_plan *scan_plans;
	int n_scan_plans;
	u8 mac_addr[ETH_ALEN] __aligned(2);
	u8 mac_addr_mask[ETH_ALEN] __aligned(2);
	struct wiphy *wiphy;
	struct net_device *dev;
	unsigned long scan_start;
	struct list_head list;
	struct ieee80211_channel *channels[];
};

/* A random address with the bits of `mask` taken from `addr`. */
static inline void get_random_mask_addr(u8 *buf, const u8 *addr, const u8 *mask)
{
	get_random_bytes(buf, ETH_ALEN);
	for (int i = 0; i < ETH_ALEN; i++)
		buf[i] = (buf[i] & ~mask[i]) | (addr[i] & mask[i]);
}

/* --- found networks ----------------------------------------------------------------------- */

struct cfg80211_bss_ies {
	u64 tsf;
	struct rcu_head rcu_head;
	int len;
	bool from_beacon;
	u8 data[];
};

struct cfg80211_bss {
	struct ieee80211_channel *channel;
	const struct cfg80211_bss_ies __rcu *ies;
	const struct cfg80211_bss_ies __rcu *beacon_ies;
	const struct cfg80211_bss_ies __rcu *proberesp_ies;
	s32 signal;			/* mBm */
	u16 beacon_interval;
	u16 capability;
	u8 bssid[ETH_ALEN];
	u8 chains;
	s8 chain_signal[IEEE80211_MAX_CHAINS];
	u8 bssid_index;
	u8 max_bssid_indicator;
	u8 priv[] __aligned(sizeof(void *));
};

void cfg80211_bss_flush(struct wiphy *wiphy);
void cfg80211_bss_iter(struct wiphy *wiphy, struct cfg80211_chan_def *chandef,
		       void (*iter)(struct wiphy *wiphy, struct cfg80211_bss *bss, void *data),
		       void *iter_data);

/* --- wake on WLAN, key rekeying --------------------------------------------------------- */

struct cfg80211_wowlan {
	bool any, disconnect, magic_pkt, gtk_rekey_failure, eap_identity_req,
	     four_way_handshake, rfkill_release;
	int n_patterns;
};

struct cfg80211_gtk_rekey_data {
	const u8 *kek, *kck, *replay_ctr;
	u32 akm;
	u8 kek_len, kck_len;
};

/* --- peer measurement (fine timing: distance to an AP) ----------------------------------- */

/* Hardware timestamps of frames to/from one peer (NULL: all). */
struct cfg80211_set_hw_timestamp {
	const u8 *macaddr;
	bool enable;
};

struct cfg80211_ftm_responder_stats {
	u32 filled;
	u32 success_num, partial_num, failed_num, asap_num, non_asap_num;
	u64 total_duration_ms;
	u32 unknown_triggers_num, reschedule_requests_num, out_of_window_triggers_num;
};

struct cfg80211_pmsr_ftm_result {
	const u8 *lci;
	const u8 *civicloc;
	unsigned int lci_len;
	unsigned int civicloc_len;
	enum nl80211_peer_measurement_ftm_failure_reasons failure_reason;
	u32 num_ftmr_attempts, num_ftmr_successes;
	s16 burst_index;
	u8 busy_retry_time;
	u8 num_bursts_exp;
	u8 burst_duration;
	u8 ftms_per_burst;
	u16 burst_period;
	s32 rssi_avg;
	s32 rssi_spread;
	struct rate_info tx_rate, rx_rate;
	s64 rtt_avg, rtt_variance, rtt_spread;
	s64 dist_avg, dist_variance, dist_spread;
	u32 num_ftmr_attempts_valid:1,
	    num_ftmr_successes_valid:1,
	    rssi_avg_valid:1,
	    rssi_spread_valid:1,
	    tx_rate_valid:1,
	    rx_rate_valid:1,
	    rtt_avg_valid:1,
	    rtt_variance_valid:1,
	    rtt_spread_valid:1,
	    dist_avg_valid:1,
	    dist_variance_valid:1,
	    dist_spread_valid:1;
};

struct cfg80211_pmsr_result {
	u64 host_time, ap_tsf;
	enum nl80211_peer_measurement_status status;
	u8 addr[ETH_ALEN];
	u8 final:1, ap_tsf_valid:1;
	enum nl80211_peer_measurement_type type;
	union {
		struct cfg80211_pmsr_ftm_result ftm;
	};
};

struct cfg80211_pmsr_ftm_request_peer {
	enum nl80211_preamble preamble;
	u16 burst_period;
	u8 requested:1, asap:1, request_lci:1, request_civicloc:1,
	   trigger_based:1, non_trigger_based:1, lmr_feedback:1;
	u8 num_bursts_exp;
	u8 burst_duration;
	u8 ftms_per_burst;
	u8 ftmr_retries;
	u8 bss_color;
};

struct cfg80211_pmsr_request_peer {
	u8 addr[ETH_ALEN];
	struct cfg80211_chan_def chandef;
	u8 report_ap_tsf:1;
	struct cfg80211_pmsr_ftm_request_peer ftm;
};

struct cfg80211_pmsr_request {
	u64 cookie;
	void *drv_data;
	u32 n_peers;
	u32 nl_portid;
	u32 timeout;
	u8 mac_addr[ETH_ALEN] __aligned(2);
	u8 mac_addr_mask[ETH_ALEN] __aligned(2);
	struct list_head list;
	struct cfg80211_pmsr_request_peer peers[];
};

struct cfg80211_pmsr_capabilities {
	unsigned int max_peers;
	u8 report_ap_tsf:1, randomize_mac_addr:1;
	struct {
		u32 preambles;
		u32 bandwidths;
		s8 max_bursts_exponent;
		u8 max_ftms_per_burst;
		u8 supported:1, asap:1, non_asap:1, request_lci:1, request_civicloc:1,
		   trigger_based:1, non_trigger_based:1;
	} ftm;
};

struct wireless_dev;
void cfg80211_pmsr_report(struct wireless_dev *wdev, struct cfg80211_pmsr_request *req,
			  struct cfg80211_pmsr_result *result, gfp_t gfp);
void cfg80211_pmsr_complete(struct wireless_dev *wdev, struct cfg80211_pmsr_request *req,
			    gfp_t gfp);

/* --- the radio ---------------------------------------------------------------------------- */

/* How many interfaces of which types may run at once. */
struct ieee80211_iface_limit {
	u16 max;
	u16 types;			/* BIT(NL80211_IFTYPE_...) */
};

struct ieee80211_iface_combination {
	const struct ieee80211_iface_limit *limits;
	u32 num_different_channels;
	u16 max_interfaces;
	u8 n_limits;
	bool beacon_int_infra_match;
	u8 radar_detect_widths;
};

enum wiphy_flags {
	WIPHY_FLAG_SUPPORTS_EXT_KEK_KCK	= BIT(0),
	WIPHY_FLAG_SUPPORTS_MLO		= BIT(1),
	WIPHY_FLAG_SPLIT_SCAN_6GHZ	= BIT(2),
	WIPHY_FLAG_NETNS_OK		= BIT(3),
	WIPHY_FLAG_PS_ON_BY_DEFAULT	= BIT(4),
	WIPHY_FLAG_4ADDR_AP		= BIT(5),
	WIPHY_FLAG_4ADDR_STATION	= BIT(6),
	WIPHY_FLAG_CONTROL_PORT_PROTOCOL = BIT(7),
	WIPHY_FLAG_IBSS_RSN		= BIT(8),
	WIPHY_FLAG_DISABLE_WEXT		= BIT(9),
	WIPHY_FLAG_MESH_AUTH		= BIT(10),
	WIPHY_FLAG_SUPPORTS_FW_ROAM	= BIT(13),
	WIPHY_FLAG_AP_UAPSD		= BIT(14),
	WIPHY_FLAG_SUPPORTS_TDLS	= BIT(15),
	WIPHY_FLAG_TDLS_EXTERNAL_SETUP	= BIT(16),
	WIPHY_FLAG_HAVE_AP_SME		= BIT(17),
	WIPHY_FLAG_REPORTS_OBSS		= BIT(18),
	WIPHY_FLAG_AP_PROBE_RESP_OFFLOAD = BIT(19),
	WIPHY_FLAG_OFFCHAN_TX		= BIT(20),
	WIPHY_FLAG_HAS_REMAIN_ON_CHANNEL = BIT(21),
	WIPHY_FLAG_HAS_CHANNEL_SWITCH	= BIT(23),
	WIPHY_FLAG_NOTIFY_REGDOM_BY_DRIVER = BIT(24),
	WIPHY_FLAG_CHANNEL_CHANGE_ON_BEACON = BIT(25),
};

enum wiphy_wowlan_support_flags {
	WIPHY_WOWLAN_ANY		= BIT(0),
	WIPHY_WOWLAN_MAGIC_PKT		= BIT(1),
	WIPHY_WOWLAN_DISCONNECT		= BIT(2),
	WIPHY_WOWLAN_SUPPORTS_GTK_REKEY	= BIT(3),
	WIPHY_WOWLAN_GTK_REKEY_FAILURE	= BIT(4),
	WIPHY_WOWLAN_EAP_IDENTITY_REQ	= BIT(5),
	WIPHY_WOWLAN_4WAY_HANDSHAKE	= BIT(6),
	WIPHY_WOWLAN_RFKILL_RELEASE	= BIT(7),
	WIPHY_WOWLAN_NET_DETECT		= BIT(8),
};

struct wiphy_wowlan_support {
	u32 flags;
	int n_patterns;
	int pattern_max_len;
	int pattern_min_len;
	int max_pkt_offset;
	int max_nd_match_sets;
};

struct wiphy_iftype_ext_capab {
	enum nl80211_iftype iftype;
	const u8 *extended_capabilities;
	const u8 *extended_capabilities_mask;
	u8 extended_capabilities_len;
	u16 eml_capabilities;
	u16 mld_capa_and_ops;
};

struct mac_address {
	u8 addr[ETH_ALEN];
};

#define ETHTOOL_FWVERS_LEN	32

struct wiphy {
	struct mutex mtx;		/* the "wiphy lock": most configuration runs under it */

	u8 perm_addr[ETH_ALEN];
	u8 addr_mask[ETH_ALEN];
	struct mac_address *addresses;
	u16 n_addresses;

	const struct ieee80211_iface_combination *iface_combinations;
	int n_iface_combinations;
	u16 interface_modes;		/* BIT(NL80211_IFTYPE_...) */

	u32 flags;			/* enum wiphy_flags */
	u32 regulatory_flags;		/* enum ieee80211_regulatory_flags */
	u32 features;			/* enum nl80211_feature_flags */
	u8 ext_features[DIV_ROUND_UP(NUM_NL80211_EXT_FEATURES, 8)];

	enum cfg80211_signal_type { CFG80211_SIGNAL_TYPE_NONE, CFG80211_SIGNAL_TYPE_MBM,
				    CFG80211_SIGNAL_TYPE_UNSPEC } signal_type;

	u8 max_scan_ssids;
	u8 max_sched_scan_reqs;
	u8 max_sched_scan_ssids;
	u8 max_match_sets;
	u16 max_scan_ie_len;
	u16 max_sched_scan_ie_len;
	u32 max_sched_scan_plans;
	u32 max_sched_scan_plan_interval;
	u32 max_sched_scan_plan_iterations;

	int n_cipher_suites;
	const u32 *cipher_suites;
	u32 rts_threshold;
	char fw_version[ETHTOOL_FWVERS_LEN];
	u32 hw_version;
	const struct wiphy_wowlan_support *wowlan;
	struct cfg80211_wowlan *wowlan_config;
	u16 max_remain_on_channel_duration;
	u32 available_antennas_tx;
	u32 available_antennas_rx;

	const u8 *extended_capabilities, *extended_capabilities_mask;
	u8 extended_capabilities_len;
	const struct wiphy_iftype_ext_capab *iftype_ext_capab;
	unsigned int num_iftype_ext_capab;

	struct ieee80211_supported_band *bands[NUM_NL80211_BANDS];

	void (*reg_notifier)(struct wiphy *wiphy, struct regulatory_request *request);
	const struct ieee80211_regdomain __rcu *regd;

	struct device dev;
	bool registered;
	const struct cfg80211_pmsr_capabilities *pmsr_capa;
	u16 hw_timestamp_max_peers;
	struct rfkill *rfkill;
	void *radio_cfg;

	u8 priv[] __aligned(32);	/* the driver's (mac80211's) own data */
};

static inline void *wiphy_priv(struct wiphy *wiphy) { return &wiphy->priv; }
static inline struct device *wiphy_dev(struct wiphy *wiphy) { return wiphy->dev.parent; }
static inline const char *wiphy_name(const struct wiphy *wiphy) { return dev_name(&wiphy->dev); }

static inline bool wiphy_ext_feature_isset(struct wiphy *wiphy, enum nl80211_ext_feature_index f)
{
	return wiphy->ext_features[f / 8] & BIT(f % 8);
}
static inline void wiphy_ext_feature_set(struct wiphy *wiphy, enum nl80211_ext_feature_index f)
{
	wiphy->ext_features[f / 8] |= BIT(f % 8);
}

static inline void wiphy_lock(struct wiphy *wiphy) { mutex_lock(&wiphy->mtx); }
static inline void wiphy_unlock(struct wiphy *wiphy) { mutex_unlock(&wiphy->mtx); }
#define lockdep_assert_wiphy(wiphy)	lockdep_assert_held(&(wiphy)->mtx)
#define wiphy_dereference(wiphy, p)	rcu_dereference_protected(p, lockdep_is_held(&(wiphy)->mtx))

/* Work that runs with the wiphy lock held, one item at a time. */
struct wiphy_work;
typedef void (*wiphy_work_func_t)(struct wiphy *, struct wiphy_work *);
struct wiphy_work {
	struct list_head entry;
	wiphy_work_func_t func;
};
static inline void wiphy_work_init(struct wiphy_work *work, wiphy_work_func_t func)
{
	INIT_LIST_HEAD(&work->entry);
	work->func = func;
}
void wiphy_work_queue(struct wiphy *wiphy, struct wiphy_work *work);
void wiphy_work_cancel(struct wiphy *wiphy, struct wiphy_work *work);
void wiphy_work_flush(struct wiphy *wiphy, struct wiphy_work *work);

/* The hardware radio switch. */
void wiphy_rfkill_set_hw_state_reason(struct wiphy *wiphy, bool blocked,
				      enum rfkill_hard_block_reasons reason);
static inline void wiphy_rfkill_set_hw_state(struct wiphy *wiphy, bool blocked)
{
	wiphy_rfkill_set_hw_state_reason(wiphy, blocked, RFKILL_HARD_BLOCK_SIGNAL);
}

/* Regulatory: the driver's own rules for where it may transmit (from
   the card's firmware), and the wireless-regdb's WMM limits. */
int regulatory_set_wiphy_regd(struct wiphy *wiphy, struct ieee80211_regdomain *rd);
int regulatory_set_wiphy_regd_sync(struct wiphy *wiphy, struct ieee80211_regdomain *rd);
int reg_query_regdb_wmm(char *alpha2, int freq, struct ieee80211_reg_rule *rule);

void cfg80211_shutdown_all_interfaces(struct wiphy *wiphy);

/* --- an interface --------------------------------------------------------------------------- */

struct wireless_dev {
	struct wiphy *wiphy;
	enum nl80211_iftype iftype;
	struct list_head list;
	struct net_device *netdev;
	u32 identifier;
	u8 address[ETH_ALEN] __aligned(sizeof(u16));
	bool use_4addr;
	bool ps;			/* power save on */
	u16 valid_links;		/* Wi-Fi 7: which links are set up */
	union {
		struct {
			u8 connected_addr[ETH_ALEN] __aligned(2);
			u8 ssid[IEEE80211_MAX_SSID_LEN];
			u8 ssid_len;
		} client;
	} u;
};

/* A management frame that should have been protected (802.11w) but wasn't. */
void cfg80211_rx_unprot_mlme_mgmt(struct net_device *dev, const u8 *buf, size_t len);

#endif
