/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * iwl-devtrace.h — MyOS's own version. Linux's (GPL-only) defines trace
 * points: hooks a tracer can switch on to log every register access and
 * command. We have no tracer, so each one is an empty macro: the
 * arguments are not even evaluated, and the hot paths (every register
 * read and write goes through one) cost nothing.
 */
#ifndef __IWLWIFI_DEVICE_TRACE
#define __IWLWIFI_DEVICE_TRACE

/* (Linux's version pulls these in; code relies on it.) */
#include <linux/skbuff.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>

#define trace_iwlwifi_err(...)			do { } while (0)
#define trace_iwlwifi_warn(...)			do { } while (0)
#define trace_iwlwifi_info(...)			do { } while (0)
#define trace_iwlwifi_crit(...)			do { } while (0)
#define trace_iwlwifi_dbg(...)			do { } while (0)
#define trace_iwlwifi_dev_hcmd(...)		do { } while (0)
#define trace_iwlwifi_dev_rx(...)		do { } while (0)
#define trace_iwlwifi_dev_tx(...)		do { } while (0)
#define trace_iwlwifi_dev_tx_tb(...)		do { } while (0)
#define trace_iwlwifi_dev_ict_read(...)		do { } while (0)
#define trace_iwlwifi_dev_ioread32(...)		do { } while (0)
#define trace_iwlwifi_dev_ioread_prph32(...)	do { } while (0)
#define trace_iwlwifi_dev_iowrite8(...)		do { } while (0)
#define trace_iwlwifi_dev_iowrite32(...)	do { } while (0)
#define trace_iwlwifi_dev_iowrite64(...)	do { } while (0)
#define trace_iwlwifi_dev_iowrite_prph32(...)	do { } while (0)
#define trace_iwlwifi_dev_iowrite_prph64(...)	do { } while (0)
#define trace_iwlwifi_dev_irq(...)		do { } while (0)
#define trace_iwlwifi_dev_irq_msix(...)		do { } while (0)
#define maybe_trace_iwlwifi_dev_rx(...)		do { } while (0)

#endif
