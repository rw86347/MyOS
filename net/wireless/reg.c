/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * reg.c — the rules of the country we are in (Linux: cfg80211's reg.c,
 * the part a driver uses).
 *
 * Each country allows Wi-Fi on some frequencies, at some power, at some
 * channel widths; on some only after checking for radar (DFS) or only in
 * answer to an AP (no initiating radiation). Intel's cards know the rules
 * themselves: their firmware works out the country (from the APs it hears)
 * and the driver hands us the rule set — regulatory_set_wiphy_regd. We
 * apply it to the radio's channels: each channel takes the rule that
 * covers its 20 MHz, its power is capped, and it's switched off if no
 * rule covers it. The flags are recomputed from what the driver
 * registered each time, so a new country undoes the old one's limits.
 *
 * No wireless-regdb of our own yet: a card that doesn't know its country
 * keeps the channels its driver registered.
 */
#include "core.h"

/* The rule covering [lo, hi] kHz, or NULL. */
static const struct ieee80211_reg_rule *rule_for(const struct ieee80211_regdomain *rd, u32 lo, u32 hi)
{
    for (u32 i = 0; i < rd->n_reg_rules; i++) {
        const struct ieee80211_freq_range *r = &rd->reg_rules[i].freq_range;
        if (r->start_freq_khz <= lo && hi <= r->end_freq_khz)
            return &rd->reg_rules[i];
    }
    return NULL;
}

static void apply(struct wiphy *wiphy, const struct ieee80211_regdomain *rd)
{
    for (int b = 0; b < NUM_NL80211_BANDS; b++) {
        struct ieee80211_supported_band *sband = wiphy->bands[b];
        for (int i = 0; sband && i < sband->n_channels; i++) {
            struct ieee80211_channel *ch = &sband->channels[i];
            u32 center = ch->center_freq * 1000 + ch->freq_offset;
            const struct ieee80211_reg_rule *r = rule_for(rd, center - 10000, center + 10000);
            u32 flags = ch->orig_flags;
            ch->max_power = ch->orig_mpwr;
            if (!r) {
                ch->flags = flags | IEEE80211_CHAN_DISABLED;
                continue;
            }
            u32 rf = r->flags, bw = r->freq_range.max_bandwidth_khz;
            if (rf & NL80211_RRF_NO_IR)
                flags |= IEEE80211_CHAN_NO_IR;
            if (rf & NL80211_RRF_DFS)
                flags |= IEEE80211_CHAN_RADAR;
            if (rf & NL80211_RRF_NO_OFDM)
                flags |= IEEE80211_CHAN_NO_OFDM;
            if ((rf & NL80211_RRF_NO_HT40MINUS) || bw < 40000 ||
                !rule_for(rd, center - 30000, center + 10000))
                flags |= IEEE80211_CHAN_NO_HT40MINUS;
            if ((rf & NL80211_RRF_NO_HT40PLUS) || bw < 40000 ||
                !rule_for(rd, center - 10000, center + 30000))
                flags |= IEEE80211_CHAN_NO_HT40PLUS;
            if ((rf & NL80211_RRF_NO_80MHZ) || bw < 80000)
                flags |= IEEE80211_CHAN_NO_80MHZ;
            if ((rf & NL80211_RRF_NO_160MHZ) || bw < 160000)
                flags |= IEEE80211_CHAN_NO_160MHZ;
            if (rf & NL80211_RRF_NO_HE)
                flags |= IEEE80211_CHAN_NO_HE;
            ch->flags = flags;
            int mbm = (int)r->power_rule.max_eirp / 100;      /* mBm -> dBm */
            if (mbm && (!ch->max_power || mbm < ch->max_power))
                ch->max_power = mbm;
        }
    }
}

/* The driver's rules for where we are now. We keep our own copy (the
   driver frees its own), and keep the old one until the new is in place. */
int regulatory_set_wiphy_regd_sync(struct wiphy *wiphy, struct ieee80211_regdomain *rd)
{
    if (!rd || !rd->n_reg_rules)
        return -EINVAL;
    size_t size = sizeof(*rd) + rd->n_reg_rules * sizeof(rd->reg_rules[0]);
    struct ieee80211_regdomain *copy = kmemdup(rd, size, GFP_KERNEL);
    if (!copy)
        return -ENOMEM;
    apply(wiphy, copy);
    const struct ieee80211_regdomain *old = wiphy->regd;
    wiphy->regd = copy;
    kfree(old);
    printk(KERN_INFO "wifi: %s: country %c%c, %u rules\n",
           wiphy_name(wiphy)[0] ? wiphy_name(wiphy) : "new radio",
           copy->alpha2[0], copy->alpha2[1], (unsigned int)copy->n_reg_rules);
    return 0;
}

/* (Linux applies it later, from a work item; at once is as good.) */
int regulatory_set_wiphy_regd(struct wiphy *wiphy, struct ieee80211_regdomain *rd)
{
    return regulatory_set_wiphy_regd_sync(wiphy, rd);
}

/* The wireless-regdb's WMM limits for a country: we have no regdb (the
   driver then uses the standard's defaults). */
int reg_query_regdb_wmm(char *alpha2, int freq, struct ieee80211_reg_rule *rule)
{
    (void)alpha2; (void)freq; (void)rule;
    return -ENODATA;
}
