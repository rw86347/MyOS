/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LINUX_IEEE80211_H
#define LINUX_IEEE80211_H

/*
 * linux/ieee80211.h — the 802.11 (Wi-Fi) frame formats and numbers, as
 * IEEE 802.11-2020 (and its HE/EHT amendments, Wi-Fi 6 and 7) defines
 * them, under the names Linux drivers use. MyOS's own file: written for
 * the 802.11 core and the drivers it carries (docs/WIFI-PLAN.md), with
 * only what those use. The numbers are the standard's.
 *
 * A Wi-Fi frame starts with a header (struct ieee80211_hdr):
 *
 *   frame_control  2   what kind of frame (below), to/from the AP, flags
 *   duration       2   how long the medium is reserved, microseconds
 *   addr1          6   receiver
 *   addr2          6   transmitter
 *   addr3          6   the third party: the BSSID, or the final destination
 *   seq_ctrl       2   sequence number (12 bits) and fragment number (4)
 *   addr4          6   only between two APs (both to- and from-DS set)
 *   qos_ctrl       2   only in QoS data frames: the priority (TID)
 *
 * All multi-byte fields are little-endian (the __le16s below).
 */
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/unaligned.h>

/* --- frame control -------------------------------------------------------- */

#define IEEE80211_FCTL_VERS		0x0003
#define IEEE80211_FCTL_FTYPE		0x000c	/* type: management, control, data */
#define IEEE80211_FCTL_STYPE		0x00f0	/* subtype */
#define IEEE80211_FCTL_TODS		0x0100
#define IEEE80211_FCTL_FROMDS		0x0200
#define IEEE80211_FCTL_MOREFRAGS	0x0400
#define IEEE80211_FCTL_RETRY		0x0800
#define IEEE80211_FCTL_PM		0x1000	/* sender goes to sleep after this */
#define IEEE80211_FCTL_MOREDATA		0x2000
#define IEEE80211_FCTL_PROTECTED	0x4000	/* encrypted */
#define IEEE80211_FCTL_ORDER		0x8000	/* (HT/HE: an HT Control field follows) */
#define IEEE80211_FCTL_CTL_EXT		0x0f00

#define IEEE80211_FTYPE_MGMT		0x0000
#define IEEE80211_FTYPE_CTL		0x0004
#define IEEE80211_FTYPE_DATA		0x0008
#define IEEE80211_FTYPE_EXT		0x000c

/* management subtypes */
#define IEEE80211_STYPE_ASSOC_REQ	0x0000
#define IEEE80211_STYPE_ASSOC_RESP	0x0010
#define IEEE80211_STYPE_REASSOC_REQ	0x0020
#define IEEE80211_STYPE_REASSOC_RESP	0x0030
#define IEEE80211_STYPE_PROBE_REQ	0x0040
#define IEEE80211_STYPE_PROBE_RESP	0x0050
#define IEEE80211_STYPE_BEACON		0x0080
#define IEEE80211_STYPE_ATIM		0x0090
#define IEEE80211_STYPE_DISASSOC	0x00a0
#define IEEE80211_STYPE_AUTH		0x00b0
#define IEEE80211_STYPE_DEAUTH		0x00c0
#define IEEE80211_STYPE_ACTION		0x00d0

/* control subtypes */
#define IEEE80211_STYPE_TRIGGER		0x0020
#define IEEE80211_STYPE_BACK_REQ	0x0080
#define IEEE80211_STYPE_BACK		0x0090
#define IEEE80211_STYPE_PSPOLL		0x00a0
#define IEEE80211_STYPE_RTS		0x00b0
#define IEEE80211_STYPE_CTS		0x00c0
#define IEEE80211_STYPE_ACK		0x00d0

/* data subtypes: bit 0x40 = no data ("null"), bit 0x80 = QoS */
#define IEEE80211_STYPE_DATA		0x0000
#define IEEE80211_STYPE_NULLFUNC	0x0040
#define IEEE80211_STYPE_QOS_DATA	0x0080
#define IEEE80211_STYPE_QOS_NULLFUNC	0x00c0

/* sequence control */
#define IEEE80211_SCTL_FRAG		0x000f
#define IEEE80211_SCTL_SEQ		0xfff0
#define IEEE80211_SN_MASK		(IEEE80211_SCTL_SEQ >> 4)
#define IEEE80211_MAX_SN		IEEE80211_SN_MASK
#define IEEE80211_SN_MODULO		(IEEE80211_MAX_SN + 1)
#define IEEE80211_SEQ_TO_SN(seq)	(((seq) & IEEE80211_SCTL_SEQ) >> 4)
#define IEEE80211_SN_TO_SEQ(sn)		(((sn) << 4) & IEEE80211_SCTL_SEQ)

/* QoS control */
#define IEEE80211_QOS_CTL_TID_MASK		0x000f
#define IEEE80211_QOS_CTL_TAG1D_MASK		0x0007
#define IEEE80211_QOS_CTL_EOSP			0x0010
#define IEEE80211_QOS_CTL_ACK_POLICY_MASK	0x0060
#define IEEE80211_QOS_CTL_ACK_POLICY_NOACK	0x0020
#define IEEE80211_QOS_CTL_A_MSDU_PRESENT	0x0080
#define IEEE80211_QOS_CTL_LEN			2
#define IEEE80211_HT_CTL_LEN			4	/* after QoS control when +HTC (ORDER) is set */

/* sizes */
#define FCS_LEN				4	/* the CRC at the end of every frame */
#define IEEE80211_MAX_SSID_LEN		32
#define IEEE80211_MAX_DATA_LEN		2304
#define IEEE80211_MAX_FRAME_LEN		2352
#define IEEE80211_MAX_RTS_THRESHOLD	2353
#define IEEE80211_NUM_TIDS		16	/* traffic ids: 8 priorities (+8 for TSPECs) */
#define IEEE80211_NUM_ACS		4	/* access categories: voice, video, best effort, background */
#define IEEE80211_MAX_CHAINS		4	/* antennas a device reports separately */
#define IEEE80211_HT_MCS_MASK_LEN	10
#define IEEE80211_MLD_MAX_NUM_LINKS	15	/* Wi-Fi 7: links one connection may use */

/* the security headers and trailers each cipher adds */
#define IEEE80211_WEP_IV_LEN		4
#define IEEE80211_WEP_ICV_LEN		4
#define IEEE80211_TKIP_IV_LEN		8
#define IEEE80211_TKIP_ICV_LEN		4
#define IEEE80211_CCMP_HDR_LEN		8
#define IEEE80211_CCMP_MIC_LEN		8
#define IEEE80211_CCMP_PN_LEN		6
#define IEEE80211_GCMP_HDR_LEN		8
#define IEEE80211_GCMP_PN_LEN		6

/* power: mBm = 100 * dBm; time: one TU ("time unit") = 1024 us */
#define DBM_TO_MBM(gain)		((gain) * 100)
#define MBM_TO_DBM(gain)		((gain) / 100)
#define DBI_TO_MBI(gain)		((gain) * 100)
#define MHZ_TO_KHZ(freq)		((freq) * 1000)
#define KHZ_TO_MHZ(freq)		((freq) / 1000)
#define TU_TO_JIFFIES(x)		(usecs_to_jiffies((x) * 1024))
#define TU_TO_EXP_TIME(x)		(jiffies + TU_TO_JIFFIES(x))

static inline u64 ieee80211_tu_to_usec(u64 tu) { return tu * 1024; }

/* --- the header ------------------------------------------------------------- */

struct ieee80211_hdr {
	__le16 frame_control;
	__le16 duration_id;
	u8 addr1[ETH_ALEN];
	u8 addr2[ETH_ALEN];
	u8 addr3[ETH_ALEN];
	__le16 seq_ctrl;
	u8 addr4[ETH_ALEN];
} __packed __aligned(2);

struct ieee80211_hdr_3addr {
	__le16 frame_control;
	__le16 duration_id;
	u8 addr1[ETH_ALEN];
	u8 addr2[ETH_ALEN];
	u8 addr3[ETH_ALEN];
	__le16 seq_ctrl;
} __packed __aligned(2);

struct ieee80211_qos_hdr {
	__le16 frame_control;
	__le16 duration_id;
	u8 addr1[ETH_ALEN];
	u8 addr2[ETH_ALEN];
	u8 addr3[ETH_ALEN];
	__le16 seq_ctrl;
	__le16 qos_ctrl;
} __packed __aligned(2);

/* The frame-control tests. fc stays little-endian: comparing it against a
   constant converted once (at compile time) is cheaper than converting fc. */
static inline bool ieee80211_has_tods(__le16 fc)	{ return fc & cpu_to_le16(IEEE80211_FCTL_TODS); }
static inline bool ieee80211_has_fromds(__le16 fc)	{ return fc & cpu_to_le16(IEEE80211_FCTL_FROMDS); }
static inline bool ieee80211_has_a4(__le16 fc)
{
	__le16 both = cpu_to_le16(IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS);
	return (fc & both) == both;
}
static inline bool ieee80211_has_morefrags(__le16 fc)	{ return fc & cpu_to_le16(IEEE80211_FCTL_MOREFRAGS); }
static inline bool ieee80211_has_retry(__le16 fc)	{ return fc & cpu_to_le16(IEEE80211_FCTL_RETRY); }
static inline bool ieee80211_has_pm(__le16 fc)		{ return fc & cpu_to_le16(IEEE80211_FCTL_PM); }
static inline bool ieee80211_has_moredata(__le16 fc)	{ return fc & cpu_to_le16(IEEE80211_FCTL_MOREDATA); }
static inline bool ieee80211_has_protected(__le16 fc)	{ return fc & cpu_to_le16(IEEE80211_FCTL_PROTECTED); }
static inline bool ieee80211_has_order(__le16 fc)	{ return fc & cpu_to_le16(IEEE80211_FCTL_ORDER); }

#define __IEEE80211_FTYPE(fc)		((fc) & cpu_to_le16(IEEE80211_FCTL_FTYPE))
#define __IEEE80211_KIND(fc)		((fc) & cpu_to_le16(IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE))
#define __IEEE80211_IS(fc, t, s)	(__IEEE80211_KIND(fc) == cpu_to_le16((t) | (s)))

static inline bool ieee80211_is_mgmt(__le16 fc)	{ return __IEEE80211_FTYPE(fc) == cpu_to_le16(IEEE80211_FTYPE_MGMT); }
static inline bool ieee80211_is_ctl(__le16 fc)	{ return __IEEE80211_FTYPE(fc) == cpu_to_le16(IEEE80211_FTYPE_CTL); }
static inline bool ieee80211_is_data(__le16 fc)	{ return __IEEE80211_FTYPE(fc) == cpu_to_le16(IEEE80211_FTYPE_DATA); }
static inline bool ieee80211_is_ext(__le16 fc)	{ return __IEEE80211_FTYPE(fc) == cpu_to_le16(IEEE80211_FTYPE_EXT); }

/* data with the QoS bit (0x80 of the subtype) */
static inline bool ieee80211_is_data_qos(__le16 fc)
{
	return (fc & cpu_to_le16(IEEE80211_FCTL_FTYPE | IEEE80211_STYPE_QOS_DATA)) ==
	       cpu_to_le16(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_QOS_DATA);
}
/* data that carries data (the "null" bit 0x40 clear) */
static inline bool ieee80211_is_data_present(__le16 fc)
{
	return (fc & cpu_to_le16(IEEE80211_FCTL_FTYPE | IEEE80211_STYPE_NULLFUNC)) ==
	       cpu_to_le16(IEEE80211_FTYPE_DATA);
}

#define IEEE80211_IS_MGMT(name, st) \
	static inline bool ieee80211_is_##name(__le16 fc) { return __IEEE80211_IS(fc, IEEE80211_FTYPE_MGMT, st); }
IEEE80211_IS_MGMT(assoc_req, IEEE80211_STYPE_ASSOC_REQ)
IEEE80211_IS_MGMT(assoc_resp, IEEE80211_STYPE_ASSOC_RESP)
IEEE80211_IS_MGMT(reassoc_req, IEEE80211_STYPE_REASSOC_REQ)
IEEE80211_IS_MGMT(reassoc_resp, IEEE80211_STYPE_REASSOC_RESP)
IEEE80211_IS_MGMT(probe_req, IEEE80211_STYPE_PROBE_REQ)
IEEE80211_IS_MGMT(probe_resp, IEEE80211_STYPE_PROBE_RESP)
IEEE80211_IS_MGMT(beacon, IEEE80211_STYPE_BEACON)
IEEE80211_IS_MGMT(disassoc, IEEE80211_STYPE_DISASSOC)
IEEE80211_IS_MGMT(auth, IEEE80211_STYPE_AUTH)
IEEE80211_IS_MGMT(deauth, IEEE80211_STYPE_DEAUTH)
IEEE80211_IS_MGMT(action, IEEE80211_STYPE_ACTION)
#undef IEEE80211_IS_MGMT

static inline bool ieee80211_is_back_req(__le16 fc)	{ return __IEEE80211_IS(fc, IEEE80211_FTYPE_CTL, IEEE80211_STYPE_BACK_REQ); }
static inline bool ieee80211_is_back(__le16 fc)		{ return __IEEE80211_IS(fc, IEEE80211_FTYPE_CTL, IEEE80211_STYPE_BACK); }
static inline bool ieee80211_is_pspoll(__le16 fc)	{ return __IEEE80211_IS(fc, IEEE80211_FTYPE_CTL, IEEE80211_STYPE_PSPOLL); }
static inline bool ieee80211_is_nullfunc(__le16 fc)	{ return __IEEE80211_IS(fc, IEEE80211_FTYPE_DATA, IEEE80211_STYPE_NULLFUNC); }
static inline bool ieee80211_is_qos_nullfunc(__le16 fc)	{ return __IEEE80211_IS(fc, IEEE80211_FTYPE_DATA, IEEE80211_STYPE_QOS_NULLFUNC); }
static inline bool ieee80211_is_any_nullfunc(__le16 fc)	{ return ieee80211_is_nullfunc(fc) || ieee80211_is_qos_nullfunc(fc); }

/* The QoS control field: after addr3/seq_ctrl, or after addr4. */
static inline u8 *ieee80211_get_qos_ctl(struct ieee80211_hdr *hdr)
{
	return (u8 *)hdr + (ieee80211_has_a4(hdr->frame_control) ? 30 : 24);
}

static inline u8 ieee80211_get_tid(struct ieee80211_hdr *hdr)
{
	return ieee80211_get_qos_ctl(hdr)[0] & IEEE80211_QOS_CTL_TID_MASK;
}

/* Who the frame is for / from: which address that is depends on the
   to-DS and from-DS bits. */
static inline u8 *ieee80211_get_SA(struct ieee80211_hdr *hdr)
{
	if (ieee80211_has_a4(hdr->frame_control))
		return hdr->addr4;
	return ieee80211_has_fromds(hdr->frame_control) ? hdr->addr3 : hdr->addr2;
}

static inline u8 *ieee80211_get_DA(struct ieee80211_hdr *hdr)
{
	return ieee80211_has_tods(hdr->frame_control) ? hdr->addr3 : hdr->addr1;
}

/* Sequence numbers are 12 bits and wrap: a is "less" than b if it is
   less than half the space behind it. */
static inline bool ieee80211_sn_less(u16 a, u16 b)
{
	return ((a - b) & IEEE80211_SN_MASK) > (IEEE80211_SN_MODULO >> 1);
}
static inline u16 ieee80211_sn_add(u16 a, u16 b)	{ return (a + b) & IEEE80211_SN_MASK; }
static inline u16 ieee80211_sn_inc(u16 a)		{ return ieee80211_sn_add(a, 1); }
static inline u16 ieee80211_sn_sub(u16 a, u16 b)	{ return (a - b) & IEEE80211_SN_MASK; }

/* --- management frames -------------------------------------------------------- */

#define WLAN_SA_QUERY_TR_ID_LEN		2
#define WLAN_MEMBERSHIP_LEN		8
#define WLAN_USER_POSITION_LEN		16

/* The body of each management frame: fixed fields, then elements
   ("variable"). Only the frame kinds our code reads are spelled out. */
struct ieee80211_mgmt {
	__le16 frame_control;
	__le16 duration;
	u8 da[ETH_ALEN];
	u8 sa[ETH_ALEN];
	u8 bssid[ETH_ALEN];
	__le16 seq_ctrl;
	union {
		struct {
			__le16 auth_alg;
			__le16 auth_transaction;
			__le16 status_code;
			u8 variable[];
		} __packed auth;
		struct {
			__le16 reason_code;
		} __packed deauth;
		struct {
			__le16 capab_info;
			__le16 listen_interval;
			u8 variable[];
		} __packed assoc_req;
		struct {
			__le16 capab_info;
			__le16 status_code;
			__le16 aid;
			u8 variable[];
		} __packed assoc_resp, reassoc_resp;
		struct {
			__le16 capab_info;
			__le16 listen_interval;
			u8 current_ap[ETH_ALEN];
			u8 variable[];
		} __packed reassoc_req;
		struct {
			__le16 reason_code;
		} __packed disassoc;
		struct {
			__le64 timestamp;
			__le16 beacon_int;
			__le16 capab_info;
			u8 variable[];
		} __packed beacon;
		struct {
			DECLARE_FLEX_ARRAY(u8, variable);
		} __packed probe_req;
		struct {
			__le64 timestamp;
			__le16 beacon_int;
			__le16 capab_info;
			u8 variable[];
		} __packed probe_resp;
		struct {
			u8 category;
			u8 action_code;
			union {
				struct {
					u8 dialog_token;
					u8 follow_up;
					u8 tod[6];
					u8 toa[6];
					__le16 tod_error;
					__le16 toa_error;
					u8 variable[];
				} __packed ftm;		/* fine timing measurement */
				struct {
					u8 dialog_token;
					u8 follow_up;
					u32 tod;
					u32 toa;
					u8 max_tod_error;
					u8 max_toa_error;
				} __packed wnm_timing_msr;
				struct {
					u8 dialog_token;
					u8 tpc_elem_id;
					u8 tpc_elem_length;
					struct ieee80211_tpc_report_ie {
						u8 tx_power;
						u8 link_margin;
					} __packed tpc;
				} __packed tpc_report;
				struct {
					u8 membership[WLAN_MEMBERSHIP_LEN];
					u8 position[WLAN_USER_POSITION_LEN];
				} __packed vht_group_notif;
				struct {
					u8 dialog_token;
					u8 variable[];
				} __packed generic;
			};
		} __packed action;
	} u;
} __packed __aligned(2);

/* The smallest action frame that holds the given field. */
#define IEEE80211_MIN_ACTION_SIZE(field)	offsetofend(struct ieee80211_mgmt, u.action.field)

/* action categories and codes */
#define WLAN_CATEGORY_SPECTRUM_MGMT		0
#define WLAN_CATEGORY_BACK			3
#define WLAN_CATEGORY_PUBLIC			4
#define WLAN_CATEGORY_RADIO_MEASUREMENT		5
#define WLAN_CATEGORY_HT			7
#define WLAN_CATEGORY_SA_QUERY			8
#define WLAN_CATEGORY_PROTECTED_DUAL_OF_ACTION	9
#define WLAN_CATEGORY_WNM_UNPROTECTED		11
#define WLAN_CATEGORY_TDLS			12
#define WLAN_CATEGORY_VHT			21
#define WLAN_ACTION_SPCT_TPC_RPRT		3
#define WLAN_PUB_ACTION_FTM_REQUEST		32
#define WLAN_PUB_ACTION_FTM_RESPONSE		33
#define WLAN_UNPROTECTED_WNM_ACTION_TIMING_MEASUREMENT_RESPONSE 1
#define WLAN_TDLS_CHANNEL_SWITCH_REQUEST	5
#define WLAN_TDLS_CHANNEL_SWITCH_RESPONSE	6
#define WLAN_REASON_TDLS_TEARDOWN_UNSPECIFIED	26

#define IEEE80211_SPCT_MSR_RPRT_TYPE_LCI	8

/* authentication algorithms, status and reason codes, capability bits */
#define WLAN_AUTH_OPEN				0
#define WLAN_AUTH_SHARED_KEY			1
#define WLAN_STATUS_SUCCESS			0
#define WLAN_STATUS_UNSPECIFIED_FAILURE		1
#define WLAN_STATUS_NOT_SUPPORTED_AUTH_ALG	13
#define WLAN_STATUS_UNKNOWN_AUTH_TRANSACTION	14
#define WLAN_STATUS_AP_UNABLE_TO_HANDLE_NEW_STA	17
#define WLAN_REASON_UNSPECIFIED			1
#define WLAN_REASON_PREV_AUTH_NOT_VALID		2
#define WLAN_REASON_DEAUTH_LEAVING		3
#define WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY	4
#define WLAN_REASON_DISASSOC_STA_HAS_LEFT	8
#define WLAN_CAPABILITY_ESS			(1 << 0)
#define WLAN_CAPABILITY_IBSS			(1 << 1)
#define WLAN_CAPABILITY_PRIVACY			(1 << 4)
#define WLAN_CAPABILITY_SHORT_PREAMBLE		(1 << 5)
#define WLAN_CAPABILITY_SHORT_SLOT_TIME		(1 << 10)

#define IEEE80211_SPCT_MSR_RPRT_TYPE_CIVIC	11

static inline bool ieee80211_is_public_action(struct ieee80211_hdr *hdr, size_t len)
{
	struct ieee80211_mgmt *m = (void *)hdr;
	return len >= IEEE80211_MIN_ACTION_SIZE(category) &&
	       ieee80211_is_action(hdr->frame_control) &&
	       m->u.action.category == WLAN_CATEGORY_PUBLIC;
}

/* An FTM (fine timing measurement) response: the frame a station uses to
   measure its distance to an AP. */
static inline bool ieee80211_is_ftm(struct sk_buff *skb)
{
	struct ieee80211_mgmt *m = (void *)skb->data;
	return skb->len >= IEEE80211_MIN_ACTION_SIZE(ftm) &&
	       ieee80211_is_public_action((void *)m, skb->len) &&
	       m->u.action.action_code == WLAN_PUB_ACTION_FTM_RESPONSE;
}

static inline bool ieee80211_is_timing_measurement(struct sk_buff *skb)
{
	struct ieee80211_mgmt *m = (void *)skb->data;
	return skb->len >= IEEE80211_MIN_ACTION_SIZE(wnm_timing_msr) &&
	       ieee80211_is_action(m->frame_control) &&
	       m->u.action.category == WLAN_CATEGORY_WNM_UNPROTECTED &&
	       m->u.action.action_code == WLAN_UNPROTECTED_WNM_ACTION_TIMING_MEASUREMENT_RESPONSE;
}

/* May this management frame wait in the AP's buffer while the receiver
   sleeps? Disassoc, deauth and actions may — except FTM, which is
   timing-critical. */
static inline bool ieee80211_is_bufferable_mmpdu(struct sk_buff *skb)
{
	struct ieee80211_mgmt *m = (void *)skb->data;
	__le16 fc = m->frame_control;

	if (!ieee80211_is_mgmt(fc))
		return false;
	if (ieee80211_is_disassoc(fc) || ieee80211_is_deauth(fc))
		return true;
	if (!ieee80211_is_action(fc))
		return false;
	if (skb->len < IEEE80211_MIN_ACTION_SIZE(action_code))
		return true;
	if (m->u.action.category != WLAN_CATEGORY_PUBLIC &&
	    m->u.action.category != WLAN_CATEGORY_PROTECTED_DUAL_OF_ACTION)
		return true;
	return m->u.action.action_code != WLAN_PUB_ACTION_FTM_REQUEST &&
	       m->u.action.action_code != WLAN_PUB_ACTION_FTM_RESPONSE;
}

/* Block-ack request (control frame): "please acknowledge from here on". */
#define IEEE80211_BAR_CTRL_TID_INFO_MASK	0xf000
#define IEEE80211_BAR_CTRL_TID_INFO_SHIFT	12

struct ieee80211_bar {
	__le16 frame_control;
	__le16 duration;
	u8 ra[ETH_ALEN];
	u8 ta[ETH_ALEN];
	__le16 control;
	__le16 start_seq_num;
} __packed;

/* --- elements ------------------------------------------------------------------ */

/* An element: id, length, then that many bytes. Frames carry a list. */
struct element {
	u8 id;
	u8 datalen;
	u8 data[];
} __packed;

#define for_each_element(e, buf, buflen)					\
	for (e = (const struct element *)(buf);					\
	     (const u8 *)(buf) + (buflen) - (const u8 *)e >= (int)sizeof(*e) &&	\
	     (const u8 *)(buf) + (buflen) - (const u8 *)e >= (int)sizeof(*e) + e->datalen; \
	     e = (const struct element *)(e->data + e->datalen))
#define for_each_element_id(e, want, buf, buflen)				\
	for_each_element(e, buf, buflen) if (e->id == (want))

/* element ids */
#define WLAN_EID_SSID			0
#define WLAN_EID_SUPP_RATES		1
#define WLAN_EID_DS_PARAMS		3
#define WLAN_EID_TIM			5
#define WLAN_EID_REQUEST		10
#define WLAN_EID_TPC_REPORT		35
#define WLAN_EID_CHANNEL_SWITCH		37
#define WLAN_EID_MEASURE_REPORT		39
#define WLAN_EID_HT_CAPABILITY		45
#define WLAN_EID_RSN			48
#define WLAN_EID_RIC_DATA		57
#define WLAN_EID_EXT_SUPP_RATES		50
#define WLAN_EID_HT_OPERATION		61
#define WLAN_EID_EXT_CHANSWITCH_ANN	60
#define WLAN_EID_MMIE			76
#define WLAN_EID_EXT_CAPABILITY		127
#define WLAN_EID_VHT_CAPABILITY		191
#define WLAN_EID_VHT_OPERATION		192
#define WLAN_EID_S1G_TWT		216
#define WLAN_EID_VENDOR_SPECIFIC	221
#define WLAN_EID_FRAGMENT		242	/* continues the element before (over 255 bytes) */
#define WLAN_EID_EXTENSION		255

/* extended capabilities: byte n of the element, bit b */
#define WLAN_EXT_CAPA1_EXT_CHANNEL_SWITCHING		BIT(2)
#define WLAN_EXT_CAPA3_MULTI_BSSID_SUPPORT		BIT(6)
#define WLAN_EXT_CAPA3_TIMING_MEASUREMENT_SUPPORT	BIT(7)
#define WLAN_EXT_CAPA8_OPMODE_NOTIF			BIT(6)
#define WLAN_EXT_CAPA8_MAX_MSDU_IN_AMSDU_LSB		BIT(7)
#define WLAN_EXT_CAPA9_MAX_MSDU_IN_AMSDU_MSB		BIT(0)
#define WLAN_EXT_CAPA10_TWT_REQUESTER_SUPPORT		BIT(5)
#define WLAN_EXT_CAPA10_OBSS_NARROW_BW_RU_TOLERANCE_SUPPORT BIT(7)

#define IEEE80211_RNR_TBTT_PARAMS_PSD_RESERVED	-128

/* A TPC report (the AP's transmit power and our link margin), from either
   spectrum management or radio measurement: both use action code 3. */
static inline bool ieee80211_action_contains_tpc(struct sk_buff *skb)
{
	struct ieee80211_mgmt *m = (void *)skb->data;
	return ieee80211_is_action(m->frame_control) &&
	       skb->len >= IEEE80211_MIN_ACTION_SIZE(tpc_report) &&
	       (m->u.action.category == WLAN_CATEGORY_SPECTRUM_MGMT ||
		m->u.action.category == WLAN_CATEGORY_RADIO_MEASUREMENT) &&
	       m->u.action.action_code == WLAN_ACTION_SPCT_TPC_RPRT &&
	       m->u.action.tpc_report.tpc_elem_id == WLAN_EID_TPC_REPORT &&
	       m->u.action.tpc_report.tpc_elem_length == sizeof(struct ieee80211_tpc_report_ie);
}

/* A vendor element: the vendor's OUI and its own type byte. */
struct ieee80211_vendor_ie {
	u8 element_id;
	u8 len;
	u8 oui[3];
	u8 oui_type;
} __packed;

#define WLAN_OUI_WFA			0x506f9a
#define WLAN_OUI_TYPE_WFA_P2P		9
#define WLAN_OUI_MICROSOFT		0x0050f2
#define WLAN_OUI_TYPE_MICROSOFT_TPC	8

/* WMM (QoS) element, station side: which access classes use U-APSD */
#define IEEE80211_WMM_IE_STA_QOSINFO_AC_VO	(1 << 0)
#define IEEE80211_WMM_IE_STA_QOSINFO_AC_VI	(1 << 1)
#define IEEE80211_WMM_IE_STA_QOSINFO_AC_BK	(1 << 2)
#define IEEE80211_WMM_IE_STA_QOSINFO_AC_BE	(1 << 3)
#define IEEE80211_WMM_IE_STA_QOSINFO_SP_ALL	0x00

/* The management-frame integrity element (802.11w: protected deauth etc.) */
struct ieee80211_mmie {
	u8 element_id;
	u8 length;
	__le16 key_id;
	u8 sequence_number[6];
	u8 mic[8];
} __packed;

/* --- security suites (OUI 00-0F-AC + a number) ------------------------------------ */

#define SUITE(oui, id)			(((oui) << 8) | (id))
#define WLAN_CIPHER_SUITE_WEP40		SUITE(0x000fac, 1)
#define WLAN_CIPHER_SUITE_TKIP		SUITE(0x000fac, 2)
#define WLAN_CIPHER_SUITE_CCMP		SUITE(0x000fac, 4)
#define WLAN_CIPHER_SUITE_WEP104	SUITE(0x000fac, 5)
#define WLAN_CIPHER_SUITE_AES_CMAC	SUITE(0x000fac, 6)
#define WLAN_CIPHER_SUITE_GCMP		SUITE(0x000fac, 8)
#define WLAN_CIPHER_SUITE_GCMP_256	SUITE(0x000fac, 9)
#define WLAN_CIPHER_SUITE_CCMP_256	SUITE(0x000fac, 10)
#define WLAN_CIPHER_SUITE_BIP_GMAC_128	SUITE(0x000fac, 11)
#define WLAN_CIPHER_SUITE_BIP_GMAC_256	SUITE(0x000fac, 12)
#define WLAN_CIPHER_SUITE_BIP_CMAC_256	SUITE(0x000fac, 13)
#define WLAN_AKM_SUITE_8021X		SUITE(0x000fac, 1)
#define WLAN_AKM_SUITE_PSK		SUITE(0x000fac, 2)
#define WLAN_AKM_SUITE_SAE		SUITE(0x000fac, 8)

#define WLAN_KEY_LEN_WEP40		5
#define WLAN_KEY_LEN_WEP104		13
#define WLAN_KEY_LEN_CCMP		16
#define WLAN_KEY_LEN_TKIP		32
#define WLAN_KEY_LEN_GCMP_256		32

/* --- HT (802.11n, "Wi-Fi 4") ---------------------------------------------------- */

struct ieee80211_mcs_info {
	u8 rx_mask[IEEE80211_HT_MCS_MASK_LEN];	/* which MCS indexes it can receive */
	__le16 rx_highest;
	u8 tx_params;
	u8 reserved[3];
} __packed;

struct ieee80211_ht_cap {
	__le16 cap_info;
	u8 ampdu_params_info;
	struct ieee80211_mcs_info mcs;
	__le16 extended_ht_cap_info;
	__le32 tx_BF_cap_info;
	u8 antenna_selection_info;
} __packed;

#define IEEE80211_HT_CAP_LDPC_CODING		0x0001
#define IEEE80211_HT_CAP_SUP_WIDTH_20_40	0x0002
#define IEEE80211_HT_CAP_SM_PS			0x000c
#define IEEE80211_HT_CAP_GRN_FLD		0x0010
#define IEEE80211_HT_CAP_SGI_20			0x0020
#define IEEE80211_HT_CAP_SGI_40			0x0040
#define IEEE80211_HT_CAP_TX_STBC		0x0080
#define IEEE80211_HT_CAP_RX_STBC		0x0300
#define IEEE80211_HT_CAP_RX_STBC_SHIFT		8
#define IEEE80211_HT_CAP_MAX_AMSDU		0x0800
#define IEEE80211_HT_CAP_DSSSCCK40		0x1000

#define IEEE80211_HT_MCS_RX_HIGHEST_MASK	0x3ff
#define IEEE80211_HT_MCS_TX_DEFINED		0x01
#define IEEE80211_HT_MCS_TX_RX_DIFF		0x02
#define IEEE80211_HT_MCS_TX_MAX_STREAMS_SHIFT	2

#define IEEE80211_HT_OP_MODE_PROTECTION			0x0003
#define IEEE80211_HT_OP_MODE_PROTECTION_NONE		0
#define IEEE80211_HT_OP_MODE_PROTECTION_NONMEMBER	1
#define IEEE80211_HT_OP_MODE_PROTECTION_20MHZ		2
#define IEEE80211_HT_OP_MODE_PROTECTION_NONHT_MIXED	3

/* A-MPDU: frames sent back to back, acknowledged together */
enum ieee80211_max_ampdu_length_exp {
	IEEE80211_HT_MAX_AMPDU_8K = 0,
	IEEE80211_HT_MAX_AMPDU_16K = 1,
	IEEE80211_HT_MAX_AMPDU_32K = 2,
	IEEE80211_HT_MAX_AMPDU_64K = 3,
};
enum ieee80211_min_mpdu_spacing {
	IEEE80211_HT_MPDU_DENSITY_NONE = 0,
	IEEE80211_HT_MPDU_DENSITY_0_25 = 1,
	IEEE80211_HT_MPDU_DENSITY_0_5 = 2,
	IEEE80211_HT_MPDU_DENSITY_1 = 3,
	IEEE80211_HT_MPDU_DENSITY_2 = 4,
	IEEE80211_HT_MPDU_DENSITY_4 = 5,
	IEEE80211_HT_MPDU_DENSITY_8 = 6,
	IEEE80211_HT_MPDU_DENSITY_16 = 7,
};
#define IEEE80211_MAX_MPDU_LEN_HT_3839		3839
#define IEEE80211_MAX_MPDU_LEN_HT_7935		7935
#define IEEE80211_MAX_MPDU_LEN_HT_BA		4095
#define IEEE80211_MAX_AMPDU_BUF_HT		0x40
#define IEEE80211_MAX_AMPDU_BUF_HE		0x100
#define IEEE80211_MAX_AMPDU_BUF_EHT		0x400

/* --- VHT (802.11ac, "Wi-Fi 5") --------------------------------------------------- */

struct ieee80211_vht_mcs_info {
	__le16 rx_mcs_map;	/* 2 bits per spatial stream: enum ieee80211_vht_mcs_support */
	__le16 rx_highest;
	__le16 tx_mcs_map;
	__le16 tx_highest;
} __packed;

struct ieee80211_vht_cap {
	__le32 vht_cap_info;
	struct ieee80211_vht_mcs_info supp_mcs;
} __packed;

enum ieee80211_vht_mcs_support {
	IEEE80211_VHT_MCS_SUPPORT_0_7 = 0,
	IEEE80211_VHT_MCS_SUPPORT_0_8 = 1,
	IEEE80211_VHT_MCS_SUPPORT_0_9 = 2,
	IEEE80211_VHT_MCS_NOT_SUPPORTED = 3,
};

enum ieee80211_vht_chanwidth {
	IEEE80211_VHT_CHANWIDTH_USE_HT = 0,
	IEEE80211_VHT_CHANWIDTH_80MHZ = 1,
	IEEE80211_VHT_CHANWIDTH_160MHZ = 2,
	IEEE80211_VHT_CHANWIDTH_80P80MHZ = 3,
};

enum ieee80211_vht_max_ampdu_length_exp {
	IEEE80211_VHT_MAX_AMPDU_8K = 0,
	IEEE80211_VHT_MAX_AMPDU_16K = 1,
	IEEE80211_VHT_MAX_AMPDU_32K = 2,
	IEEE80211_VHT_MAX_AMPDU_64K = 3,
	IEEE80211_VHT_MAX_AMPDU_128K = 4,
	IEEE80211_VHT_MAX_AMPDU_256K = 5,
	IEEE80211_VHT_MAX_AMPDU_512K = 6,
	IEEE80211_VHT_MAX_AMPDU_1024K = 7,
};

#define IEEE80211_MAX_MPDU_LEN_VHT_3895			3895
#define IEEE80211_MAX_MPDU_LEN_VHT_7991			7991
#define IEEE80211_MAX_MPDU_LEN_VHT_11454		11454

#define IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_3895		0x00000000
#define IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_7991		0x00000001
#define IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_11454		0x00000002
#define IEEE80211_VHT_CAP_MAX_MPDU_MASK			0x00000003
#define IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_160MHZ	0x00000004
#define IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_160_80PLUS80MHZ 0x00000008
#define IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_MASK		0x0000000c
#define IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_SHIFT		2
#define IEEE80211_VHT_CAP_RXLDPC			0x00000010
#define IEEE80211_VHT_CAP_SHORT_GI_80			0x00000020
#define IEEE80211_VHT_CAP_SHORT_GI_160			0x00000040
#define IEEE80211_VHT_CAP_TXSTBC			0x00000080
#define IEEE80211_VHT_CAP_RXSTBC_1			0x00000100
#define IEEE80211_VHT_CAP_RXSTBC_MASK			0x00000700
#define IEEE80211_VHT_CAP_SU_BEAMFORMER_CAPABLE		0x00000800
#define IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE		0x00001000
#define IEEE80211_VHT_CAP_BEAMFORMEE_STS_SHIFT		13
#define IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK		(7 << IEEE80211_VHT_CAP_BEAMFORMEE_STS_SHIFT)
#define IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE		0x00100000
#define IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT 23
#define IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_MASK \
	(7 << IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT)
#define IEEE80211_VHT_CAP_RX_ANTENNA_PATTERN		0x10000000
#define IEEE80211_VHT_CAP_TX_ANTENNA_PATTERN		0x20000000
#define IEEE80211_VHT_CAP_EXT_NSS_BW_MASK		0xc0000000
#define IEEE80211_VHT_CAP_EXT_NSS_BW_SHIFT		30
#define IEEE80211_VHT_EXT_NSS_BW_CAPABLE		(1 << 13)

/* How many spatial streams a VHT peer can use at this width and MCS
   (the "extended NSS" rules for 160 MHz). The 802.11 core has it. */
int ieee80211_get_vht_max_nss(struct ieee80211_vht_cap *cap, enum ieee80211_vht_chanwidth bw,
			      int mcs, bool ext_nss_bw_capable, unsigned int max_vht_nss);

/* --- HE (802.11ax, "Wi-Fi 6") ------------------------------------------------------ */

struct ieee80211_he_cap_elem {
	u8 mac_cap_info[6];
	u8 phy_cap_info[11];
} __packed;

/* per width: 2 bits per spatial stream, enum ieee80211_he_mcs_support */
struct ieee80211_he_mcs_nss_supp {
	__le16 rx_mcs_80;
	__le16 tx_mcs_80;
	__le16 rx_mcs_160;
	__le16 tx_mcs_160;
	__le16 rx_mcs_80p80;
	__le16 tx_mcs_80p80;
} __packed;

enum ieee80211_he_mcs_support {
	IEEE80211_HE_MCS_SUPPORT_0_7 = 0,
	IEEE80211_HE_MCS_SUPPORT_0_9 = 1,
	IEEE80211_HE_MCS_SUPPORT_0_11 = 2,
	IEEE80211_HE_MCS_NOT_SUPPORTED = 3,
};

struct ieee80211_he_6ghz_capa {
	__le16 capa;
} __packed;

struct ieee80211_he_mu_edca_param_ac_rec {
	u8 aifsn;
	u8 ecw_min_max;
	u8 mu_edca_timer;
} __packed;

#define IEEE80211_HE_PPE_THRES_MAX_LEN			25
#define IEEE80211_HE_PPE_THRES_INFO_HEADER_SIZE		7
#define IEEE80211_PPE_THRES_INFO_PPET_SIZE		3
#define IEEE80211_PPE_THRES_NSS_MASK			7
#define IEEE80211_PPE_THRES_RU_INDEX_BITMASK_MASK	0x78
#define IEEE80211_PPE_THRES_RU_INDEX_BITMASK_POS	3

#define IEEE80211_HE_6GHZ_CAP_MIN_MPDU_START		0x0007
#define IEEE80211_HE_6GHZ_CAP_MAX_AMPDU_LEN_EXP		0x0038
#define IEEE80211_HE_6GHZ_CAP_MAX_MPDU_LEN		0x00c0
#define IEEE80211_HE_6GHZ_CAP_RX_ANTPAT_CONS		0x1000
#define IEEE80211_HE_6GHZ_CAP_TX_ANTPAT_CONS		0x2000

#define IEEE80211_HE_MAC_CAP0_HTC_HE				0x01
#define IEEE80211_HE_MAC_CAP1_TF_MAC_PAD_DUR_16US		0x08
#define IEEE80211_HE_MAC_CAP1_MULTI_TID_AGG_RX_QOS_8		0x70
#define IEEE80211_HE_MAC_CAP1_LINK_ADAPTATION			0x80
#define IEEE80211_HE_MAC_CAP2_LINK_ADAPTATION			0x01
#define IEEE80211_HE_MAC_CAP2_BSR				0x08
#define IEEE80211_HE_MAC_CAP2_32BIT_BA_BITMAP			0x20
#define IEEE80211_HE_MAC_CAP2_ACK_EN				0x80
#define IEEE80211_HE_MAC_CAP3_OMI_CONTROL			0x02
#define IEEE80211_HE_MAC_CAP3_MAX_AMPDU_LEN_EXP_EXT_1		0x08
#define IEEE80211_HE_MAC_CAP3_MAX_AMPDU_LEN_EXP_EXT_2		0x10
#define IEEE80211_HE_MAC_CAP3_MAX_AMPDU_LEN_EXP_EXT_3		0x18
#define IEEE80211_HE_MAC_CAP3_MAX_AMPDU_LEN_EXP_MASK		0x18
#define IEEE80211_HE_MAC_CAP3_RX_CTRL_FRAME_TO_MULTIBSS		0x80
#define IEEE80211_HE_MAC_CAP4_BQR				0x04
#define IEEE80211_HE_MAC_CAP4_AMSDU_IN_AMPDU			0x40
#define IEEE80211_HE_MAC_CAP4_MULTI_TID_AGG_TX_QOS_B39		0x80
#define IEEE80211_HE_MAC_CAP5_MULTI_TID_AGG_TX_QOS_B40		0x01
#define IEEE80211_HE_MAC_CAP5_MULTI_TID_AGG_TX_QOS_B41		0x02
#define IEEE80211_HE_MAC_CAP5_OM_CTRL_UL_MU_DATA_DIS_RX		0x10
#define IEEE80211_HE_MAC_CAP5_HE_DYNAMIC_SM_PS			0x20
#define IEEE80211_HE_MAC_CAP5_UL_2x996_TONE_RU			0x08
#define IEEE80211_HE_MAC_CAP5_HT_VHT_TRIG_FRAME_RX		0x80

#define IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G		0x02
#define IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_80MHZ_IN_5G	0x04
#define IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G		0x08
#define IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_MASK_ALL		0x1e
#define IEEE80211_HE_PHY_CAP1_PREAMBLE_PUNC_RX_MASK			0x0f
#define IEEE80211_HE_PHY_CAP1_DEVICE_CLASS_A				0x10
#define IEEE80211_HE_PHY_CAP1_LDPC_CODING_IN_PAYLOAD			0x20
#define IEEE80211_HE_PHY_CAP1_MIDAMBLE_RX_TX_MAX_NSTS			0x80
#define IEEE80211_HE_PHY_CAP2_MIDAMBLE_RX_TX_MAX_NSTS			0x01
#define IEEE80211_HE_PHY_CAP2_NDP_4x_LTF_AND_3_2US			0x02
#define IEEE80211_HE_PHY_CAP2_STBC_TX_UNDER_80MHZ			0x04
#define IEEE80211_HE_PHY_CAP2_STBC_RX_UNDER_80MHZ			0x08
#define IEEE80211_HE_PHY_CAP2_UL_MU_FULL_MU_MIMO			0x40
#define IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_TX_BPSK			0x01
#define IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_TX_MASK			0x03
#define IEEE80211_HE_PHY_CAP3_DCM_MAX_TX_NSS_1				0x00
#define IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_RX_BPSK			0x08
#define IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_RX_MASK			0x18
#define IEEE80211_HE_PHY_CAP3_DCM_MAX_RX_NSS_1				0x00
#define IEEE80211_HE_PHY_CAP4_SU_BEAMFORMEE				0x01
#define IEEE80211_HE_PHY_CAP4_BEAMFORMEE_MAX_STS_UNDER_80MHZ_8		0x1c
#define IEEE80211_HE_PHY_CAP4_BEAMFORMEE_MAX_STS_ABOVE_80MHZ_8		0xe0
#define IEEE80211_HE_PHY_CAP5_BEAMFORMEE_NUM_SND_DIM_UNDER_80MHZ_2	0x01
#define IEEE80211_HE_PHY_CAP5_BEAMFORMEE_NUM_SND_DIM_ABOVE_80MHZ_2	0x08
#define IEEE80211_HE_PHY_CAP6_TRIG_SU_BEAMFORMING_FB			0x04
#define IEEE80211_HE_PHY_CAP6_TRIG_MU_BEAMFORMING_PARTIAL_BW_FB		0x08
#define IEEE80211_HE_PHY_CAP6_PPE_THRESHOLD_PRESENT			0x80
#define IEEE80211_HE_PHY_CAP7_POWER_BOOST_FACTOR_SUPP			0x02
#define IEEE80211_HE_PHY_CAP7_HE_SU_MU_PPDU_4XLTF_AND_08_US_GI		0x04
#define IEEE80211_HE_PHY_CAP7_MAX_NC_1					0x08
#define IEEE80211_HE_PHY_CAP7_MAX_NC_2					0x10
#define IEEE80211_HE_PHY_CAP7_STBC_RX_ABOVE_80MHZ			0x80
#define IEEE80211_HE_PHY_CAP8_HE_ER_SU_PPDU_4XLTF_AND_08_US_GI		0x01
#define IEEE80211_HE_PHY_CAP8_20MHZ_IN_40MHZ_HE_PPDU_IN_2G		0x02
#define IEEE80211_HE_PHY_CAP8_20MHZ_IN_160MHZ_HE_PPDU			0x04
#define IEEE80211_HE_PHY_CAP8_80MHZ_IN_160MHZ_HE_PPDU			0x08
#define IEEE80211_HE_PHY_CAP8_DCM_MAX_RU_242				0x00
#define IEEE80211_HE_PHY_CAP9_TX_1024_QAM_LESS_THAN_242_TONE_RU		0x04
#define IEEE80211_HE_PHY_CAP9_RX_1024_QAM_LESS_THAN_242_TONE_RU		0x08
#define IEEE80211_HE_PHY_CAP9_RX_FULL_BW_SU_USING_MU_WITH_COMP_SIGB	0x10
#define IEEE80211_HE_PHY_CAP9_RX_FULL_BW_SU_USING_MU_WITH_NON_COMP_SIGB	0x20
#define IEEE80211_HE_PHY_CAP9_NOMINAL_PKT_PADDING_MASK			0xc0
#define IEEE80211_HE_PHY_CAP9_NOMINAL_PKT_PADDING_POS			6
#define IEEE80211_HE_PHY_CAP9_NOMINAL_PKT_PADDING_RESERVED		0x3
#define IEEE80211_HE_PHY_CAP10_HE_MU_M1RU_MAX_LTF			0x01

/* --- EHT (802.11be, "Wi-Fi 7") and multi-link ------------------------------------ */

struct ieee80211_eht_cap_elem_fixed {
	u8 mac_cap_info[2];
	u8 phy_cap_info[9];
} __packed;

/* max spatial streams per MCS range: rx in the low nibble, tx in the high */
struct ieee80211_eht_mcs_nss_supp_20mhz_only {
	union {
		struct {
			u8 rx_tx_mcs7_max_nss;
			u8 rx_tx_mcs9_max_nss;
			u8 rx_tx_mcs11_max_nss;
			u8 rx_tx_mcs13_max_nss;
		};
		u8 rx_tx_max_nss[4];
	};
} __packed;

struct ieee80211_eht_mcs_nss_supp_bw {
	union {
		struct {
			u8 rx_tx_mcs9_max_nss;
			u8 rx_tx_mcs11_max_nss;
			u8 rx_tx_mcs13_max_nss;
		};
		u8 rx_tx_max_nss[3];
	};
} __packed;

#define IEEE80211_EHT_MCS_NSS_RX			0x0f
#define IEEE80211_EHT_MCS_NSS_TX			0xf0
#define IEEE80211_EHT_PPE_THRES_MAX_LEN			32
#define IEEE80211_EHT_PPE_THRES_INFO_HEADER_SIZE	9
#define IEEE80211_EHT_PPE_THRES_NSS_MASK		0xf
#define IEEE80211_EHT_PPE_THRES_RU_INDEX_BITMASK_MASK	0x1f0

#define IEEE80211_EHT_MAC_CAP0_OM_CONTROL		0x02
#define IEEE80211_EHT_MAC_CAP0_MAX_MPDU_LEN_MASK	0xc0
#define IEEE80211_EHT_MAC_CAP0_MAX_MPDU_LEN_7991	1
#define IEEE80211_EHT_MAC_CAP0_MAX_MPDU_LEN_11454	2
#define IEEE80211_EHT_MAC_CAP1_MAX_AMPDU_LEN_MASK	0x01

#define IEEE80211_EHT_PHY_CAP0_320MHZ_IN_6GHZ			0x02
#define IEEE80211_EHT_PHY_CAP0_242_TONE_RU_GT20MHZ		0x04
#define IEEE80211_EHT_PHY_CAP0_NDP_4_EHT_LFT_32_GI		0x08
#define IEEE80211_EHT_PHY_CAP0_SU_BEAMFORMEE			0x40
#define IEEE80211_EHT_PHY_CAP0_BEAMFORMEE_SS_80MHZ_MASK		0x80
#define IEEE80211_EHT_PHY_CAP1_BEAMFORMEE_SS_80MHZ_MASK		0x03
#define IEEE80211_EHT_PHY_CAP1_BEAMFORMEE_SS_160MHZ_MASK	0x1c
#define IEEE80211_EHT_PHY_CAP1_BEAMFORMEE_SS_320MHZ_MASK	0xe0
#define IEEE80211_EHT_PHY_CAP2_SOUNDING_DIM_320MHZ_MASK		0xc0
#define IEEE80211_EHT_PHY_CAP3_TRIG_SU_BF_FDBK			0x20
#define IEEE80211_EHT_PHY_CAP4_EHT_MU_PPDU_4_EHT_LTF_08_GI	0x08
#define IEEE80211_EHT_PHY_CAP5_TX_LESS_242_TONE_RU_SUPP		0x02
#define IEEE80211_EHT_PHY_CAP5_RX_LESS_242_TONE_RU_SUPP		0x04
#define IEEE80211_EHT_PHY_CAP5_PPE_THRESHOLD_PRESENT		0x08
#define IEEE80211_EHT_PHY_CAP5_COMMON_NOMINAL_PKT_PAD_MASK	0x30
#define IEEE80211_EHT_PHY_CAP5_COMMON_NOMINAL_PKT_PAD_0US	0
#define IEEE80211_EHT_PHY_CAP5_COMMON_NOMINAL_PKT_PAD_8US	1
#define IEEE80211_EHT_PHY_CAP5_COMMON_NOMINAL_PKT_PAD_16US	2
#define IEEE80211_EHT_PHY_CAP5_COMMON_NOMINAL_PKT_PAD_20US	3
#define IEEE80211_EHT_PHY_CAP5_SUPP_EXTRA_EHT_LTF		0x40
#define IEEE80211_EHT_PHY_CAP8_RX_1024QAM_WIDER_BW_DL_OFDMA	0x01
#define IEEE80211_EHT_PHY_CAP8_RX_4096QAM_WIDER_BW_DL_OFDMA	0x02

#define IEEE80211_EML_CAP_EMLSR_SUPP				0x0001
#define IEEE80211_EML_CAP_EML_PADDING_DELAY			0x000e
#define IEEE80211_EML_CAP_EML_PADDING_DELAY_32US		1
#define IEEE80211_EML_CAP_EML_TRANSITION_DELAY			0x0070
#define IEEE80211_EML_CAP_EMLSR_TRANSITION_DELAY_64US		3
#define IEEE80211_MLD_CAP_OP_TID_TO_LINK_MAP_NEG_SUPP		0x0060
#define IEEE80211_MLD_CAP_OP_TID_TO_LINK_MAP_NEG_SUPP_SAME	1
#define IEEE80211_MLD_CAP_OP_LINK_RECONF_SUPPORT		0x2000

/* --- UHR (802.11bn, "Wi-Fi 8"): only the capability bits the driver states -- */

struct ieee80211_uhr_cap_mac {
	u8 mac_cap[6];
} __packed;

struct ieee80211_uhr_cap_phy {
	__le32 cap;
	u8 reserved;
} __packed;

#define IEEE80211_UHR_MAC_CAP0_DPS_SUPP		0x01
#define IEEE80211_UHR_MAC_CAP0_NPCA_SUPP	0x10
#define IEEE80211_UHR_MAC_CAP1_DUO_SUPP		0x80
#define IEEE80211_UHR_PHY_CAP_ELR_TX		0x00000040
#define IEEE80211_UHR_PHY_CAP_ELR_RX		0x00000080

/* --- Wi-Fi Direct (P2P) notice of absence ------------------------------------------ */

#define IEEE80211_P2P_NOA_DESC_MAX		4
#define IEEE80211_P2P_OPPPS_ENABLE_BIT		BIT(7)
#define IEEE80211_P2P_OPPPS_CTWINDOW_MASK	0x7f

struct ieee80211_p2p_noa_desc {
	u8 count;
	__le32 duration;
	__le32 interval;
	__le32 start_time;
} __packed;

struct ieee80211_p2p_noa_attr {
	u8 index;
	u8 oppps_ctwindow;
	struct ieee80211_p2p_noa_desc desc[IEEE80211_P2P_NOA_DESC_MAX];
} __packed;

#endif
