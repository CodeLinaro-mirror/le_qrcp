/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * nl80211 vendor command handlers for QCC730 RCP WiFi driver.
 * Each handler serialises the request through rcp_wifi_vendor_cmd() which
 * blocks (up to VENDOR_CMD_TIMEOUT_MS in rcp_wifi_mgmt.c) until QCC730 replies.
 *
 * Payload/response layouts are the shared rcp_vc_* packed structs in
 * <rcp_vendor_cmds.h>. Raw request bytes come from `iw ... vendor send` and
 * responses are returned as a single NL attr via `iw ... vendor recv`.
 */

#include <linux/module.h>
#include <linux/nl80211.h>
#include <net/cfg80211.h>
#include <net/netlink.h>

#include "rcp_wifi_cfg80211.h"
#include <rcp_vendor_cmds.h>

/* ------------------------------------------------------------------
 * Helper: build vendor reply skb with raw bytes as NL attr.
 * ------------------------------------------------------------------ */
static int vendor_reply_data(struct wiphy *wiphy, const void *data, size_t len)
{
	struct sk_buff *skb;

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy,
						  nla_total_size(len));
	if (!skb)
		return -ENOMEM;

	if (nla_put(skb, 1, len, data)) {
		kfree_skb(skb);
		return -EMSGSIZE;
	}

	return cfg80211_vendor_cmd_reply(skb);
}

/* ------------------------------------------------------------------
 * Generic SET / GET handler bodies.
 *
 * SET: raw request bytes -> QCC730, no response payload.
 * GET: empty request -> QCC730, response bytes returned via NL attr.
 * ------------------------------------------------------------------ */
static int vendor_set_raw(u32 subcmd, const void *data, int data_len, size_t expect_len)
{
	if (data_len < (int)expect_len)
		return -EINVAL;

	return (rcp_wifi_vendor_cmd(subcmd, data, expect_len, NULL, 0) == RCP_WIFI_OK)
		       ? 0 : -EIO;
}

static int vendor_get_raw(struct wiphy *wiphy, u32 subcmd,
			  const void *req, size_t req_len, size_t resp_len)
{
	u8 resp[RCP_WIFI_VENDOR_CMD_MAX_PAYLOAD] = {0};

	if (rcp_wifi_vendor_cmd(subcmd, req, req_len, resp, resp_len) != RCP_WIFI_OK)
		return -EIO;

	return vendor_reply_data(wiphy, resp, resp_len);
}

/* ------------------------------------------------------------------
 * Vendor command handlers
 * ------------------------------------------------------------------ */

static int rcp_vendor_set_phy_mode(struct wiphy *wiphy, struct wireless_dev *wdev,
				   const void *data, int data_len)
{
	return vendor_set_raw(QCC730_SUBCMD_SET_PHY_MODE, data, data_len,
			      sizeof(struct rcp_vc_phy_mode));
}

static int rcp_vendor_get_phy_mode(struct wiphy *wiphy, struct wireless_dev *wdev,
				   const void *data, int data_len)
{
	return vendor_get_raw(wiphy, QCC730_SUBCMD_GET_PHY_MODE, NULL, 0,
			      sizeof(struct rcp_vc_phy_mode));
}

static int rcp_vendor_get_mac_address(struct wiphy *wiphy, struct wireless_dev *wdev,
				      const void *data, int data_len)
{
	return vendor_get_raw(wiphy, QCC730_SUBCMD_GET_MAC_ADDRESS, NULL, 0,
			      sizeof(struct rcp_vc_mac_address));
}

static int rcp_vendor_get_boot_reason(struct wiphy *wiphy, struct wireless_dev *wdev,
				      const void *data, int data_len)
{
	return vendor_get_raw(wiphy, QCC730_SUBCMD_GET_BOOT_REASON, NULL, 0,
			      sizeof(struct rcp_vc_boot_reason));
}

static int rcp_vendor_set_rts_rate(struct wiphy *wiphy, struct wireless_dev *wdev,
				   const void *data, int data_len)
{
	return vendor_set_raw(QCC730_SUBCMD_SET_RTS_RATE, data, data_len,
			      sizeof(struct rcp_vc_rts_rate));
}

static int rcp_vendor_get_rts_rate(struct wiphy *wiphy, struct wireless_dev *wdev,
				   const void *data, int data_len)
{
	return vendor_get_raw(wiphy, QCC730_SUBCMD_GET_RTS_RATE, NULL, 0,
			      sizeof(struct rcp_vc_rts_rate));
}

static int rcp_vendor_set_rsp_rate(struct wiphy *wiphy, struct wireless_dev *wdev,
				   const void *data, int data_len)
{
	return vendor_set_raw(QCC730_SUBCMD_SET_RSP_RATE, data, data_len,
			      sizeof(struct rcp_vc_rsp_rate));
}

static int rcp_vendor_set_edca_param(struct wiphy *wiphy, struct wireless_dev *wdev,
				     const void *data, int data_len)
{
	return vendor_set_raw(QCC730_SUBCMD_SET_EDCA_PARAM_CFG, data, data_len,
			      sizeof(struct rcp_vc_edca_param));
}

static int rcp_vendor_get_edca_param(struct wiphy *wiphy, struct wireless_dev *wdev,
				     const void *data, int data_len)
{
	/* qid is an input selector carried in the request payload. */
	struct rcp_vc_edca_param req = {0};

	if (data_len >= (int)sizeof(req.qid))
		req.qid = ((const u8 *)data)[0];

	return vendor_get_raw(wiphy, QCC730_SUBCMD_GET_EDCA_PARAM_CFG, &req, sizeof(req),
			      sizeof(req));
}

static int rcp_vendor_set_ba_win_timing(struct wiphy *wiphy, struct wireless_dev *wdev,
					const void *data, int data_len)
{
	return vendor_set_raw(QCC730_SUBCMD_SET_BA_WIN_TIMING, data, data_len,
			      sizeof(struct rcp_vc_ba_win_timing));
}

static int rcp_vendor_get_ba_win_timing(struct wiphy *wiphy, struct wireless_dev *wdev,
					const void *data, int data_len)
{
	return vendor_get_raw(wiphy, QCC730_SUBCMD_GET_BA_WIN_TIMING, NULL, 0,
			      sizeof(struct rcp_vc_ba_win_timing));
}

static int rcp_vendor_set_ba_win_size(struct wiphy *wiphy, struct wireless_dev *wdev,
				      const void *data, int data_len)
{
	return vendor_set_raw(QCC730_SUBCMD_SET_BA_WIN_SIZE, data, data_len,
			      sizeof(struct rcp_vc_ba_win_size));
}

static int rcp_vendor_set_slot_time(struct wiphy *wiphy, struct wireless_dev *wdev,
				    const void *data, int data_len)
{
	return vendor_set_raw(QCC730_SUBCMD_SET_SLOT_TIME, data, data_len,
			      sizeof(struct rcp_vc_slot_time));
}

static int rcp_vendor_get_slot_time(struct wiphy *wiphy, struct wireless_dev *wdev,
				    const void *data, int data_len)
{
	return vendor_get_raw(wiphy, QCC730_SUBCMD_GET_SLOT_TIME, NULL, 0,
			      sizeof(struct rcp_vc_slot_time));
}

static int rcp_vendor_set_bmiss_threshold(struct wiphy *wiphy, struct wireless_dev *wdev,
					  const void *data, int data_len)
{
	return vendor_set_raw(QCC730_SUBCMD_SET_BMISS_THRESHOLD, data, data_len,
			      sizeof(struct rcp_vc_bmiss_threshold));
}

static int rcp_vendor_get_bmiss_threshold(struct wiphy *wiphy, struct wireless_dev *wdev,
					  const void *data, int data_len)
{
	return vendor_get_raw(wiphy, QCC730_SUBCMD_GET_BMISS_THRESHOLD, NULL, 0,
			      sizeof(struct rcp_vc_bmiss_threshold));
}

static int rcp_vendor_set_cts_to_self(struct wiphy *wiphy, struct wireless_dev *wdev,
				      const void *data, int data_len)
{
	return vendor_set_raw(QCC730_SUBCMD_SET_CTS_TO_SELF, data, data_len,
			      sizeof(struct rcp_vc_cts_to_self));
}

static int rcp_vendor_unit_test(struct wiphy *wiphy, struct wireless_dev *wdev,
				const void *data, int data_len)
{
	return vendor_set_raw(QCC730_SUBCMD_UNIT_TEST, data, data_len,
			      sizeof(struct rcp_vc_unit_test));
}

/* ------------------------------------------------------------------
 * Vendor command table
 * ------------------------------------------------------------------ */

#define RCP_VENDOR_OUI 0x001374

#define VC_ENTRY(sub, cb, extra_flags)                                                             \
	{                                                                                          \
		{ .vendor_id = RCP_VENDOR_OUI, .subcmd = (sub) },                                  \
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV | (extra_flags),                               \
		.doit = (cb), .policy = VENDOR_CMD_RAW_DATA,                                        \
	}

const struct wiphy_vendor_command rcp_vendor_commands[] = {
	VC_ENTRY(QCC730_SUBCMD_SET_PHY_MODE, rcp_vendor_set_phy_mode,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_GET_PHY_MODE, rcp_vendor_get_phy_mode,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_GET_MAC_ADDRESS, rcp_vendor_get_mac_address, 0),
	VC_ENTRY(QCC730_SUBCMD_GET_BOOT_REASON, rcp_vendor_get_boot_reason, 0),
	VC_ENTRY(QCC730_SUBCMD_SET_RTS_RATE, rcp_vendor_set_rts_rate,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_GET_RTS_RATE, rcp_vendor_get_rts_rate,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_SET_RSP_RATE, rcp_vendor_set_rsp_rate,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_SET_EDCA_PARAM_CFG, rcp_vendor_set_edca_param,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_GET_EDCA_PARAM_CFG, rcp_vendor_get_edca_param,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_SET_BA_WIN_TIMING, rcp_vendor_set_ba_win_timing,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_GET_BA_WIN_TIMING, rcp_vendor_get_ba_win_timing,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_SET_BA_WIN_SIZE, rcp_vendor_set_ba_win_size,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_SET_SLOT_TIME, rcp_vendor_set_slot_time,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_GET_SLOT_TIME, rcp_vendor_get_slot_time,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_SET_BMISS_THRESHOLD, rcp_vendor_set_bmiss_threshold,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_GET_BMISS_THRESHOLD, rcp_vendor_get_bmiss_threshold,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_SET_CTS_TO_SELF, rcp_vendor_set_cts_to_self,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
	VC_ENTRY(QCC730_SUBCMD_UNIT_TEST, rcp_vendor_unit_test,
		 WIPHY_VENDOR_CMD_NEED_RUNNING),
};

const int rcp_vendor_commands_count = ARRAY_SIZE(rcp_vendor_commands);
