/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * QCC730 RCP vendor command definitions — shared by QCC730 and host.
 *
 * Each subcmd carries a raw packed payload/response struct defined below.
 * The layouts mirror the driver's qcom_wifi_*_params structs (natural,
 * padding-free), so the firmware can memcpy between them directly.
 */

#ifndef RCP_VENDOR_CMDS_H
#define RCP_VENDOR_CMDS_H

#include <stdint.h>
#include <wifi/rcp_wifi_mgmt.h>

/**
 * @brief QCC730 vendor command subcommand IDs.
 *
 * Carried in the subcmd_id field of the protobuf VendorCmd.subcmd_id field.
 */
enum qcc730_vendor_subcmd {
	QCC730_SUBCMD_SET_PHY_MODE        = 1,
	QCC730_SUBCMD_GET_PHY_MODE        = 2,
	QCC730_SUBCMD_GET_MAC_ADDRESS     = 3,
	QCC730_SUBCMD_GET_BOOT_REASON     = 4,
	QCC730_SUBCMD_SET_RTS_RATE        = 5,
	QCC730_SUBCMD_GET_RTS_RATE        = 6,
	QCC730_SUBCMD_SET_RSP_RATE        = 7,
	QCC730_SUBCMD_SET_EDCA_PARAM_CFG  = 8,
	QCC730_SUBCMD_GET_EDCA_PARAM_CFG  = 9,
	QCC730_SUBCMD_SET_BA_WIN_TIMING   = 10,
	QCC730_SUBCMD_GET_BA_WIN_TIMING   = 11,
	QCC730_SUBCMD_SET_BA_WIN_SIZE     = 12,
	QCC730_SUBCMD_SET_SLOT_TIME       = 13,
	QCC730_SUBCMD_GET_SLOT_TIME       = 14,
	QCC730_SUBCMD_SET_BMISS_THRESHOLD = 15,
	QCC730_SUBCMD_GET_BMISS_THRESHOLD = 16,
	QCC730_SUBCMD_SET_CTS_TO_SELF     = 17,
	QCC730_SUBCMD_UNIT_TEST           = 18,
};

/* ------------------------------------------------------------------
 * Per-subcmd payload layouts (raw packed structs, little-endian).
 * Field order/types mirror the driver qcom_wifi_*_params structs.
 * ------------------------------------------------------------------ */

/* SET_PHY_MODE (1) req / GET_PHY_MODE (2) resp */
struct rcp_vc_phy_mode {
	uint32_t phy_mode;
} __attribute__((packed));

/* GET_MAC_ADDRESS (3) resp */
struct rcp_vc_mac_address {
	uint8_t mac[6];
} __attribute__((packed));

/* GET_BOOT_REASON (4) resp */
struct rcp_vc_boot_reason {
	uint32_t boot_reason;
} __attribute__((packed));

/* SET_RTS_RATE (5) req / GET_RTS_RATE (6) resp */
struct rcp_vc_rts_rate {
	uint32_t rts_rate;
} __attribute__((packed));

/* SET_RSP_RATE (7) req */
struct rcp_vc_rsp_rate {
	uint8_t rate_idx;
} __attribute__((packed));

/* SET_EDCA_PARAM_CFG (8) req / GET_EDCA_PARAM_CFG (9) req+resp.
 * For GET, qid is an input selector; the remaining fields are filled on return.
 */
struct rcp_vc_edca_param {
	uint8_t  qid;
	uint8_t  aifsn;
	uint16_t cw_min;
	uint16_t cw_max;
	uint16_t txop_limit;
} __attribute__((packed));

/* SET_BA_WIN_TIMING (10) req / GET_BA_WIN_TIMING (11) resp */
struct rcp_vc_ba_win_timing {
	uint16_t ack_timeout;
	uint16_t delay;
} __attribute__((packed));

/* SET_BA_WIN_SIZE (12) req */
struct rcp_vc_ba_win_size {
	uint16_t tx_size;
	uint16_t rx_size;
} __attribute__((packed));

/* SET_SLOT_TIME (13) req / GET_SLOT_TIME (14) resp */
struct rcp_vc_slot_time {
	uint32_t slot_time;
} __attribute__((packed));

/* SET_BMISS_THRESHOLD (15) req / GET_BMISS_THRESHOLD (16) resp */
struct rcp_vc_bmiss_threshold {
	uint32_t threshold;
} __attribute__((packed));

/* SET_CTS_TO_SELF (17) req */
struct rcp_vc_cts_to_self {
	uint32_t enable;
} __attribute__((packed));

/* UNIT_TEST (18) req (SET-only; results are logged on the QCC730 console) */
struct rcp_vc_unit_test {
	uint8_t  vdev_id;
	uint8_t  module_id;
	uint16_t num_args;
	uint32_t args[16];
} __attribute__((packed));

#endif /* RCP_VENDOR_CMDS_H */
