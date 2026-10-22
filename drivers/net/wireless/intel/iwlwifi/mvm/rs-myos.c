// SPDX-License-Identifier: BSD-3-Clause
/*
 * rs-myos.c — the rate-scaling entry points the rest of mvm calls. Linux's
 * rs.c has the same four, choosing between firmware and driver rate
 * scaling.
 *
 * Cards from the 22000 family on (the AX201) choose their own rates in
 * firmware ("TLC offload", rs-fw.c). Older ones (the 7265 in the GPD P2
 * Max) need the driver to hand the firmware a rate table for the AP: the
 * LINK_QUALITY command. Once the link is authorized, every data frame says
 * "use the station's table" (TX_CMD_FLG_STA_RATE, tx.c). Without a table
 * nothing goes out: the 4-way handshake worked (those frames carry their
 * own rate) and DHCP never did.
 *
 * Linux's driver algorithm watches every frame's result and moves the table
 * up and down through HT and MIMO rates. Ours is one fixed table, sent once:
 * the AP's legacy rates, fastest first. The firmware walks down it on its
 * own when a try isn't acknowledged (one entry per retry), so a weak signal
 * still gets through, just slower. Legacy rates only (54 Mbps at most):
 * docs/gaps.md.
 */
#include "mvm.h"
#include "rs.h"

/* Legacy rates by speed, fastest first: OFDM and CCK interleaved, as
   54 48 36 24 18 12 11 9 6 5.5 2 1 Mbps. Indexes are the firmware's
   (IWL_RATE_*_INDEX), the same as mac80211's on 2.4 GHz. */
static const u8 by_speed[] = {
	IWL_RATE_54M_INDEX, IWL_RATE_48M_INDEX, IWL_RATE_36M_INDEX,
	IWL_RATE_24M_INDEX, IWL_RATE_18M_INDEX, IWL_RATE_12M_INDEX,
	IWL_RATE_11M_INDEX, IWL_RATE_9M_INDEX, IWL_RATE_6M_INDEX,
	IWL_RATE_5M_INDEX, IWL_RATE_2M_INDEX, IWL_RATE_1M_INDEX,
};

/* rate_n_flags, version 1 (what these older firmwares read): the PLCP code
   in bits 0-7, bit 9 for CCK, the antenna in bits 14-16. */
static u32 legacy_rate(int idx, u8 ant)
{
	u32 r = iwl_mvm_rate_idx_to_plcp(idx) | ((u32)ant << RATE_MCS_ANT_POS);

	if (idx < IWL_FIRST_OFDM_RATE)
		r |= RATE_MCS_CCK_MSK_V1;
	return r;
}

static void fixed_rate_init(struct iwl_mvm *mvm, struct ieee80211_sta *sta,
			    struct ieee80211_link_sta *link_sta, enum nl80211_band band)
{
	struct iwl_mvm_sta *mvmsta = iwl_mvm_sta_from_mac80211(sta);
	struct iwl_lq_sta *lq_sta = &mvmsta->deflink.lq_sta.rs_drv;
	struct iwl_lq_cmd *lq = &lq_sta->lq;
	u8 valid = iwl_mvm_get_valid_tx_ant(mvm);
	u8 ant = first_antenna(valid);
	/* mac80211's bit i is its band's rate i; 5 GHz has no CCK, so its
	   rate 0 is the firmware's IWL_FIRST_OFDM_RATE. */
	int shift = band == NL80211_BAND_2GHZ ? 0 : IWL_FIRST_OFDM_RATE;
	u32 ap_rates = link_sta->supp_rates[band];
	int n = 0;

	/* The firmware's index for each AP rate, as a bit mask. */
	u32 mask = 0;
	for (int i = 0; i < 32; i++)
		if ((ap_rates & BIT(i)) && i + shift < IWL_RATE_COUNT_LEGACY)
			mask |= BIT(i + shift);
	if (!mask)	/* nothing usable advertised: the mandatory rate */
		mask = band == NL80211_BAND_2GHZ ? BIT(IWL_RATE_1M_INDEX) : BIT(IWL_RATE_6M_INDEX);

	u8 rts = lq->flags & LQ_FLAG_USE_RTS_MSK;	/* tx_protection's, kept */
	memset(lq, 0, sizeof(*lq));
	lq->sta_id = mvmsta->deflink.sta_id;
	lq->flags = rts;
	lq->single_stream_ant_msk = ant;
	lq->dual_stream_ant_msk = valid;
	lq->agg_time_limit = cpu_to_le16(4000);	/* 4 ms, Linux's default */
	lq->agg_disable_start_th = 3;
	lq->agg_frame_cnt_limit = lq_sta->pers.max_agg_bufsize;
	lq->ss_params = cpu_to_le32(LQ_SS_PARAMS_VALID);	/* no STBC, no beamforming */

	for (unsigned int k = 0; k < ARRAY_SIZE(by_speed) && n < LQ_MAX_RETRY_NUM; k++)
		if (mask & BIT(by_speed[k]))
			lq->rs_table[n++] = cpu_to_le32(legacy_rate(by_speed[k], ant));
	/* The slowest rate fills the rest: the last retries are the surest. */
	for (u32 last = le32_to_cpu(lq->rs_table[n - 1]); n < LQ_MAX_RETRY_NUM; n++)
		lq->rs_table[n] = cpu_to_le32(last);

	iwl_mvm_send_lq_cmd(mvm, lq);
}

void iwl_mvm_rs_rate_init(struct iwl_mvm *mvm, struct ieee80211_vif *vif,
			  struct ieee80211_sta *sta, struct ieee80211_bss_conf *link_conf,
			  struct ieee80211_link_sta *link_sta, enum nl80211_band band)
{
	if (!iwl_mvm_has_tlc_offload(mvm)) {
		fixed_rate_init(mvm, sta, link_sta, band);
		return;
	}
	iwl_mvm_rs_fw_rate_init(mvm, vif, sta, link_conf, link_sta, band);
}

/* Firmware rate scaling learns from its own transmissions, and the fixed
   table doesn't change: the driver has nothing to do here. */
void iwl_mvm_rs_tx_status(struct iwl_mvm *mvm, struct ieee80211_sta *sta,
			  int tid, struct ieee80211_tx_info *info, bool ndp)
{
	(void)mvm; (void)sta; (void)tid; (void)info; (void)ndp;
}

/* RTS/CTS before each frame (thermal throttling asks for it). On an old
   card that is a flag in the same table, sent again. */
int iwl_mvm_tx_protection(struct iwl_mvm *mvm, struct iwl_mvm_sta *mvmsta, bool enable)
{
	struct iwl_lq_cmd *lq;

	if (iwl_mvm_has_tlc_offload(mvm))
		return rs_fw_tx_protection(mvm, mvmsta, enable);
	lq = &mvmsta->deflink.lq_sta.rs_drv.lq;
	if (enable)
		lq->flags |= LQ_FLAG_USE_RTS_MSK;
	else
		lq->flags &= ~LQ_FLAG_USE_RTS_MSK;
	if (!lq->rs_table[0])
		return 0;	/* no table yet: fixed_rate_init sends the flag with it */
	return iwl_mvm_send_lq_cmd(mvm, lq);
}

/* No driver algorithm to register with mac80211. */
int iwl_mvm_rate_control_register(void) { return 0; }
void iwl_mvm_rate_control_unregister(void) { }

/* The driver's rate algorithm keeps the last signal strength; neither the
   firmware's nor the fixed table needs it. */
void rs_update_last_rssi(struct iwl_mvm *mvm, struct iwl_mvm_sta *mvmsta, struct ieee80211_rx_status *rx_status)
{
    (void)mvm; (void)mvmsta; (void)rx_status;
}
