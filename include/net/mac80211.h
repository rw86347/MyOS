/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef NET_MAC80211_H
#define NET_MAC80211_H

/*
 * net/mac80211.h — the driver side of MyOS's 802.11 core: what a "soft
 * MAC" Wi-Fi driver gives the core and gets from it, with Linux's names
 * (docs/WIFI-PLAN.md). MyOS's own file, written for the core and the
 * drivers it carries; only what those use.
 *
 * The model, as in Linux:
 *   ieee80211_hw     the driver's handle on the core: one per radio. The
 *                    driver allocates it with room for its own data (priv),
 *                    sets flags (what the hardware does itself), registers.
 *   ieee80211_ops    the driver's callbacks: start, add an interface, scan,
 *                    set a key, send a frame (tx), and so on.
 *   ieee80211_vif    one virtual interface (station, AP, ...).
 *   ieee80211_sta    a peer: our AP when we are a station.
 *   ieee80211_txq    a queue of frames waiting for one peer and priority;
 *                    the driver pulls from it (ieee80211_tx_dequeue).
 * Received frames go up with ieee80211_rx_napi; sent frames come back
 * with their status (ieee80211_tx_status_*).
 *
 * vif, sta, txq and chanctx each end in drv_priv[]: the driver's own
 * per-object data, sized by the *_data_size fields of ieee80211_hw.
 */
#include <linux/atomic.h>
#include <linux/ieee80211.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/unaligned.h>
#include <net/ieee80211_radiotap.h>
#include <linux/ktime.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>
#include <net/cfg80211.h>

struct ieee80211_vif;
struct ieee80211_sta;
struct netdev_hw_addr_list;
struct ieee80211_hw;
struct dentry;

/* --- queues --------------------------------------------------------------------------- */

#define IEEE80211_MAX_QUEUES		16
#define IEEE80211_MAX_QUEUE_MAP		BIT(IEEE80211_MAX_QUEUES) - 1
#define IEEE80211_INVAL_HW_QUEUE	0xff

/* The four access categories of WMM (QoS), highest priority first. */
enum ieee80211_ac_numbers {
	IEEE80211_AC_VO = 0,	/* voice */
	IEEE80211_AC_VI = 1,	/* video */
	IEEE80211_AC_BE = 2,	/* best effort */
	IEEE80211_AC_BK = 3,	/* background */
};

/* How one access category contends for the air. */
struct ieee80211_tx_queue_params {
	u16 txop;
	u16 cw_min;
	u16 cw_max;
	u8 aifs;
	bool acm;
	bool uapsd;
	bool mu_edca;
	struct ieee80211_he_mu_edca_param_ac_rec mu_edca_param_rec;
};

/* --- channel contexts: a channel the radio is tuned to, shared by vifs ------------------- */

enum ieee80211_chanctx_change {
	IEEE80211_CHANCTX_CHANGE_WIDTH		= BIT(0),
	IEEE80211_CHANCTX_CHANGE_RX_CHAINS	= BIT(1),
	IEEE80211_CHANCTX_CHANGE_RADAR		= BIT(2),
	IEEE80211_CHANCTX_CHANGE_CHANNEL	= BIT(3),
	IEEE80211_CHANCTX_CHANGE_MIN_DEF	= BIT(4),
	IEEE80211_CHANCTX_CHANGE_AP		= BIT(5),
	IEEE80211_CHANCTX_CHANGE_PUNCTURING	= BIT(6),
};

struct ieee80211_chan_req {
	struct cfg80211_chan_def oper;	/* what we use */
	struct cfg80211_chan_def ap;	/* what the AP uses, if wider */
};

struct ieee80211_chanctx_conf {
	struct cfg80211_chan_def def;
	struct cfg80211_chan_def min_def;	/* the narrowest every user can live with */
	struct cfg80211_chan_def ap;
	int radio_idx;
	u8 rx_chains_static, rx_chains_dynamic;
	bool radar_enabled;
	u8 drv_priv[] __aligned(sizeof(void *));
};

enum ieee80211_chanctx_switch_mode {
	CHANCTX_SWMODE_REASSIGN_VIF,
	CHANCTX_SWMODE_SWAP_CONTEXTS,
};

struct ieee80211_vif_chanctx_switch {
	struct ieee80211_vif *vif;
	struct ieee80211_bss_conf *link_conf;
	struct ieee80211_chanctx_conf *old_ctx;
	struct ieee80211_chanctx_conf *new_ctx;
};

/* --- what changed (bss_info_changed / link_info_changed / vif_cfg_changed) -------------- */

enum ieee80211_bss_change {
	BSS_CHANGED_ASSOC		= 1 << 0,
	BSS_CHANGED_ERP_CTS_PROT	= 1 << 1,
	BSS_CHANGED_ERP_PREAMBLE	= 1 << 2,
	BSS_CHANGED_ERP_SLOT		= 1 << 3,
	BSS_CHANGED_HT			= 1 << 4,
	BSS_CHANGED_BASIC_RATES		= 1 << 5,
	BSS_CHANGED_BEACON_INT		= 1 << 6,
	BSS_CHANGED_BSSID		= 1 << 7,
	BSS_CHANGED_BEACON		= 1 << 8,
	BSS_CHANGED_BEACON_ENABLED	= 1 << 9,
	BSS_CHANGED_CQM			= 1 << 10,
	BSS_CHANGED_IBSS		= 1 << 11,
	BSS_CHANGED_ARP_FILTER		= 1 << 12,
	BSS_CHANGED_QOS			= 1 << 13,
	BSS_CHANGED_IDLE		= 1 << 14,
	BSS_CHANGED_SSID		= 1 << 15,
	BSS_CHANGED_AP_PROBE_RESP	= 1 << 16,
	BSS_CHANGED_PS			= 1 << 17,
	BSS_CHANGED_TXPOWER		= 1 << 18,
	BSS_CHANGED_P2P_PS		= 1 << 19,
	BSS_CHANGED_BEACON_INFO		= 1 << 20,
	BSS_CHANGED_BANDWIDTH		= 1 << 21,
	BSS_CHANGED_OCB			= 1 << 22,
	BSS_CHANGED_MU_GROUPS		= 1 << 23,
	BSS_CHANGED_KEEP_ALIVE		= 1 << 24,
	BSS_CHANGED_MCAST_RATE		= 1 << 25,
	BSS_CHANGED_FTM_RESPONDER	= 1 << 26,
	BSS_CHANGED_TWT			= 1 << 27,
	BSS_CHANGED_HE_OBSS_PD		= 1 << 28,
	BSS_CHANGED_HE_BSS_COLOR	= 1 << 29,
	BSS_CHANGED_FILS_DISCOVERY	= 1 << 30,
	BSS_CHANGED_UNSOL_BCAST_PROBE_RESP = BIT_ULL(31),
	BSS_CHANGED_MLD_VALID_LINKS	= BIT_ULL(33),
	BSS_CHANGED_MLD_TTLM		= BIT_ULL(34),
	BSS_CHANGED_TPE			= BIT_ULL(35),
};

/* --- events the core passes on (event_callback) --------------------------------------------- */

enum ieee80211_event_type { RSSI_EVENT, MLME_EVENT, BAR_RX_EVENT, BA_FRAME_TIMEOUT };
enum ieee80211_rssi_event_data { RSSI_EVENT_HIGH, RSSI_EVENT_LOW };
enum ieee80211_mlme_event_data { AUTH_EVENT, ASSOC_EVENT, DEAUTH_RX_EVENT, DEAUTH_TX_EVENT };
enum ieee80211_mlme_event_status { MLME_SUCCESS, MLME_DENIED, MLME_TIMEOUT };

struct ieee80211_rssi_event {
	enum ieee80211_rssi_event_data data;
};

struct ieee80211_mlme_event {
	enum ieee80211_mlme_event_data data;
	enum ieee80211_mlme_event_status status;
	u16 reason;
};

struct ieee80211_ba_event {
	struct ieee80211_sta *sta;
	u16 tid;
	u16 ssn;
};

struct ieee80211_event {
	enum ieee80211_event_type type;
	union {
		struct ieee80211_rssi_event rssi;
		struct ieee80211_mlme_event mlme;
		struct ieee80211_ba_event ba;
	} u;
};

/* --- one link of an interface (Wi-Fi 7 has several; before it, just one) ------------------- */

struct ieee80211_mu_group_data {
	u8 membership[WLAN_MEMBERSHIP_LEN];
	u8 position[WLAN_USER_POSITION_LEN];
};

struct ieee80211_ftm_responder_params {
	const u8 *lci;
	const u8 *civicloc;
	size_t lci_len;
	size_t civicloc_len;
};

/* Transmit power limits the AP announced (TPE element). */
#define IEEE80211_TPE_EIRP_ENTRIES_320MHZ	5
#define IEEE80211_TPE_PSD_ENTRIES_320MHZ	16

struct ieee80211_parsed_tpe_eirp {
	bool valid;
	s8 power[IEEE80211_TPE_EIRP_ENTRIES_320MHZ];
	u8 count;
};

struct ieee80211_parsed_tpe_psd {
	bool valid;
	s8 power[IEEE80211_TPE_PSD_ENTRIES_320MHZ];
	u8 count, n;
};

struct ieee80211_parsed_tpe {
	struct ieee80211_parsed_tpe_eirp max_local[2], max_reg_client[2];
	struct ieee80211_parsed_tpe_psd psd_local[2], psd_reg_client[2];
};

/* What the core knows about the network on one link (BSS parameters). */
struct ieee80211_bss_conf {
	struct ieee80211_vif *vif;
	struct cfg80211_bss *bss;
	const u8 *bssid;
	unsigned int link_id;
	u8 addr[ETH_ALEN] __aligned(2);
	u8 htc_trig_based_pkt_ext;
	bool uora_exists;
	u8 uora_ocw_range;
	u16 frame_time_rts_th;
	bool he_support;
	bool twt_requester;
	bool twt_responder;
	bool twt_protected;
	bool twt_broadcast;
	bool use_cts_prot;
	bool use_short_preamble;
	bool use_short_slot;
	bool enable_beacon;
	u8 dtim_period;
	u16 beacon_int;			/* TUs */
	u16 assoc_capability;
	u64 sync_tsf;			/* the AP's clock at the last beacon... */
	u32 sync_device_ts;		/* ...and ours then */
	u8 sync_dtim_count;
	u32 basic_rates;		/* bitmap into the band's bitrates */
	struct ieee80211_rate *beacon_rate;
	int mcast_rate[NUM_NL80211_BANDS];
	u16 ht_operation_mode;
	s32 cqm_rssi_thold;		/* report when the signal crosses this (dBm) */
	u32 cqm_rssi_hyst;
	s32 cqm_rssi_low;
	s32 cqm_rssi_high;
	struct ieee80211_chan_req chanreq;
	struct ieee80211_mu_group_data mu_group;
	bool qos;
	bool hidden_ssid;
	int txpower;			/* dBm */
	enum nl80211_tx_power_setting txpower_type;
	struct ieee80211_p2p_noa_attr p2p_noa_attr;
	bool allow_p2p_go_ps;
	u16 max_idle_period;
	bool protected_keep_alive;
	bool ftm_responder;
	struct ieee80211_ftm_responder_params *ftmr_params;
	bool nontransmitted;		/* (multiple BSSID: a network hidden in another's beacon) */
	u8 transmitter_bssid[ETH_ALEN];
	u8 bssid_index;
	u8 bssid_indicator;
	bool ema_ap;
	u8 profile_periodicity;
	struct {
		u32 params;
		u16 nss_set;
	} he_oper;
	struct cfg80211_he_bss_color he_bss_color;
	struct cfg80211_bitrate_mask beacon_tx_rate;
	enum ieee80211_ap_reg_power power_type;
	struct ieee80211_parsed_tpe tpe;
	u8 pwr_reduction;
	bool eht_support;
	bool epcs_support;
	bool uhr_support;
	bool csa_active;
	bool mu_mimo_owner;
	struct ieee80211_chanctx_conf __rcu *chanctx_conf;
	bool color_change_active;
	u8 color_change_color;
	bool ht_ldpc, vht_ldpc, he_ldpc;
	bool vht_su_beamformer, vht_su_beamformee, vht_mu_beamformer, vht_mu_beamformee;
	bool he_su_beamformer, he_su_beamformee, he_mu_beamformer, he_full_ul_mumimo;
	bool eht_su_beamformer, eht_su_beamformee, eht_mu_beamformer;
	bool eht_80mhz_full_bw_ul_mumimo;
	bool eht_disable_mcs15;
	u8 bss_param_ch_cnt;
	u8 bss_param_ch_cnt_link_id;
};

/* --- frames to send: skb->cb holds a struct ieee80211_tx_info ------------------------------------- */

enum mac80211_tx_info_flags {
	IEEE80211_TX_CTL_REQ_TX_STATUS		= BIT(0),
	IEEE80211_TX_CTL_ASSIGN_SEQ		= BIT(1),	/* the driver/firmware numbers it */
	IEEE80211_TX_CTL_NO_ACK			= BIT(2),
	IEEE80211_TX_CTL_CLEAR_PS_FILT		= BIT(3),
	IEEE80211_TX_CTL_FIRST_FRAGMENT		= BIT(4),
	IEEE80211_TX_CTL_SEND_AFTER_DTIM	= BIT(5),
	IEEE80211_TX_CTL_AMPDU			= BIT(6),
	IEEE80211_TX_CTL_INJECTED		= BIT(7),
	IEEE80211_TX_STAT_TX_FILTERED		= BIT(8),
	IEEE80211_TX_STAT_ACK			= BIT(9),	/* the receiver acknowledged it */
	IEEE80211_TX_STAT_AMPDU			= BIT(10),
	IEEE80211_TX_STAT_AMPDU_NO_BACK		= BIT(11),
	IEEE80211_TX_CTL_RATE_CTRL_PROBE	= BIT(12),
	IEEE80211_TX_INTFL_OFFCHAN_TX_OK	= BIT(13),
	IEEE80211_TX_CTL_HW_80211_ENCAP		= BIT(14),
	IEEE80211_TX_INTFL_RETRIED		= BIT(15),
	IEEE80211_TX_INTFL_DONT_ENCRYPT		= BIT(16),
	IEEE80211_TX_CTL_NO_PS_BUFFER		= BIT(17),
	IEEE80211_TX_CTL_MORE_FRAMES		= BIT(18),
	IEEE80211_TX_INTFL_RETRANSMISSION	= BIT(19),
	IEEE80211_TX_INTFL_MLME_CONN_TX		= BIT(20),
	IEEE80211_TX_INTFL_NL80211_FRAME_TX	= BIT(21),
	IEEE80211_TX_CTL_LDPC			= BIT(22),
	IEEE80211_TX_CTL_STBC			= BIT(23) | BIT(24),
	IEEE80211_TX_CTL_TX_OFFCHAN		= BIT(25),
	IEEE80211_TX_INTFL_TKIP_MIC_FAILURE	= BIT(26),
	IEEE80211_TX_CTL_NO_CCK_RATE		= BIT(27),
	IEEE80211_TX_STATUS_EOSP		= BIT(28),
	IEEE80211_TX_CTL_USE_MINRATE		= BIT(29),
	IEEE80211_TX_CTL_DONTFRAG		= BIT(30),
	IEEE80211_TX_STAT_NOACK_TRANSMITTED	= BIT(31),
};
#define IEEE80211_TX_CTL_STBC_SHIFT		23

enum mac80211_tx_control_flags {
	IEEE80211_TX_CTRL_PORT_CTRL_PROTO	= BIT(0),	/* an EAPOL (key handshake) frame */
	IEEE80211_TX_CTRL_PS_RESPONSE		= BIT(1),
	IEEE80211_TX_CTRL_RATE_INJECT		= BIT(2),
	IEEE80211_TX_CTRL_AMSDU			= BIT(3),
	IEEE80211_TX_CTRL_FAST_XMIT		= BIT(4),
	IEEE80211_TX_CTRL_SKIP_MPATH_LOOKUP	= BIT(5),
	IEEE80211_TX_INTCFL_NEED_TXPROCESSING	= BIT(6),
	IEEE80211_TX_CTRL_NO_SEQNO		= BIT(7),
	IEEE80211_TX_CTRL_DONT_REORDER		= BIT(8),
	IEEE80211_TX_CTRL_MCAST_MLO_FIRST_TX	= BIT(9),
	IEEE80211_TX_CTRL_DONT_USE_RATE_MASK	= BIT(10),
	IEEE80211_TX_CTRL_MLO_LINK		= 0xf0000000,	/* which link (Wi-Fi 7) */
};
#define IEEE80211_TX_CTRL_MLO_LINK_UNSPEC	0xf
#define IEEE80211_LINK_UNSPECIFIED		0xf

enum mac80211_rate_control_flags {
	IEEE80211_TX_RC_USE_RTS_CTS		= BIT(0),
	IEEE80211_TX_RC_USE_CTS_PROTECT		= BIT(1),
	IEEE80211_TX_RC_USE_SHORT_PREAMBLE	= BIT(2),
	IEEE80211_TX_RC_MCS			= BIT(3),
	IEEE80211_TX_RC_GREEN_FIELD		= BIT(4),
	IEEE80211_TX_RC_40_MHZ_WIDTH		= BIT(5),
	IEEE80211_TX_RC_DUP_DATA		= BIT(6),
	IEEE80211_TX_RC_SHORT_GI		= BIT(7),
	IEEE80211_TX_RC_VHT_MCS			= BIT(8),
	IEEE80211_TX_RC_80_MHZ_WIDTH		= BIT(9),
	IEEE80211_TX_RC_160_MHZ_WIDTH		= BIT(10),
};

#define IEEE80211_TX_INFO_DRIVER_DATA_SIZE	40
#define IEEE80211_TX_INFO_RATE_DRIVER_DATA_SIZE	24
#define IEEE80211_TX_MAX_RATES			4
#define IEEE80211_TX_RATE_TABLE_SIZE		4

/* One rate try: an index (legacy: into the band's bitrates; HT: the MCS;
   VHT: MCS in bits 0-3, streams-1 in 4-6) and how often to try it. */
struct ieee80211_tx_rate {
	s8 idx;
	u16 count:5, flags:11;		/* IEEE80211_TX_RC_* */
} __packed;

static inline void ieee80211_rate_set_vht(struct ieee80211_tx_rate *rate, u8 mcs, u8 nss)
{
	WARN_ON(mcs & ~0xf);
	WARN_ON((nss - 1) & ~0x7);
	rate->idx = (s8)(((nss - 1) << 4) | mcs);
}
static inline u8 ieee80211_rate_get_vht_mcs(const struct ieee80211_tx_rate *rate) { return rate->idx & 0xf; }
static inline u8 ieee80211_rate_get_vht_nss(const struct ieee80211_tx_rate *rate) { return (rate->idx >> 4) + 1; }

/* Everything about a frame on its way out and back: in skb->cb, so it
   must fit in 48 bytes. "control" while queued, "status" once sent. */
struct ieee80211_tx_info {
	u32 flags;			/* enum mac80211_tx_info_flags */
	u32 band:3,
	    status_data_idr:1,
	    status_data:13,
	    hw_queue:4,
	    tx_time_est:10;
	union {
		struct {
			union {
				struct {
					struct ieee80211_tx_rate rates[IEEE80211_TX_MAX_RATES];
					s8 rts_cts_rate_idx;
					u8 use_rts:1;
					u8 use_cts_prot:1;
					u8 short_preamble:1;
					u8 skip_table:1;
					u8 antennas:2;
				};
				unsigned long jiffies;
			};
			struct ieee80211_vif *vif;
			struct ieee80211_key_conf *hw_key;
			u32 flags;	/* enum mac80211_tx_control_flags */
			u32 enqueue_time;
		} control;
		struct {
			u64 cookie;
		} ack;
		struct {
			struct ieee80211_tx_rate rates[IEEE80211_TX_MAX_RATES];
			s32 ack_signal;
			u8 ampdu_ack_len;
			u8 ampdu_len;
			u8 antenna;
			u8 pad;
			u16 tx_time;
			u8 flags;
			u8 pad2;
			void *status_driver_data[16 / sizeof(void *)];
		} status;
		struct {
			struct ieee80211_tx_rate driver_rates[IEEE80211_TX_MAX_RATES];
			u8 pad[4];
			void *rate_driver_data[IEEE80211_TX_INFO_RATE_DRIVER_DATA_SIZE / sizeof(void *)];
		};
		void *driver_data[IEEE80211_TX_INFO_DRIVER_DATA_SIZE / sizeof(void *)];
	};
};

static inline struct ieee80211_tx_info *IEEE80211_SKB_CB(struct sk_buff *skb)
{
	return (struct ieee80211_tx_info *)skb->cb;
}

struct ieee80211_rate_status {
	struct rate_info rate_idx;
	u8 try_count;
	u8 tx_power_idx;
};

/* A finished transmission, reported back in full (ieee80211_tx_status_ext). */
struct ieee80211_tx_status {
	struct ieee80211_sta *sta;
	struct ieee80211_tx_info *info;
	struct sk_buff *skb;
	struct ieee80211_rate_status *rates;
	ktime_t ack_hwtstamp;
	u8 n_rates;
	struct list_head *free_list;
};

/* --- frames received: skb->cb holds a struct ieee80211_rx_status -------------------------------- */

enum mac80211_rx_flags {
	RX_FLAG_MMIC_ERROR		= BIT(0),
	RX_FLAG_DECRYPTED		= BIT(1),	/* the hardware decrypted it */
	RX_FLAG_ONLY_MONITOR		= BIT(2),
	RX_FLAG_MMIC_STRIPPED		= BIT(3),
	RX_FLAG_IV_STRIPPED		= BIT(4),
	RX_FLAG_FAILED_FCS_CRC		= BIT(5),
	RX_FLAG_FAILED_PLCP_CRC		= BIT(6),
	RX_FLAG_MACTIME_IS_RTAP_TS64	= BIT(7),
	RX_FLAG_NO_SIGNAL_VAL		= BIT(8),
	RX_FLAG_AMPDU_DETAILS		= BIT(9),
	RX_FLAG_PN_VALIDATED		= BIT(10),	/* replay already checked */
	RX_FLAG_DUP_VALIDATED		= BIT(11),
	RX_FLAG_AMPDU_LAST_KNOWN	= BIT(12),
	RX_FLAG_AMPDU_IS_LAST		= BIT(13),
	RX_FLAG_AMPDU_DELIM_CRC_ERROR	= BIT(14),
	RX_FLAG_MACTIME			= BIT(16) | BIT(17),
	RX_FLAG_MACTIME_PLCP_START	= 1 << 16,
	RX_FLAG_MACTIME_START		= 2 << 16,
	RX_FLAG_MACTIME_END		= 3 << 16,
	RX_FLAG_SKIP_MONITOR		= BIT(18),
	RX_FLAG_AMSDU_MORE		= BIT(19),
	RX_FLAG_RADIOTAP_TLV_AT_END	= BIT(20),
	RX_FLAG_MIC_STRIPPED		= BIT(21),
	RX_FLAG_ALLOW_SAME_PN		= BIT(22),
	RX_FLAG_ICV_STRIPPED		= BIT(23),
	RX_FLAG_AMPDU_EOF_BIT		= BIT(24),
	RX_FLAG_AMPDU_EOF_BIT_KNOWN	= BIT(25),
	RX_FLAG_RADIOTAP_HE		= BIT(26),
	RX_FLAG_RADIOTAP_HE_MU		= BIT(27),
	RX_FLAG_RADIOTAP_LSIG		= BIT(28),
	RX_FLAG_NO_PSDU			= BIT(29),
	RX_FLAG_8023			= BIT(30),
	RX_FLAG_RADIOTAP_VHT		= BIT(31),
};

enum mac80211_rx_encoding_flags {
	RX_ENC_FLAG_SHORTPRE		= BIT(0),
	RX_ENC_FLAG_SHORT_GI		= BIT(2),
	RX_ENC_FLAG_HT_GF		= BIT(3),
	RX_ENC_FLAG_STBC_MASK		= BIT(4) | BIT(5),
	RX_ENC_FLAG_LDPC		= BIT(6),
	RX_ENC_FLAG_BF			= BIT(7),
};
#define RX_ENC_FLAG_STBC_SHIFT		4

enum mac80211_rx_encoding {
	RX_ENC_LEGACY = 0,
	RX_ENC_HT,
	RX_ENC_VHT,
	RX_ENC_HE,
	RX_ENC_EHT,
	RX_ENC_UHR,
};

/* A received frame's radio facts: when, which channel, how fast, how strong. */
struct ieee80211_rx_status {
	u64 mactime;
	union {
		u64 boottime_ns;
		ktime_t ack_tx_hwtstamp;
	};
	u32 device_timestamp;
	u32 ampdu_reference;
	u32 flag;			/* enum mac80211_rx_flags */
	u16 freq:13, freq_offset:1;	/* MHz */
	u8 enc_flags;
	u8 encoding:3, bw:4;		/* enum mac80211_rx_encoding, enum rate_info_bw */
	union {
		struct {
			u8 he_ru:3;
			u8 he_gi:2;
			u8 he_dcm:1;
		} __packed;
		struct {
			u8 ru:4;
			u8 gi:2;
		} __packed eht;
		struct {
			u8 ru:4;
			u8 gi:2;
			u8 elr:1;
			u8 im:1;
		} __packed uhr;
	} __packed;
	u8 rate_idx;
	u8 nss;
	u8 rx_flags;
	u8 band;
	u8 antenna;
	s8 signal;			/* dBm */
	u8 chains;
	s8 chain_signal[IEEE80211_MAX_CHAINS];
	u8 zero_length_psdu_type;
	u8 link_valid:1, link_id:4;
};

static inline struct ieee80211_rx_status *IEEE80211_SKB_RXCB(struct sk_buff *skb)
{
	return (struct ieee80211_rx_status *)skb->cb;
}

static inline u32 ieee80211_rx_status_to_khz(struct ieee80211_rx_status *rx_status)
{
	return MHZ_TO_KHZ(rx_status->freq) + (rx_status->freq_offset ? 500 : 0);
}

/* --- radio-wide configuration (config callback) --------------------------------------------- */

enum ieee80211_conf_flags {
	IEEE80211_CONF_MONITOR		= 1 << 0,
	IEEE80211_CONF_PS		= 1 << 1,
	IEEE80211_CONF_IDLE		= 1 << 2,
	IEEE80211_CONF_OFFCHANNEL	= 1 << 3,
};

enum ieee80211_conf_changed {
	IEEE80211_CONF_CHANGE_SMPS		= BIT(1),
	IEEE80211_CONF_CHANGE_LISTEN_INTERVAL	= BIT(2),
	IEEE80211_CONF_CHANGE_MONITOR		= BIT(3),
	IEEE80211_CONF_CHANGE_PS		= BIT(4),
	IEEE80211_CONF_CHANGE_POWER		= BIT(5),
	IEEE80211_CONF_CHANGE_CHANNEL		= BIT(6),
	IEEE80211_CONF_CHANGE_RETRY_LIMITS	= BIT(7),
	IEEE80211_CONF_CHANGE_IDLE		= BIT(8),
};

enum ieee80211_smps_mode {
	IEEE80211_SMPS_AUTOMATIC,
	IEEE80211_SMPS_OFF,
	IEEE80211_SMPS_STATIC,
	IEEE80211_SMPS_DYNAMIC,
	IEEE80211_SMPS_NUM_MODES,
};

struct ieee80211_conf {
	u32 flags;			/* enum ieee80211_conf_flags */
	int power_level, dynamic_ps_timeout;
	u16 listen_interval;
	u8 ps_dtim_period;
	u8 long_frame_max_tx_count, short_frame_max_tx_count;
	struct cfg80211_chan_def chandef;
	bool radar_enabled;
	enum ieee80211_smps_mode smps_mode;
};

/* The AP moving to another channel (CSA). */
struct ieee80211_channel_switch {
	u64 timestamp;
	u32 device_timestamp;
	bool block_tx;
	struct cfg80211_chan_def chandef;
	u8 count;
	u8 link_id;
	u32 delay;
};

/* --- a virtual interface -------------------------------------------------------------------------- */

enum ieee80211_vif_flags {
	IEEE80211_VIF_BEACON_FILTER		= BIT(0),
	IEEE80211_VIF_SUPPORTS_CQM_RSSI		= BIT(1),
	IEEE80211_VIF_SUPPORTS_UAPSD		= BIT(2),
	IEEE80211_VIF_GET_NOA_UPDATE		= BIT(3),
	IEEE80211_VIF_EML_ACTIVE		= BIT(4),
	IEEE80211_VIF_IGNORE_OFDMA_WIDER_BW	= BIT(5),
	IEEE80211_VIF_REMOVE_AP_AFTER_DISASSOC	= BIT(6),
};

enum ieee80211_offload_flags {
	IEEE80211_OFFLOAD_ENCAP_ENABLED		= BIT(0),
	IEEE80211_OFFLOAD_ENCAP_4ADDR		= BIT(1),
	IEEE80211_OFFLOAD_DECAP_ENABLED		= BIT(2),
	IEEE80211_OFFLOAD_ENCAP_MCAST		= BIT(3),
};

#define IEEE80211_BSS_ARP_ADDR_LIST_LEN 4

/* The interface-wide part of the connection (the links have the rest). */
struct ieee80211_vif_cfg {
	bool assoc, ibss_joined;
	bool ibss_creator;
	bool ps;			/* power save on */
	u16 aid;			/* association id the AP gave us */
	u16 eml_cap;
	u16 eml_med_sync_delay;
	u16 mld_capa_op;
	__be32 arp_addr_list[IEEE80211_BSS_ARP_ADDR_LIST_LEN];
	int arp_addr_cnt;
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	size_t ssid_len;
	bool s1g;
	bool idle;
	u8 ap_addr[ETH_ALEN] __aligned(2);
};

struct ieee80211_txq;

struct ieee80211_vif {
	enum nl80211_iftype type;
	struct ieee80211_vif_cfg cfg;
	struct ieee80211_bss_conf bss_conf;	/* link 0 (all of it, before Wi-Fi 7) */
	struct ieee80211_bss_conf __rcu *link_conf[IEEE80211_MLD_MAX_NUM_LINKS];
	u16 valid_links, active_links, dormant_links, suspended_links;
	u8 addr[ETH_ALEN] __aligned(2);
	bool addr_valid;
	bool p2p;
	u8 cab_queue;
	u8 hw_queue[IEEE80211_NUM_ACS];
	struct ieee80211_txq *txq;	/* its multicast queue */
	struct ieee80211_txq *txq_mgmt;
	netdev_features_t netdev_features;
	u32 driver_flags;		/* enum ieee80211_vif_flags */
	u32 offload_flags;		/* enum ieee80211_offload_flags */
	struct dentry *debugfs_dir;
	bool probe_req_reg;
	bool rx_mcast_action_reg;
	u8 drv_priv[] __aligned(sizeof(void *));
};

/* Multi-link (Wi-Fi 7) helpers. Without valid_links there is one link, 0. */
static inline bool ieee80211_vif_is_mld(const struct ieee80211_vif *vif) { return vif->valid_links != 0; }

static inline bool ieee80211_vif_link_active(const struct ieee80211_vif *vif, unsigned int link_id)
{
	if (!ieee80211_vif_is_mld(vif))
		return link_id == 0;
	return vif->active_links & BIT(link_id);
}

static inline enum nl80211_iftype ieee80211_iftype_p2p(enum nl80211_iftype type, bool p2p)
{
	if (p2p) {
		if (type == NL80211_IFTYPE_STATION)
			return NL80211_IFTYPE_P2P_CLIENT;
		if (type == NL80211_IFTYPE_AP)
			return NL80211_IFTYPE_P2P_GO;
	}
	return type;
}
static inline enum nl80211_iftype ieee80211_vif_type_p2p(struct ieee80211_vif *vif)
{
	return ieee80211_iftype_p2p(vif->type, vif->p2p);
}

static inline const struct ieee80211_sta_he_cap *
ieee80211_get_he_iftype_cap_vif(const struct ieee80211_supported_band *sband, struct ieee80211_vif *vif)
{
	return ieee80211_get_he_iftype_cap(sband, ieee80211_vif_type_p2p(vif));
}
static inline const struct ieee80211_sta_eht_cap *
ieee80211_get_eht_iftype_cap_vif(const struct ieee80211_supported_band *sband, struct ieee80211_vif *vif)
{
	return ieee80211_get_eht_iftype_cap(sband, ieee80211_vif_type_p2p(vif));
}

struct wireless_dev *ieee80211_vif_to_wdev(struct ieee80211_vif *vif);

/* Links are reached under the wiphy lock or RCU; on one CPU with no
   preemption here both are plain loads. */
#define link_conf_dereference_protected(vif, link_id)	rcu_dereference_protected((vif)->link_conf[link_id], 1)
#define link_conf_dereference_check(vif, link_id)	rcu_dereference_check((vif)->link_conf[link_id], 1)
#define link_sta_dereference_protected(sta, link_id)	rcu_dereference_protected((sta)->link[link_id], 1)
#define link_sta_dereference_check(sta, link_id)	rcu_dereference_check((sta)->link[link_id], 1)

#define for_each_vif_active_link(vif, link, link_id)					\
	for (link_id = 0; link_id < ARRAY_SIZE((vif)->link_conf); link_id++)		\
		if ((!(vif)->active_links || (vif)->active_links & BIT(link_id)) &&	\
		    (link = link_conf_dereference_check(vif, link_id)))

/* --- keys ---------------------------------------------------------------------------------------- */

enum ieee80211_key_flags {
	IEEE80211_KEY_FLAG_GENERATE_IV_MGMT	= BIT(0),
	IEEE80211_KEY_FLAG_GENERATE_IV		= BIT(1),
	IEEE80211_KEY_FLAG_GENERATE_MMIC	= BIT(2),
	IEEE80211_KEY_FLAG_PAIRWISE		= BIT(3),	/* per-station, not group */
	IEEE80211_KEY_FLAG_SW_MGMT_TX		= BIT(4),
	IEEE80211_KEY_FLAG_PUT_IV_SPACE		= BIT(5),
	IEEE80211_KEY_FLAG_RX_MGMT		= BIT(6),
	IEEE80211_KEY_FLAG_RESERVE_TAILROOM	= BIT(7),
	IEEE80211_KEY_FLAG_PUT_MIC_SPACE	= BIT(8),
	IEEE80211_KEY_FLAG_NO_AUTO_TX		= BIT(9),
	IEEE80211_KEY_FLAG_GENERATE_MMIE	= BIT(10),
	IEEE80211_KEY_FLAG_SPP_AMSDU		= BIT(11),
};

/* A key the hardware encrypts and decrypts with. */
struct ieee80211_key_conf {
	atomic64_t tx_pn;		/* packet number: never reused (replay protection) */
	u32 cipher;			/* WLAN_CIPHER_SUITE_* */
	u8 icv_len;
	u8 iv_len;
	u8 hw_key_idx;			/* the driver's slot for it */
	s8 keyidx;
	u16 flags;			/* enum ieee80211_key_flags */
	s8 link_id;
	u8 keylen;
	u8 key[];
};

#define IEEE80211_MAX_PN_LEN	16

/* A receive sequence counter (the last packet number seen). */
struct ieee80211_key_seq {
	union {
		struct { u32 iv32; u16 iv16; } tkip;
		struct { u8 pn[6]; } ccmp;
		struct { u8 pn[6]; } aes_cmac;
		struct { u8 pn[6]; } aes_gmac;
		struct { u8 pn[6]; } gcmp;
		struct { u8 seq[IEEE80211_MAX_PN_LEN]; u8 seq_len; } hw;
	};
};

enum set_key_cmd { SET_KEY, DISABLE_KEY };

/* --- peers ------------------------------------------------------------------------------------- */

/* A peer's progress: none -> authenticated -> associated -> authorized
   (the 4-way handshake done; data may flow). */
enum ieee80211_sta_state {
	IEEE80211_STA_NOTEXIST,
	IEEE80211_STA_NONE,
	IEEE80211_STA_AUTH,
	IEEE80211_STA_ASSOC,
	IEEE80211_STA_AUTHORIZED,
};

enum ieee80211_sta_rx_bandwidth {
	IEEE80211_STA_RX_BW_20 = 0,
	IEEE80211_STA_RX_BW_40,
	IEEE80211_STA_RX_BW_80,
	IEEE80211_STA_RX_BW_160,
	IEEE80211_STA_RX_BW_320,
};

struct ieee80211_sta_rates {
	struct rcu_head rcu_head;
	struct {
		s8 idx;
		u8 count;
		u8 count_cts;
		u8 count_rts;
		u16 flags;
	} rate[IEEE80211_TX_RATE_TABLE_SIZE];
};

struct ieee80211_sta_txpwr {
	s16 power;
	enum nl80211_tx_power_setting type;
};

/* The biggest frames we may send it (A-MSDU limits). */
struct ieee80211_sta_aggregates {
	u16 max_amsdu_len;
	u16 max_rc_amsdu_len;
	u16 max_tid_amsdu_len[IEEE80211_NUM_TIDS];
};

/* What a peer can do on one link. */
struct ieee80211_link_sta {
	struct ieee80211_sta *sta;
	u8 addr[ETH_ALEN];
	u8 link_id;
	enum ieee80211_smps_mode smps_mode;
	u32 supp_rates[NUM_NL80211_BANDS];
	struct ieee80211_sta_ht_cap ht_cap;
	struct ieee80211_sta_vht_cap vht_cap;
	struct ieee80211_sta_he_cap he_cap;
	struct ieee80211_he_6ghz_capa he_6ghz_capa;
	struct ieee80211_sta_eht_cap eht_cap;
	struct ieee80211_sta_uhr_cap uhr_cap;
	struct ieee80211_sta_aggregates agg;
	u8 rx_nss;
	enum ieee80211_sta_rx_bandwidth bandwidth;
	struct ieee80211_sta_txpwr txpwr;
};

struct ieee80211_sta {
	u8 addr[ETH_ALEN] __aligned(2);
	u16 aid;
	u16 max_rx_aggregation_subframes;
	bool wme;			/* QoS */
	u8 uapsd_queues;
	u8 max_sp;
	struct ieee80211_sta_rates __rcu *rates;
	bool tdls;
	bool tdls_initiator;
	bool mfp;			/* management frame protection */
	bool mlo;
	bool spp_amsdu;
	u8 max_amsdu_subframes;
	u16 eml_cap;
	struct ieee80211_sta_aggregates *cur;
	bool support_p2p_ps;
	struct ieee80211_txq *txq[IEEE80211_NUM_TIDS + 1];	/* one per TID, + management */
	u16 valid_links;
	struct ieee80211_link_sta deflink;
	struct ieee80211_link_sta __rcu *link[IEEE80211_MLD_MAX_NUM_LINKS];
	u8 drv_priv[] __aligned(sizeof(void *));
};

#define for_each_sta_active_link(vif, sta, link_sta, link_id)				\
	for (link_id = 0; link_id < ARRAY_SIZE((sta)->link); link_id++)		\
		if ((!(vif)->active_links || (vif)->active_links & BIT(link_id)) &&	\
		    ((link_sta) = link_sta_dereference_check(sta, link_id)))

enum sta_notify_cmd { STA_NOTIFY_SLEEP, STA_NOTIFY_AWAKE };

struct ieee80211_tx_control {
	struct ieee80211_sta *sta;
};

/* Frames for one peer and TID (or the vif's multicast/management). */
struct ieee80211_txq {
	struct ieee80211_vif *vif;
	struct ieee80211_sta *sta;
	u8 tid;
	u8 ac;
	u8 drv_priv[] __aligned(sizeof(void *));
};

/* --- the hardware -------------------------------------------------------------------------------- */

/* What the hardware (or its firmware) does itself, so the core doesn't.
   Only the numbering is the core's business: drivers use the names. */
enum ieee80211_hw_flags {
	IEEE80211_HW_HAS_RATE_CONTROL,
	IEEE80211_HW_RX_INCLUDES_FCS,
	IEEE80211_HW_HOST_BROADCAST_PS_BUFFERING,
	IEEE80211_HW_SIGNAL_UNSPEC,
	IEEE80211_HW_SIGNAL_DBM,
	IEEE80211_HW_NEED_DTIM_BEFORE_ASSOC,
	IEEE80211_HW_SPECTRUM_MGMT,
	IEEE80211_HW_AMPDU_AGGREGATION,
	IEEE80211_HW_SUPPORTS_PS,
	IEEE80211_HW_PS_NULLFUNC_STACK,
	IEEE80211_HW_SUPPORTS_DYNAMIC_PS,
	IEEE80211_HW_MFP_CAPABLE,
	IEEE80211_HW_WANT_MONITOR_VIF,
	IEEE80211_HW_NO_VIRTUAL_MONITOR,
	IEEE80211_HW_NO_AUTO_VIF,
	IEEE80211_HW_SW_CRYPTO_CONTROL,
	IEEE80211_HW_SUPPORT_FAST_XMIT,
	IEEE80211_HW_REPORTS_TX_ACK_STATUS,
	IEEE80211_HW_CONNECTION_MONITOR,
	IEEE80211_HW_QUEUE_CONTROL,
	IEEE80211_HW_SUPPORTS_PER_STA_GTK,
	IEEE80211_HW_AP_LINK_PS,
	IEEE80211_HW_TX_AMPDU_SETUP_IN_HW,
	IEEE80211_HW_SUPPORTS_RC_TABLE,
	IEEE80211_HW_P2P_DEV_ADDR_FOR_INTF,
	IEEE80211_HW_TIMING_BEACON_ONLY,
	IEEE80211_HW_SUPPORTS_HT_CCK_RATES,
	IEEE80211_HW_CHANCTX_STA_CSA,
	IEEE80211_HW_SUPPORTS_CLONED_SKBS,
	IEEE80211_HW_SINGLE_SCAN_ON_ALL_BANDS,
	IEEE80211_HW_TDLS_WIDER_BW,
	IEEE80211_HW_SUPPORTS_AMSDU_IN_AMPDU,
	IEEE80211_HW_BEACON_TX_STATUS,
	IEEE80211_HW_NEEDS_UNIQUE_STA_ADDR,
	IEEE80211_HW_SUPPORTS_REORDERING_BUFFER,
	IEEE80211_HW_USES_RSS,
	IEEE80211_HW_TX_AMSDU,
	IEEE80211_HW_TX_FRAG_LIST,
	IEEE80211_HW_REPORTS_LOW_ACK,
	IEEE80211_HW_SUPPORTS_TX_FRAG,
	IEEE80211_HW_SUPPORTS_TDLS_BUFFER_STA,
	IEEE80211_HW_DOESNT_SUPPORT_QOS_NDP,
	IEEE80211_HW_BUFF_MMPDU_TXQ,
	IEEE80211_HW_SUPPORTS_VHT_EXT_NSS_BW,
	IEEE80211_HW_STA_MMPDU_TXQ,
	IEEE80211_HW_TX_STATUS_NO_AMPDU_LEN,
	IEEE80211_HW_SUPPORTS_MULTI_BSSID,
	IEEE80211_HW_SUPPORTS_ONLY_HE_MULTI_BSSID,
	IEEE80211_HW_AMPDU_KEYBORDER_SUPPORT,
	IEEE80211_HW_SUPPORTS_TX_ENCAP_OFFLOAD,
	IEEE80211_HW_SUPPORTS_RX_DECAP_OFFLOAD,
	IEEE80211_HW_SUPPORTS_CONC_MON_RX_DECAP,
	IEEE80211_HW_DETECTS_COLOR_COLLISION,
	IEEE80211_HW_MLO_MCAST_MULTI_LINK_TX,
	IEEE80211_HW_DISALLOW_PUNCTURING,
	IEEE80211_HW_HANDLES_QUIET_CSA,
	IEEE80211_HW_STRICT,
	IEEE80211_HW_SUPPORTS_NDP_BLOCKACK,
	NUM_IEEE80211_HW_FLAGS
};

struct ieee80211_hw {
	struct ieee80211_conf conf;
	struct wiphy *wiphy;
	const char *rate_control_algorithm;
	void *priv;			/* the driver's data (ieee80211_alloc_hw's size) */
	unsigned long flags[BITS_TO_LONGS(NUM_IEEE80211_HW_FLAGS)];
	unsigned int extra_tx_headroom;
	unsigned int extra_beacon_tailroom;
	int vif_data_size;		/* sizes of the drv_priv[] areas */
	int sta_data_size;
	int chanctx_data_size;
	int txq_data_size;
	u16 queues;
	u16 max_listen_interval;
	s8 max_signal;
	u8 max_rates;
	u8 max_report_rates;
	u8 max_rate_tries;
	u16 max_rx_aggregation_subframes;
	u16 max_tx_aggregation_subframes;
	u8 max_tx_fragments;
	u8 offchannel_tx_hw_queue;
	u8 radiotap_mcs_details;
	u16 radiotap_vht_details;
	struct {
		int units_pos;
		s16 accuracy;
	} radiotap_timestamp;
	netdev_features_t netdev_features;
	u8 uapsd_queues;
	u8 uapsd_max_sp_len;
	u8 max_nan_de_entries;
	u8 tx_sk_pacing_shift;
	u8 weight_multiplier;
	u32 max_mtu;
	const s8 *tx_power_levels;
	u8 max_txpwr_levels_idx;
};

static inline bool _ieee80211_hw_check(struct ieee80211_hw *hw, enum ieee80211_hw_flags flg)
{
	return test_bit(flg, hw->flags);
}
#define ieee80211_hw_check(hw, flg)	_ieee80211_hw_check(hw, IEEE80211_HW_##flg)

static inline void _ieee80211_hw_set(struct ieee80211_hw *hw, enum ieee80211_hw_flags flg)
{
	__set_bit(flg, hw->flags);
}
#define ieee80211_hw_set(hw, flg)	_ieee80211_hw_set(hw, IEEE80211_HW_##flg)

struct ieee80211_hw *wiphy_to_ieee80211_hw(struct wiphy *wiphy);

static inline void SET_IEEE80211_DEV(struct ieee80211_hw *hw, struct device *dev)
{
	hw->wiphy->dev.parent = dev;
}
static inline void SET_IEEE80211_PERM_ADDR(struct ieee80211_hw *hw, const u8 *addr)
{
	memcpy(hw->wiphy->perm_addr, addr, ETH_ALEN);
}

/* --- callback arguments ------------------------------------------------------------------------- */

struct ieee80211_scan_ies {
	const u8 *ies[NUM_NL80211_BANDS];	/* per-band elements for probe requests */
	size_t len[NUM_NL80211_BANDS];
	const u8 *common_ies;
	size_t common_ie_len;
};

struct ieee80211_scan_request {
	struct ieee80211_scan_ies ies;
	struct cfg80211_scan_request req;	/* last: it ends in a flexible array */
};

struct ieee80211_tdls_ch_sw_params {
	struct ieee80211_sta *sta;
	struct cfg80211_chan_def *chandef;
	u8 action_code;
	u32 status;
	u32 timestamp;
	u16 switch_time;
	u16 switch_timeout;
	struct sk_buff *tmpl_skb;
	u32 ch_sw_tm_ie;
};

/* Receive filter: which frames beyond our own the driver should pass up. */
enum ieee80211_filter_flags {
	FIF_ALLMULTI		= 1 << 1,
	FIF_FCSFAIL		= 1 << 2,
	FIF_PLCPFAIL		= 1 << 3,
	FIF_BCN_PRBRESP_PROMISC	= 1 << 4,
	FIF_CONTROL		= 1 << 5,
	FIF_OTHER_BSS		= 1 << 6,
	FIF_PSPOLL		= 1 << 7,
	FIF_PROBE_REQ		= 1 << 8,
	FIF_MCAST_ACTION	= 1 << 9,
};

/* Block acknowledgement sessions (A-MPDU aggregation), per peer and TID. */
enum ieee80211_ampdu_mlme_action {
	IEEE80211_AMPDU_RX_START,
	IEEE80211_AMPDU_RX_STOP,
	IEEE80211_AMPDU_TX_START,
	IEEE80211_AMPDU_TX_STOP_CONT,
	IEEE80211_AMPDU_TX_STOP_FLUSH,
	IEEE80211_AMPDU_TX_STOP_FLUSH_CONT,
	IEEE80211_AMPDU_TX_OPERATIONAL,
};
#define IEEE80211_AMPDU_TX_START_IMMEDIATE	1
#define IEEE80211_AMPDU_TX_START_DELAY_ADDBA	2

struct ieee80211_ampdu_params {
	enum ieee80211_ampdu_mlme_action action;
	struct ieee80211_sta *sta;
	u16 tid;
	u16 ssn;			/* starting sequence number */
	u16 buf_size;
	bool amsdu;
	u16 timeout;
};

enum ieee80211_frame_release_type { IEEE80211_FRAME_RELEASE_PSPOLL, IEEE80211_FRAME_RELEASE_UAPSD };

enum ieee80211_rate_control_changed {
	IEEE80211_RC_BW_CHANGED		= BIT(0),
	IEEE80211_RC_SMPS_CHANGED	= BIT(1),
	IEEE80211_RC_SUPP_RATES_CHANGED	= BIT(2),
	IEEE80211_RC_NSS_CHANGED	= BIT(3),
};

enum ieee80211_roc_type { IEEE80211_ROC_TYPE_NORMAL = 0, IEEE80211_ROC_TYPE_MGMT_TX };
enum ieee80211_reconfig_type { IEEE80211_RECONFIG_TYPE_RESTART, IEEE80211_RECONFIG_TYPE_SUSPEND };

/* Before/after the core sends authentication or association frames: the
   driver keeps the radio on the channel meanwhile. */
struct ieee80211_prep_tx_info {
	u16 duration;
	u16 subtype;
	u8 success:1, was_assoc:1;
	int link_id;
};

struct inet6_dev;

/* The driver's callbacks. Ones a driver leaves NULL the core does
   without (or reports as not supported). */
struct ieee80211_ops {
	void (*tx)(struct ieee80211_hw *hw, struct ieee80211_tx_control *control, struct sk_buff *skb);
	int (*start)(struct ieee80211_hw *hw);
	void (*stop)(struct ieee80211_hw *hw, bool suspend);
	int (*suspend)(struct ieee80211_hw *hw, struct cfg80211_wowlan *wowlan);
	int (*resume)(struct ieee80211_hw *hw);
	void (*set_wakeup)(struct ieee80211_hw *hw, bool enabled);
	int (*add_interface)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	void (*remove_interface)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	int (*config)(struct ieee80211_hw *hw, int radio_idx, u32 changed);
	void (*bss_info_changed)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				 struct ieee80211_bss_conf *info, u64 changed);
	void (*vif_cfg_changed)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, u64 changed);
	void (*link_info_changed)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				  struct ieee80211_bss_conf *info, u64 changed);
	int (*start_ap)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			struct ieee80211_bss_conf *link_conf);
	void (*stop_ap)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			struct ieee80211_bss_conf *link_conf);
	u64 (*prepare_multicast)(struct ieee80211_hw *hw, struct netdev_hw_addr_list *mc_list);
	void (*configure_filter)(struct ieee80211_hw *hw, unsigned int changed_flags,
				 unsigned int *total_flags, u64 multicast);
	void (*config_iface_filter)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				    unsigned int filter_flags, unsigned int changed_flags);
	int (*set_tim)(struct ieee80211_hw *hw, struct ieee80211_sta *sta, bool set);
	int (*set_key)(struct ieee80211_hw *hw, enum set_key_cmd cmd, struct ieee80211_vif *vif,
		       struct ieee80211_sta *sta, struct ieee80211_key_conf *key);
	void (*update_tkip_key)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				struct ieee80211_key_conf *conf, struct ieee80211_sta *sta,
				u32 iv32, u16 *phase1key);
	void (*set_rekey_data)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			       struct cfg80211_gtk_rekey_data *data);
	void (*set_default_unicast_key)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, int idx);
	int (*hw_scan)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		       struct ieee80211_scan_request *req);
	void (*cancel_hw_scan)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	int (*sched_scan_start)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				struct cfg80211_sched_scan_request *req, struct ieee80211_scan_ies *ies);
	int (*sched_scan_stop)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	int (*set_rts_threshold)(struct ieee80211_hw *hw, int radio_idx, u32 value);
	void (*vif_add_debugfs)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	void (*link_add_debugfs)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				 struct ieee80211_bss_conf *link_conf, struct dentry *dir);
	void (*link_sta_add_debugfs)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				     struct ieee80211_link_sta *link_sta, struct dentry *dir);
	void (*sta_notify)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, enum sta_notify_cmd,
			   struct ieee80211_sta *sta);
	int (*sta_state)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_sta *sta,
			 enum ieee80211_sta_state old_state, enum ieee80211_sta_state new_state);
	void (*sta_pre_rcu_remove)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				   struct ieee80211_sta *sta);
	void (*link_sta_rc_update)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				   struct ieee80211_link_sta *link_sta, u32 changed);
	void (*sta_statistics)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			       struct ieee80211_sta *sta, struct station_info *sinfo);
	int (*conf_tx)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, unsigned int link_id,
		       u16 ac, const struct ieee80211_tx_queue_params *params);
	int (*tx_last_beacon)(struct ieee80211_hw *hw);
	int (*ampdu_action)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			    struct ieee80211_ampdu_params *params);
	int (*get_survey)(struct ieee80211_hw *hw, int idx, struct survey_info *survey);
	void (*flush)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, u32 queues, bool drop);
	void (*flush_sta)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct ieee80211_sta *sta);
	void (*channel_switch)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			       struct ieee80211_channel_switch *ch_switch);
	int (*set_antenna)(struct ieee80211_hw *hw, int radio_idx, u32 tx_ant, u32 rx_ant);
	int (*get_antenna)(struct ieee80211_hw *hw, int radio_idx, u32 *tx_ant, u32 *rx_ant);
	int (*remain_on_channel)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				 struct ieee80211_channel *chan, int duration, enum ieee80211_roc_type type);
	int (*cancel_remain_on_channel)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	void (*event_callback)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			       const struct ieee80211_event *event);
	void (*allow_buffered_frames)(struct ieee80211_hw *hw, struct ieee80211_sta *sta, u16 tids,
				      int num_frames, enum ieee80211_frame_release_type reason,
				      bool more_data);
	void (*release_buffered_frames)(struct ieee80211_hw *hw, struct ieee80211_sta *sta, u16 tids,
					int num_frames, enum ieee80211_frame_release_type reason,
					bool more_data);
	void (*mgd_prepare_tx)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			       struct ieee80211_prep_tx_info *info);
	void (*mgd_complete_tx)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				struct ieee80211_prep_tx_info *info);
	void (*mgd_protect_tdls_discover)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
					  unsigned int link_id);
	int (*add_chanctx)(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx);
	void (*remove_chanctx)(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx);
	void (*change_chanctx)(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx, u32 changed);
	int (*assign_vif_chanctx)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				  struct ieee80211_bss_conf *link_conf, struct ieee80211_chanctx_conf *ctx);
	void (*unassign_vif_chanctx)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				     struct ieee80211_bss_conf *link_conf, struct ieee80211_chanctx_conf *ctx);
	int (*switch_vif_chanctx)(struct ieee80211_hw *hw, struct ieee80211_vif_chanctx_switch *vifs,
				  int n_vifs, enum ieee80211_chanctx_switch_mode mode);
	void (*reconfig_complete)(struct ieee80211_hw *hw, enum ieee80211_reconfig_type reconfig_type);
	void (*ipv6_addr_change)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, struct inet6_dev *idev);
	int (*pre_channel_switch)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				  struct ieee80211_channel_switch *ch_switch);
	int (*post_channel_switch)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				   struct ieee80211_bss_conf *link_conf);
	void (*abort_channel_switch)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				     struct ieee80211_bss_conf *link_conf);
	void (*channel_switch_rx_beacon)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
					 struct ieee80211_channel_switch *ch_switch);
	int (*join_ibss)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	void (*leave_ibss)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
	int (*tdls_channel_switch)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				   struct ieee80211_sta *sta, u8 oper_class,
				   struct cfg80211_chan_def *chandef, struct sk_buff *tmpl_skb,
				   u32 ch_sw_tm_ie);
	void (*tdls_cancel_channel_switch)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
					   struct ieee80211_sta *sta);
	void (*tdls_recv_channel_switch)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
					 struct ieee80211_tdls_ch_sw_params *params);
	void (*wake_tx_queue)(struct ieee80211_hw *hw, struct ieee80211_txq *txq);
	void (*sync_rx_queues)(struct ieee80211_hw *hw);
	bool (*can_aggregate_in_amsdu)(struct ieee80211_hw *hw, struct sk_buff *head, struct sk_buff *skb);
	int (*get_ftm_responder_stats)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				       struct cfg80211_ftm_responder_stats *ftm_stats);
	int (*start_pmsr)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  struct cfg80211_pmsr_request *request);
	void (*abort_pmsr)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			   struct cfg80211_pmsr_request *request);
	int (*set_hw_timestamp)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				struct cfg80211_set_hw_timestamp *hwts);
	bool (*can_activate_links)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, u16 active_links);
	int (*change_vif_links)(struct ieee80211_hw *hw, struct ieee80211_vif *vif, u16 old_links,
				u16 new_links, struct ieee80211_bss_conf *old[IEEE80211_MLD_MAX_NUM_LINKS]);
	int (*change_sta_links)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				struct ieee80211_sta *sta, u16 old_links, u16 new_links);
};

/* --- what the core offers the driver --------------------------------------------------------------- */

/* Life cycle. alloc gives a hw with priv_data_len bytes for the driver. */
struct ieee80211_hw *ieee80211_alloc_hw_nm(size_t priv_data_len, const struct ieee80211_ops *ops,
					   const char *requested_name);
static inline struct ieee80211_hw *ieee80211_alloc_hw(size_t priv_data_len, const struct ieee80211_ops *ops)
{
	return ieee80211_alloc_hw_nm(priv_data_len, ops, NULL);
}
int ieee80211_register_hw(struct ieee80211_hw *hw);
void ieee80211_unregister_hw(struct ieee80211_hw *hw);
void ieee80211_free_hw(struct ieee80211_hw *hw);
void ieee80211_restart_hw(struct ieee80211_hw *hw);	/* after a firmware crash */

/* Receiving. The skb's cb holds the ieee80211_rx_status. */
void ieee80211_rx_napi(struct ieee80211_hw *hw, struct ieee80211_sta *sta, struct sk_buff *skb,
		       struct napi_struct *napi);
void ieee80211_rx_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb);

/* Sending: frames waiting in a txq, and their status once sent. */
struct sk_buff *ieee80211_tx_dequeue(struct ieee80211_hw *hw, struct ieee80211_txq *txq);
void ieee80211_tx_status_skb(struct ieee80211_hw *hw, struct sk_buff *skb);
void ieee80211_tx_status_ext(struct ieee80211_hw *hw, struct ieee80211_tx_status *status);
void ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb);
void ieee80211_stop_queues(struct ieee80211_hw *hw);
void ieee80211_wake_queues(struct ieee80211_hw *hw);

/* Converting a received 802.11 data frame to Ethernet. */
int ieee80211_data_to_8023(struct sk_buff *skb, const u8 *addr, enum nl80211_iftype iftype);

/* Scanning, remain-on-channel. */
void ieee80211_scan_completed(struct ieee80211_hw *hw, struct cfg80211_scan_info *info);
void ieee80211_sched_scan_results(struct ieee80211_hw *hw);
void ieee80211_sched_scan_stopped(struct ieee80211_hw *hw);
void ieee80211_ready_on_channel(struct ieee80211_hw *hw);
void ieee80211_remain_on_channel_expired(struct ieee80211_hw *hw);

/* Walking interfaces, stations, keys. */
enum ieee80211_interface_iteration_flags {
	IEEE80211_IFACE_ITER_NORMAL		= 0,
	IEEE80211_IFACE_ITER_RESUME_ALL		= BIT(0),
	IEEE80211_IFACE_ITER_ACTIVE		= BIT(1),
	IEEE80211_IFACE_SKIP_SDATA_NOT_IN_DRIVER = BIT(2),
};
void ieee80211_iterate_interfaces(struct ieee80211_hw *hw, u32 iter_flags,
				  void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif),
				  void *data);
static inline void ieee80211_iterate_active_interfaces(struct ieee80211_hw *hw, u32 iter_flags,
		void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif), void *data)
{
	ieee80211_iterate_interfaces(hw, iter_flags | IEEE80211_IFACE_ITER_ACTIVE, iterator, data);
}
void ieee80211_iterate_active_interfaces_atomic(struct ieee80211_hw *hw, u32 iter_flags,
		void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif), void *data);
void ieee80211_iterate_stations_atomic(struct ieee80211_hw *hw,
				       void (*iterator)(void *data, struct ieee80211_sta *sta),
				       void *data);
struct ieee80211_sta *ieee80211_find_sta(struct ieee80211_vif *vif, const u8 *addr);
struct ieee80211_sta *ieee80211_find_sta_by_ifaddr(struct ieee80211_hw *hw, const u8 *addr,
						   const u8 *localaddr);
void ieee80211_iter_keys(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			 void (*iter)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				      struct ieee80211_sta *sta, struct ieee80211_key_conf *key, void *data),
			 void *iter_data);
void ieee80211_iter_keys_rcu(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			     void (*iter)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
					  struct ieee80211_sta *sta, struct ieee80211_key_conf *key,
					  void *data),
			     void *iter_data);

/* Keys: replay counters, TKIP's per-packet keys, errors seen. */
void ieee80211_get_key_rx_seq(struct ieee80211_key_conf *keyconf, int tid, struct ieee80211_key_seq *seq);
void ieee80211_get_tkip_rx_p1k(struct ieee80211_key_conf *keyconf, const u8 *ta, u32 iv32, u16 *p1k);
void ieee80211_get_tkip_p2k(struct ieee80211_key_conf *keyconf, struct sk_buff *skb, u8 *p2k);
u8 *ieee80211_tkip_add_iv(u8 *pos, struct ieee80211_key_conf *keyconf, u64 pn);
void ieee80211_key_mic_failure(struct ieee80211_key_conf *keyconf);
void ieee80211_key_replay(struct ieee80211_key_conf *keyconf);

/* Power save of peers (we as AP). */
int ieee80211_sta_ps_transition(struct ieee80211_sta *sta, bool start);
void ieee80211_sta_pspoll(struct ieee80211_sta *sta);
void ieee80211_sta_uapsd_trigger(struct ieee80211_sta *sta, u8 tid);
void ieee80211_sta_set_buffered(struct ieee80211_sta *sta, u8 tid, bool buffered);
void ieee80211_sta_block_awake(struct ieee80211_hw *hw, struct ieee80211_sta *pubsta, bool block);
void ieee80211_sta_eosp(struct ieee80211_sta *pubsta);
void ieee80211_send_eosp_nullfunc(struct ieee80211_sta *pubsta, int tid);
void ieee80211_sta_recalc_aggregates(struct ieee80211_sta *pubsta);

/* Block-ack sessions. */
void ieee80211_start_tx_ba_cb_irqsafe(struct ieee80211_vif *vif, const u8 *ra, u16 tid);
void ieee80211_stop_tx_ba_cb_irqsafe(struct ieee80211_vif *vif, const u8 *ra, u16 tid);
void ieee80211_mark_rx_ba_filtered_frames(struct ieee80211_sta *pubsta, u8 tid, u16 ssn,
					  u64 filtered, u16 received_mpdus);
void ieee80211_rx_ba_timer_expired(struct ieee80211_vif *vif, const u8 *addr, unsigned int tid);
int ieee80211_reserve_tid(struct ieee80211_sta *sta, u8 tid);
void ieee80211_unreserve_tid(struct ieee80211_sta *sta, u8 tid);

/* The connection: losing it, signal reports, channel switches. */
void ieee80211_beacon_loss(struct ieee80211_vif *vif);
void ieee80211_connection_loss(struct ieee80211_vif *vif);
void ieee80211_disconnect(struct ieee80211_vif *vif, bool reconnect);
void ieee80211_hw_restart_disconnect(struct ieee80211_vif *vif);
void ieee80211_cqm_rssi_notify(struct ieee80211_vif *vif,
			       enum nl80211_cqm_rssi_threshold_event rssi_event, s32 rssi_level, gfp_t gfp);
void ieee80211_cqm_beacon_loss_notify(struct ieee80211_vif *vif, gfp_t gfp);
void ieee80211_chswitch_done(struct ieee80211_vif *vif, bool success, unsigned int link_id);
void ieee80211_channel_switch_disconnect(struct ieee80211_vif *vif);
void ieee80211_request_smps(struct ieee80211_vif *vif, unsigned int link_id,
			    enum ieee80211_smps_mode smps_mode);
void ieee80211_update_mu_groups(struct ieee80211_vif *vif, unsigned int link_id,
				const u8 *membership, const u8 *position);
void ieee80211_tdls_oper_request(struct ieee80211_vif *vif, const u8 *peer,
				 enum nl80211_tdls_operation oper, u16 reason_code, gfp_t gfp);

/* Beacons (we as AP). */
struct ieee80211_mutable_offsets {
	u16 tim_offset;
	u16 tim_length;
	u16 cntdwn_counter_offs[2];
	u16 mbssid_off;
};
struct sk_buff *ieee80211_beacon_get_template(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
					      struct ieee80211_mutable_offsets *offs,
					      unsigned int link_id);
u8 ieee80211_beacon_update_cntdwn(struct ieee80211_vif *vif, unsigned int link_id);
void ieee80211_beacon_set_cntdwn(struct ieee80211_vif *vif, u8 counter);
void ieee80211_csa_finish(struct ieee80211_vif *vif, unsigned int link_id);
bool ieee80211_beacon_cntdwn_is_complete(struct ieee80211_vif *vif, unsigned int link_id);

#endif
