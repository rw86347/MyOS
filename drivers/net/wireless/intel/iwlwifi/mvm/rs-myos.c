// SPDX-License-Identifier: BSD-3-Clause
/*
 * rs-myos.c — the rate-scaling entry points the rest of mvm calls, for
 * cards whose firmware does the rate scaling (mvm/rs.h says why that is
 * all of ours). Linux's rs.c has the same four, choosing between firmware
 * and driver rate scaling; here the driver choice is an error.
 */
#include "mvm.h"
#include "rs.h"

void iwl_mvm_rs_rate_init(struct iwl_mvm *mvm, struct ieee80211_vif *vif,
			  struct ieee80211_sta *sta, struct ieee80211_bss_conf *link_conf,
			  struct ieee80211_link_sta *link_sta, enum nl80211_band band)
{
	if (WARN_ON_ONCE(!iwl_mvm_has_tlc_offload(mvm)))
		return;
	iwl_mvm_rs_fw_rate_init(mvm, vif, sta, link_conf, link_sta, band);
}

/* Firmware rate scaling learns from its own transmissions; the driver
   has nothing to tell it. (mvm only calls this without TLC offload.) */
void iwl_mvm_rs_tx_status(struct iwl_mvm *mvm, struct ieee80211_sta *sta,
			  int tid, struct ieee80211_tx_info *info, bool ndp)
{
	(void)mvm; (void)sta; (void)tid; (void)info; (void)ndp;
}

int iwl_mvm_tx_protection(struct iwl_mvm *mvm, struct iwl_mvm_sta *mvmsta, bool enable)
{
	if (WARN_ON_ONCE(!iwl_mvm_has_tlc_offload(mvm)))
		return -EOPNOTSUPP;
	return rs_fw_tx_protection(mvm, mvmsta, enable);
}

/* No driver algorithm to register with mac80211: the firmware has it. */
int iwl_mvm_rate_control_register(void) { return 0; }
void iwl_mvm_rate_control_unregister(void) { }

/* The driver's rate algorithm keeps the last signal strength; the firmware's
   (ours) measures its own. */
void rs_update_last_rssi(struct iwl_mvm *mvm, struct iwl_mvm_sta *mvmsta, struct ieee80211_rx_status *rx_status)
{
    (void)mvm; (void)mvmsta; (void)rx_status;
}
