/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mvm/rs.h — MyOS's own version. Rate scaling picks how fast to send to
 * each station. Old Intel cards did it in the driver (Linux's rs.c, a big
 * GPL-only file we don't take). Every card our copy supports (22000 family
 * and later, the AX201 among them) does it in firmware ("TLC offload",
 * rs-fw.c): the driver only says what the station can do and listens for
 * the firmware's notes. So this header declares that path, plus the few
 * names the shared code mentions for the old one (rs-myos.c).
 */
#ifndef __rs__
#define __rs__

#include <net/mac80211.h>
#include "iwl-config.h"
#include "fw/api/rs.h"

struct iwl_mvm;
struct iwl_mvm_sta;
struct iwl_mvm_link_sta;
struct iwl_rx_cmd_buffer;

#define RS_NAME "iwl-mvm-rs"

#define LINK_QUAL_AGG_FRAME_LIMIT_DEF		63
#define LINK_QUAL_AGG_FRAME_LIMIT_MAX		63
#define LINK_QUAL_AGG_FRAME_LIMIT_GEN2_DEF	64
#define LINK_QUAL_AGG_FRAME_LIMIT_GEN2_MAX	64
#define LINK_QUAL_AGG_FRAME_LIMIT_MIN		0

/* A sent frame's status carries the rate table's "colour" (bits 8-10) and
   the reduced transmit power (bits 0-7) back to rate scaling. */
#define RS_DRV_DATA_TXP_MSK		0xff
#define RS_DRV_DATA_LQ_COLOR_POS	8
#define RS_DRV_DATA_LQ_COLOR_MSK	(7 << RS_DRV_DATA_LQ_COLOR_POS)
#define RS_DRV_DATA_LQ_COLOR_GET(f)	(((f) & RS_DRV_DATA_LQ_COLOR_MSK) >> RS_DRV_DATA_LQ_COLOR_POS)
#define RS_DRV_DATA_PACK(c, p)		((void *)(uintptr_t)(((uintptr_t)(p)) | ((c) << RS_DRV_DATA_LQ_COLOR_POS)))

/* Per-station state of firmware rate scaling (rs-fw.c). */
struct iwl_lq_sta_rs_fw {
	u32 last_rate_n_flags;			/* the last rate the firmware reported */
	struct lq_sta_pers_rs_fw {		/* set once when the station is added */
		u32 sta_id;
#ifdef CONFIG_MAC80211_DEBUGFS
		u32 dbg_fixed_rate;
		u16 dbg_agg_frame_count_lim;
#endif
		u8 chains;
		s8 chain_signal[IEEE80211_MAX_CHAINS];
		s8 last_rssi;
		struct iwl_mvm *drv;
	} pers;
};

/* The driver-side rate scaling's state: only the parts sta.c touches when
   it sets up an old card's aggregation limits. Never live on our cards. */
struct iwl_lq_sta {
	struct iwl_lq_cmd lq;
	struct {
		spinlock_t lock;
		u16 max_agg_bufsize;
	} pers;
};

void iwl_mvm_rs_rate_init(struct iwl_mvm *mvm, struct ieee80211_vif *vif,
			  struct ieee80211_sta *sta, struct ieee80211_bss_conf *link_conf,
			  struct ieee80211_link_sta *link_sta, enum nl80211_band band);
void iwl_mvm_rs_tx_status(struct iwl_mvm *mvm, struct ieee80211_sta *sta,
			  int tid, struct ieee80211_tx_info *info, bool ndp);
int iwl_mvm_rate_control_register(void);
void iwl_mvm_rate_control_unregister(void);
int iwl_mvm_tx_protection(struct iwl_mvm *mvm, struct iwl_mvm_sta *mvmsta, bool enable);
#ifdef CONFIG_IWLWIFI_DEBUGFS
void iwl_mvm_reset_frame_stats(struct iwl_mvm *mvm);
#endif

/* rs-fw.c */
void iwl_mvm_rs_add_sta(struct iwl_mvm *mvm, struct iwl_mvm_sta *mvmsta);
void iwl_mvm_rs_add_sta_link(struct iwl_mvm *mvm, struct iwl_mvm_link_sta *link_sta);
void iwl_mvm_rs_fw_rate_init(struct iwl_mvm *mvm, struct ieee80211_vif *vif,
			     struct ieee80211_sta *sta, struct ieee80211_bss_conf *link_conf,
			     struct ieee80211_link_sta *link_sta, enum nl80211_band band);
int rs_fw_tx_protection(struct iwl_mvm *mvm, struct iwl_mvm_sta *mvmsta, bool enable);
void iwl_mvm_tlc_update_notif(struct iwl_mvm *mvm, struct iwl_rx_cmd_buffer *rxb);
u16 rs_fw_get_max_amsdu_len(struct ieee80211_sta *sta, struct ieee80211_bss_conf *link_conf,
			    struct ieee80211_link_sta *link_sta);

#endif
