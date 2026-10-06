/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * iwl-debug.h — MyOS's own version of the driver's logging header (Linux's
 * is GPL-only, so it is not copied; docs/WIFI-PLAN.md). Same names, same
 * calls; written from how the driver uses them.
 *
 * Errors, warnings and info lines always print (iwl-debug.c). The many
 * IWL_DEBUG_* lines print only in a build with CONFIG_IWLWIFI_DEBUG and
 * only for the IWL_DL_* areas switched on in iwlwifi_mod_params.debug_level;
 * otherwise they compile to nothing — no call, no format string kept.
 */
#ifndef __iwl_debug_h__
#define __iwl_debug_h__

#include <linux/device.h>
#include "iwl-modparams.h"

enum iwl_err_mode {
	IWL_ERR_MODE_REGULAR,
	IWL_ERR_MODE_RFKILL,		/* expected while the radio is switched off */
	IWL_ERR_MODE_TRACE_ONLY,	/* not printed */
	IWL_ERR_MODE_RATELIMIT,
};

struct device;
void __printf(3, 4) __iwl_err(struct device *dev, enum iwl_err_mode mode, const char *fmt, ...);
void __printf(2, 3) __iwl_warn(struct device *dev, const char *fmt, ...);
void __printf(2, 3) __iwl_info(struct device *dev, const char *fmt, ...);
void __printf(2, 3) __iwl_crit(struct device *dev, const char *fmt, ...);

/* `m` is anything with a ->dev: the op mode, the transport, the driver. */
#define IWL_ERR_DEV(d, f, a...)		__iwl_err((d), IWL_ERR_MODE_REGULAR, f, ## a)
#define IWL_ERR(m, f, a...)		IWL_ERR_DEV((m)->dev, f, ## a)
#define IWL_ERR_LIMIT(m, f, a...)	__iwl_err((m)->dev, IWL_ERR_MODE_RATELIMIT, f, ## a)
#define IWL_WARN(m, f, a...)		__iwl_warn((m)->dev, f, ## a)
#define IWL_INFO(m, f, a...)		__iwl_info((m)->dev, f, ## a)
#define IWL_CRIT(m, f, a...)		__iwl_crit((m)->dev, f, ## a)

/* The debug areas (bits of the debug_level module parameter, as in Linux). */
#define IWL_DL_INFO		0x00000001
#define IWL_DL_MAC80211		0x00000002
#define IWL_DL_HCMD		0x00000004
#define IWL_DL_TDLS		0x00000008
#define IWL_DL_QUOTA		0x00000010
#define IWL_DL_TE		0x00000020
#define IWL_DL_EEPROM		0x00000040
#define IWL_DL_RADIO		0x00000080
#define IWL_DL_POWER		0x00000100
#define IWL_DL_TEMP		0x00000200
#define IWL_DL_WOWLAN		0x00000400
#define IWL_DL_SCAN		0x00000800
#define IWL_DL_ASSOC		0x00001000
#define IWL_DL_DROP		0x00002000
#define IWL_DL_LAR		0x00004000
#define IWL_DL_COEX		0x00008000
#define IWL_DL_FW		0x00010000
#define IWL_DL_RF_KILL		0x00020000
#define IWL_DL_TPT		0x00040000
#define IWL_DL_PTP		0x00080000
#define IWL_DL_RATE		0x00100000
#define IWL_DL_CALIB		0x00200000
#define IWL_DL_WEP		0x00400000
#define IWL_DL_TX		0x00800000
#define IWL_DL_RX		0x01000000
#define IWL_DL_ISR		0x02000000
#define IWL_DL_HT		0x04000000
#define IWL_DL_EHT		0x08000000
#define IWL_DL_11H		0x10000000
#define IWL_DL_STATS		0x20000000
#define IWL_DL_TX_REPLY		0x40000000
#define IWL_DL_TX_QUEUES	0x80000000

#ifdef CONFIG_IWLWIFI_DEBUG
void __printf(5, 6) __iwl_dbg(struct device *dev, u32 level, bool limit,
			      const char *function, const char *fmt, ...);
static inline bool iwl_have_debug_level(u32 level)
{
	return iwlwifi_mod_params.debug_level & level;
}
/* Bytes in hex (a firmware file it couldn't parse), under the same switch. */
#define iwl_print_hex_dump(m, level, p, len)					\
	do {									\
		(void)(m);							\
		if (iwl_have_debug_level(level))				\
			print_hex_dump(KERN_DEBUG, "iwl data: ",		\
				       DUMP_PREFIX_OFFSET, 16, 1, (p), (len), 1); \
	} while (0)
#else
/* Compiled out, but still type-checked: a wrong format is still a warning. */
static inline void __printf(5, 6) __iwl_dbg(struct device *dev, u32 level, bool limit,
					    const char *function, const char *fmt, ...)
{
	(void)dev; (void)level; (void)limit; (void)function; (void)fmt;
}
static inline bool iwl_have_debug_level(u32 level) { (void)level; return false; }
#define iwl_print_hex_dump(m, level, p, len) do { (void)(m); (void)(p); (void)(len); } while (0)
#endif

#define IWL_DEBUG_DEV(d, level, f, a...)	__iwl_dbg((d), (level), false, __func__, f, ## a)
#define IWL_DEBUG(m, level, f, a...)		IWL_DEBUG_DEV((m)->dev, level, f, ## a)
#define IWL_DEBUG_LIMIT(m, level, f, a...)	__iwl_dbg((m)->dev, (level), true, __func__, f, ## a)

#define IWL_DEBUG_INFO(p, f, a...)		IWL_DEBUG(p, IWL_DL_INFO, f, ## a)
#define IWL_DEBUG_MAC80211(p, f, a...)		IWL_DEBUG(p, IWL_DL_MAC80211, f, ## a)
#define IWL_DEBUG_HC(p, f, a...)		IWL_DEBUG(p, IWL_DL_HCMD, f, ## a)
#define IWL_DEBUG_TDLS(p, f, a...)		IWL_DEBUG(p, IWL_DL_TDLS, f, ## a)
#define IWL_DEBUG_QUOTA(p, f, a...)		IWL_DEBUG(p, IWL_DL_QUOTA, f, ## a)
#define IWL_DEBUG_TE(p, f, a...)		IWL_DEBUG(p, IWL_DL_TE, f, ## a)
#define IWL_DEBUG_EEPROM(d, f, a...)		IWL_DEBUG_DEV(d, IWL_DL_EEPROM, f, ## a)
#define IWL_DEBUG_RADIO(p, f, a...)		IWL_DEBUG(p, IWL_DL_RADIO, f, ## a)
#define IWL_DEBUG_DEV_RADIO(d, f, a...)		IWL_DEBUG_DEV(d, IWL_DL_RADIO, f, ## a)
#define IWL_DEBUG_POWER(p, f, a...)		IWL_DEBUG(p, IWL_DL_POWER, f, ## a)
#define IWL_DEBUG_DEV_POWER(d, f, a...)		IWL_DEBUG_DEV(d, IWL_DL_POWER, f, ## a)
#define IWL_DEBUG_TEMP(p, f, a...)		IWL_DEBUG(p, IWL_DL_TEMP, f, ## a)
#define IWL_DEBUG_WOWLAN(p, f, a...)		IWL_DEBUG(p, IWL_DL_WOWLAN, f, ## a)
#define IWL_DEBUG_SCAN(p, f, a...)		IWL_DEBUG(p, IWL_DL_SCAN, f, ## a)
#define IWL_DEBUG_ASSOC(p, f, a...)		IWL_DEBUG(p, IWL_DL_ASSOC | IWL_DL_INFO, f, ## a)
#define IWL_DEBUG_ASSOC_LIMIT(p, f, a...)	IWL_DEBUG_LIMIT(p, IWL_DL_ASSOC | IWL_DL_INFO, f, ## a)
#define IWL_DEBUG_DROP(p, f, a...)		IWL_DEBUG(p, IWL_DL_DROP, f, ## a)
#define IWL_DEBUG_DROP_LIMIT(p, f, a...)	IWL_DEBUG_LIMIT(p, IWL_DL_DROP, f, ## a)
#define IWL_DEBUG_LAR(p, f, a...)		IWL_DEBUG(p, IWL_DL_LAR, f, ## a)
#define IWL_DEBUG_COEX(p, f, a...)		IWL_DEBUG(p, IWL_DL_COEX, f, ## a)
#define IWL_DEBUG_FW(p, f, a...)		IWL_DEBUG(p, IWL_DL_FW, f, ## a)
#define IWL_DEBUG_FW_INFO(p, f, a...)		IWL_DEBUG(p, IWL_DL_INFO | IWL_DL_FW, f, ## a)
#define IWL_DEBUG_RF_KILL(p, f, a...)		IWL_DEBUG(p, IWL_DL_RF_KILL, f, ## a)
#define IWL_DEBUG_TPT(p, f, a...)		IWL_DEBUG(p, IWL_DL_TPT, f, ## a)
#define IWL_DEBUG_PTP(p, f, a...)		IWL_DEBUG(p, IWL_DL_PTP, f, ## a)
#define IWL_DEBUG_RATE(p, f, a...)		IWL_DEBUG(p, IWL_DL_RATE, f, ## a)
#define IWL_DEBUG_RATE_LIMIT(p, f, a...)	IWL_DEBUG_LIMIT(p, IWL_DL_RATE, f, ## a)
#define IWL_DEBUG_CALIB(p, f, a...)		IWL_DEBUG(p, IWL_DL_CALIB, f, ## a)
#define IWL_DEBUG_WEP(p, f, a...)		IWL_DEBUG(p, IWL_DL_WEP, f, ## a)
#define IWL_DEBUG_TX(p, f, a...)		IWL_DEBUG(p, IWL_DL_TX, f, ## a)
#define IWL_DEBUG_RX(p, f, a...)		IWL_DEBUG(p, IWL_DL_RX, f, ## a)
#define IWL_DEBUG_ISR(p, f, a...)		IWL_DEBUG(p, IWL_DL_ISR, f, ## a)
#define IWL_DEBUG_HT(p, f, a...)		IWL_DEBUG(p, IWL_DL_HT, f, ## a)
#define IWL_DEBUG_EHT(p, f, a...)		IWL_DEBUG(p, IWL_DL_EHT, f, ## a)
#define IWL_DEBUG_11H(p, f, a...)		IWL_DEBUG(p, IWL_DL_11H, f, ## a)
#define IWL_DEBUG_STATS(p, f, a...)		IWL_DEBUG(p, IWL_DL_STATS, f, ## a)
#define IWL_DEBUG_STATS_LIMIT(p, f, a...)	IWL_DEBUG_LIMIT(p, IWL_DL_STATS, f, ## a)
#define IWL_DEBUG_TX_REPLY(p, f, a...)		IWL_DEBUG(p, IWL_DL_TX_REPLY, f, ## a)
#define IWL_DEBUG_TX_QUEUES(p, f, a...)		IWL_DEBUG(p, IWL_DL_TX_QUEUES, f, ## a)

#endif
