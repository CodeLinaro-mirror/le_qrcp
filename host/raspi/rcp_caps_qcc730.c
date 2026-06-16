/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * QCC730 chip capabilities. 802.11n HT20 only, 1 NSS, MCS 0-3 (firmware PHY
 * ceiling — see CR4488883). No VHT/HE.
 */
#include <linux/module.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>

#include "rcp_chip_caps.h"

/* 2.4 GHz channels 1-13 */
static const struct ieee80211_channel rcp_qcc730_chans_2g[] = {
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2412, .hw_value = 1  },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2417, .hw_value = 2  },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2422, .hw_value = 3  },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2427, .hw_value = 4  },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2432, .hw_value = 5  },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2437, .hw_value = 6  },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2442, .hw_value = 7  },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2447, .hw_value = 8  },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2452, .hw_value = 9  },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2457, .hw_value = 10 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2462, .hw_value = 11 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2467, .hw_value = 12 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2472, .hw_value = 13 },
};

static const struct ieee80211_rate rcp_qcc730_rates_bg[] = {
	{ .bitrate = 10  }, { .bitrate = 20  }, { .bitrate = 55  },
	{ .bitrate = 110 }, { .bitrate = 60  }, { .bitrate = 90  },
	{ .bitrate = 120 }, { .bitrate = 180 }, { .bitrate = 240 },
	{ .bitrate = 360 }, { .bitrate = 480 }, { .bitrate = 540 },
};

static const struct ieee80211_rate rcp_qcc730_rates_ofdm[] = {
	{ .bitrate = 60  }, { .bitrate = 90  }, { .bitrate = 120 },
	{ .bitrate = 180 }, { .bitrate = 240 }, { .bitrate = 360 },
	{ .bitrate = 480 }, { .bitrate = 540 },
};

/* 5 GHz channels — UNII-1/2/2e/3 */
static const struct ieee80211_channel rcp_qcc730_chans_5g[] = {
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5180, .hw_value = 36  },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5200, .hw_value = 40  },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5220, .hw_value = 44  },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5240, .hw_value = 48  },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5260, .hw_value = 52  },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5280, .hw_value = 56  },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5300, .hw_value = 60  },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5320, .hw_value = 64  },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5500, .hw_value = 100 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5520, .hw_value = 104 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5540, .hw_value = 108 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5560, .hw_value = 112 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5580, .hw_value = 116 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5600, .hw_value = 120 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5620, .hw_value = 124 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5640, .hw_value = 128 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5660, .hw_value = 132 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5680, .hw_value = 136 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5700, .hw_value = 140 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5745, .hw_value = 149 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5765, .hw_value = 153 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5785, .hw_value = 157 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5805, .hw_value = 161 },
	{ .band = NL80211_BAND_5GHZ, .center_freq = 5825, .hw_value = 165 },
};

const struct rcp_chip_caps rcp_caps_qcc730 = {
	.name		= "QCC730",
	.chans_2g	= rcp_qcc730_chans_2g,
	.n_chans_2g	= ARRAY_SIZE(rcp_qcc730_chans_2g),
	.chans_5g	= rcp_qcc730_chans_5g,
	.n_chans_5g	= ARRAY_SIZE(rcp_qcc730_chans_5g),
	.rates_bg	= rcp_qcc730_rates_bg,
	.n_rates_bg	= ARRAY_SIZE(rcp_qcc730_rates_bg),
	.rates_ofdm	= rcp_qcc730_rates_ofdm,
	.n_rates_ofdm	= ARRAY_SIZE(rcp_qcc730_rates_ofdm),

	.ht_supported	= true,
	.ht_cap_flags	= IEEE80211_HT_CAP_SGI_20,
	.ht_mcs_rx_mask0 = 0x0F,	/* MCS 0-3, 1 NSS */
	.ht_ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K,
	.ht_ampdu_density = IEEE80211_HT_MPDU_DENSITY_NONE,

	.antenna_tx	= BIT(0),
	.antenna_rx	= BIT(0),

	.txpow_policy_safe	= 0,
	.txpow_policy_freerun	= 1,

	.rts_threshold_native	= false,
	.rts_off_threshold	= 2347,
};
