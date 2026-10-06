// SPDX-License-Identifier: BSD-3-Clause
/*
 * myos.c — how MyOS starts Intel's Wi-Fi driver (ours, not Linux's). Linux
 * loads iwlwifi.ko, then iwlmvm.ko, each running its module_init; here both
 * are built in and boot calls this once, after the root file system is up
 * (the firmware comes from /lib/firmware, in the background).
 *
 * make IWLDEBUG=1 builds the driver's own debug lines in (CONFIG_IWLWIFI_
 * DEBUG) and switches on the areas that matter when bringing a card up: the
 * commands sent to the firmware, scans, the firmware's state, regulatory,
 * joining a network. They go to the kernel log only (dmesg), not the screen.
 * It also leaves the card stopped after a firmware error instead of
 * restarting it, so the first error stays readable.
 */
#include "iwl-debug.h"
#include "iwl-modparams.h"

int __init_iwl_drv_init(void);
int __init_iwl_mvm_init(void);

void iwlwifi_myos_init(void)
{
#ifdef CONFIG_IWLWIFI_DEBUG
	iwlwifi_mod_params.debug_level = IWL_DL_INFO | IWL_DL_MAC80211 | IWL_DL_HCMD | IWL_DL_TE |
					 IWL_DL_SCAN | IWL_DL_ASSOC | IWL_DL_LAR | IWL_DL_FW |
					 IWL_DL_RF_KILL;
	/* After a firmware error, stop rather than restart: the first error is
	   the one to read (dmesg), and the restarts after it bury it. */
	iwlwifi_mod_params.fw_restart = false;
#endif
	__init_iwl_drv_init();		/* the PCI driver: finds the card */
	__init_iwl_mvm_init();		/* its "mvm" op mode (the AX201's) */
}
