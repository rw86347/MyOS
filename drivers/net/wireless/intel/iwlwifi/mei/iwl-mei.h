/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mei/iwl-mei.h — MyOS's own version (Linux's is GPL-only). MEI lets
 * Intel's Management Engine (AMT remote management) share the Wi-Fi card
 * with the OS. MyOS never runs that, so this is what Linux has when MEI
 * is compiled out: the types the driver mentions, and functions that
 * answer "no ME here".
 */
#ifndef __iwl_mei_h__
#define __iwl_mei_h__

#include <linux/if_ether.h>
#include <linux/ieee80211.h>
#include <linux/skbuff.h>

enum iwl_mei_nvm_caps {
	MEI_NVM_CAPS_LARI_SUPPORT	= BIT(0),
	MEI_NVM_CAPS_11AX_SUPPORT	= BIT(1),
};

/* The card's settings as the ME read them (only when the ME owns it). */
struct iwl_mei_nvm {
	u8 hw_addr[ETH_ALEN];
	u8 n_hw_addrs;
	u8 reserved;
	u32 radio_cfg;
	u32 caps;
	u32 nvm_version;
	u32 channels[110];
};

enum iwl_mei_pairwise_cipher {
	IWL_MEI_CIPHER_NONE	= 0,
	IWL_MEI_CIPHER_TKIP	= 2,
	IWL_MEI_CIPHER_CCMP	= 4,
	IWL_MEI_CIPHER_GCMP	= 8,
	IWL_MEI_CIPHER_GCMP_256	= 9,
};

enum iwl_mei_akm_auth {
	IWL_MEI_AKM_AUTH_OPEN		= 0,
	IWL_MEI_AKM_AUTH_RSNA		= 6,
	IWL_MEI_AKM_AUTH_RSNA_PSK	= 7,
	IWL_MEI_AKM_AUTH_SAE		= 9,
};

struct iwl_mei_conn_info {
	u8 lp_state;
	u8 auth_mode;
	u8 ssid_len;
	u8 channel;
	u8 band;
	u8 pairwise_cipher;
	u8 bssid[ETH_ALEN];
	u8 ssid[IEEE80211_MAX_SSID_LEN];
};

struct iwl_mei_colloc_info {
	u8 channel;
	u8 bssid[ETH_ALEN];
};

struct iwl_mei_ops {
	void (*me_conn_status)(void *priv, const struct iwl_mei_conn_info *conn_info);
	void (*rfkill)(void *priv, bool blocked, bool csme_taking_ownership);
	void (*roaming_forbidden)(void *priv, bool forbidden);
	void (*sap_connected)(void *priv);
	void (*nic_stolen)(void *priv);
};

struct net_device;

static inline bool iwl_mei_is_connected(void) { return false; }
static inline struct iwl_mei_nvm *iwl_mei_get_nvm(void) { return NULL; }
static inline int iwl_mei_get_ownership(void) { return 0; }
static inline void iwl_mei_set_rfkill_state(bool hw_rfkill, bool sw_rfkill) { (void)hw_rfkill; (void)sw_rfkill; }
static inline void iwl_mei_set_nic_info(const u8 *mac, const u8 *nvm_mac) { (void)mac; (void)nvm_mac; }
static inline void iwl_mei_set_country_code(u16 mcc) { (void)mcc; }
static inline void iwl_mei_set_power_limit(const __le16 *limit) { (void)limit; }
static inline int iwl_mei_register(void *priv, const struct iwl_mei_ops *ops) { (void)priv; (void)ops; return -EOPNOTSUPP; }
static inline void iwl_mei_start_unregister(void) { }
static inline void iwl_mei_unregister_complete(void) { }
static inline void iwl_mei_set_netdev(struct net_device *netdev) { (void)netdev; }
static inline void iwl_mei_tx_copy_to_csme(struct sk_buff *skb, unsigned int ivlen) { (void)skb; (void)ivlen; }
static inline void iwl_mei_host_associated(const struct iwl_mei_conn_info *c, const struct iwl_mei_colloc_info *l) { (void)c; (void)l; }
static inline void iwl_mei_host_disassociated(void) { }
static inline void iwl_mei_device_state(bool up) { (void)up; }
static inline int iwl_mei_pldr_req(void) { return 0; }
static inline void iwl_mei_alive_notif(bool success) { (void)success; }

#endif
