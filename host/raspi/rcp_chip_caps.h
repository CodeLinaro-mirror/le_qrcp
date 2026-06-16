/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * Per-chip capability description. The generic RCP cfg80211 driver reads a
 * chip's caps (selected via device-tree of_match .data) instead of hardcoding
 * chip constants. Adding a new chip = add one rcp_caps_<chip>.c + one of_match
 * line; the generic code does not change.
 */
#ifndef RCP_CHIP_CAPS_H
#define RCP_CHIP_CAPS_H

#include <linux/types.h>
#include <net/cfg80211.h>

struct rcp_chip_caps {
	const char *name;			/* "QCC730" — logs only */

	/* PHY bands (const templates; probe kmemdups channels to writable copies) */
	const struct ieee80211_channel *chans_2g;	int n_chans_2g;
	const struct ieee80211_channel *chans_5g;	int n_chans_5g;
	const struct ieee80211_rate    *rates_bg;	int n_rates_bg;
	const struct ieee80211_rate    *rates_ofdm;	int n_rates_ofdm;

	/* HT capability (ht_supported=false => no HT advertised) */
	bool ht_supported;
	u16  ht_cap_flags;	/* e.g. IEEE80211_HT_CAP_SGI_20 */
	u8   ht_mcs_rx_mask0;	/* MCS bitmap byte 0, e.g. 0x0F = MCS0-3 */
	u8   ht_ampdu_factor;
	u8   ht_ampdu_density;

	/* Antenna bitmaps */
	u32  antenna_tx;
	u32  antenna_rx;

	/* TX power policy encoding (chip-specific values sent over proto) */
	u8   txpow_policy_safe;		/* LIMITED/SAFE */
	u8   txpow_policy_freerun;	/* FIXED/FREERUN */

	/* RTS capability */
	bool rts_threshold_native;	/* false: chip only on/off (degrade) */
	u32  rts_off_threshold;		/* >= this (or 0xFFFFFFFF) => disable */

	/*
	 * FUTURE behaviour hooks: when a chip needs different *logic* (not just
	 * parameters), add a function pointer here and have the generic op call
	 * it when non-NULL, else fall back to the data-driven default. Keeps the
	 * data-only chips untouched. e.g.:
	 *   int (*set_rts)(struct rcp_priv *priv, u32 threshold);
	 */
};

extern const struct rcp_chip_caps rcp_caps_qcc730;

#endif /* RCP_CHIP_CAPS_H */
