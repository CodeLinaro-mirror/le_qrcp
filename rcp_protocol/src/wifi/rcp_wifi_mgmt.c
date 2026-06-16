/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/* rcp_wifi_mgmt.c
 *
 * RCP WiFi management module.
 *
 * WiFi management module for Qualcomm RCP MCU.
 * This module aims at providing Qualcomm RCP WiFi management functionality
 * for platforms without own WiFi management frameworks (e.g. STM32 applications
 * with lwIP networking).
 *
 * For certain host platforms a dedicated driver (if available) will be a better choice.
 * For example:
 * - Linux OS - RCP cfg80211 driver,
 * - Zephyr RTOS - RCP WiFi device driver.
 */
#include <pb_encode.h>
#include <pb_decode.h>
#include "wifi_mgmt.pb.h"
#include <string.h>

#include <rcp_log.h>
#include <wifi/rcp_wifi_mgmt.h>
#include <rcp_interface.h>
#include <rcp_config.h>

static rcp_wifi_event_callback evt_callback[RCP_WIFI_MAX_CB_CNT];
uint8_t num_cb;

/* Nanopb message structs can be large (>900 bytes). On some RTOS targets
 * (e.g. shell tasks with small stacks) this can lead to stack overflows
 * and hard-to-debug memory corruption.
 *
 * Keep the commonly used TX/RX protobuf objects in static storage to
 * minimize per-call stack usage.
 *
 * NOTE: WiFi mgmt API is not re-entrant. These objects are shared
 * between calls, so callers must not use the API concurrently from
 * multiple threads without external serialization.
 */
static ros_wifi_v1_WifiMgmtRequest tx_mgmt_req;
static ros_wifi_v1_WifiMgmtUplink rx_uplink_msg;

static enum rcp_wifi_status encode_and_send(ros_wifi_v1_WifiMgmtRequest *mgmt_req);
/* Encode disconnection request protobuf */
static bool encode_disconn_req(uint8_t *buf, uint16_t buf_len, uint16_t *enc_len);
/* Encode connection request protobuf */
static bool encode_conn_req(struct rcp_wifi_conn_req *req, uint8_t *buf, uint16_t buf_len,
			    uint16_t *enc_len);
static uint8_t handle_rcp_msg(enum rcp_rx_status status, rcp_buffer_hdl_t *payload_ptr,
			      rcp_error_info_t *err_info_ptr);
static void build_get_l2_interfaces_req(enum rcp_netif_type net_if_type, uint32_t index,
					ros_wifi_v1_WifiMgmtRequest *mgmt_req);
static void build_get_iface_status_req(uint32_t if_index, ros_wifi_v1_WifiMgmtRequest *mgmt_req);
static void build_wifi_ap_enable_req(uint32_t if_index, const struct rcp_wifi_ap_config *cfg,
				     ros_wifi_v1_WifiMgmtRequest *mgmt_req);
static void build_wifi_ap_disable_req(uint32_t if_index, ros_wifi_v1_WifiMgmtRequest *mgmt_req);

static enum rcp_wifi_status encode_and_send(ros_wifi_v1_WifiMgmtRequest *mgmt_req)

{
	link_result_t res;
	rcp_buffer_hdl_t *msg_buffer = get_buffer(RCP_BUF_SIZE);

	if (msg_buffer == NULL) {
		RCP_LOG_ERR("Failed to allocate buffer");
		return RCP_WIFI_ERR_MEM;
	}

	pb_ostream_t stream = pb_ostream_from_buffer(msg_buffer->data_ptr, msg_buffer->max_size);
	bool ok = pb_encode(&stream, ros_wifi_v1_WifiMgmtRequest_fields, mgmt_req);
	if (!ok) {
		RCP_LOG_ERR("Failed to encode WiFi mgmt request: %s", PB_GET_ERROR(&stream));
		release_buffer(&msg_buffer);
		return RCP_WIFI_ERR;
	}
	msg_buffer->size = (uint16_t)stream.bytes_written;

	RCP_LOG_INF("WiFi mgmt TX: op=%u tag=%u len=%u", (unsigned)mgmt_req->op,
		    (unsigned)mgmt_req->which_payload, (unsigned)msg_buffer->size);

	res = link_send(RCP_STREAM_WIFI_MGMT, msg_buffer);

	if (res != RCP_LNK_SUCCESS) {
		RCP_LOG_ERR("WiFi mgmt send failed (%d)", res);
		release_buffer(&msg_buffer);
		return RCP_WIFI_ERR;
	}
	return RCP_WIFI_OK;
}

static void build_get_iface_status_req(uint32_t if_index, ros_wifi_v1_WifiMgmtRequest *mgmt_req)
{
	mgmt_req->payload.get_iface_status = (ros_wifi_v1_GetInterfaceStatusRequest)
		ros_wifi_v1_GetInterfaceStatusRequest_init_default;
	mgmt_req->payload.get_iface_status.iface_index = if_index;
	mgmt_req->which_payload = ros_wifi_v1_WifiMgmtRequest_get_iface_status_tag;
	mgmt_req->op = ros_wifi_v1_RequestType_REQUEST_TYPE_GET_IFACE_STATUS;
	mgmt_req->txn_id = 0;
}

static void build_get_l2_interfaces_req(enum rcp_netif_type net_if_type, uint32_t index,
					ros_wifi_v1_WifiMgmtRequest *mgmt_req)
{
	mgmt_req->payload.get_l2_interfaces =
		(ros_wifi_v1_RequestInterfaces)ros_wifi_v1_RequestInterfaces_init_default;
	mgmt_req->payload.get_l2_interfaces.net_if_type = (ros_wifi_v1_NetIfType)net_if_type;
	mgmt_req->payload.get_l2_interfaces.index = index;
	mgmt_req->which_payload = ros_wifi_v1_WifiMgmtRequest_get_l2_interfaces_tag;
	mgmt_req->op = ros_wifi_v1_RequestType_REQUEST_TYPE_GET_INTERFACES;
	mgmt_req->txn_id = 0;
}

static void build_wifi_scan_req(uint32_t if_index, struct rcp_wifi_scan_params *params,
				ros_wifi_v1_WifiMgmtRequest *mgmt_req)
{
	mgmt_req->payload.scan.iface_index = if_index;
	mgmt_req->payload.scan.has_params = true;

	ros_wifi_v1_WifiScanParams scan_params = ros_wifi_v1_WifiScanParams_init_zero;
	scan_params.scan_type = params->scan_type;
	scan_params.bands_bitmap = params->bands_bitmap;
	scan_params.dwell_time_active_ms = params->dwell_time_active;
	scan_params.dwell_time_passive_ms = params->dwell_time_passive;

	scan_params.ssids_count = params->ssids_count;
	for (pb_size_t idx = 0; idx < scan_params.ssids_count; idx++) {
		scan_params.ssids[idx].size = params->ssids[idx].size;
		memcpy(scan_params.ssids[idx].bytes, params->ssids[idx].bytes,
		       scan_params.ssids[idx].size);
	}

	scan_params.max_bss_cnt = params->max_bss_cnt;

	scan_params.band_chan_count = params->band_chan_count;
	for (pb_size_t idx = 0; idx < params->band_chan_count; idx++) {
		scan_params.band_chan[idx].band = (ros_wifi_v1_WifiBand)params->band_chan[idx].band;
		scan_params.band_chan[idx].channel = params->band_chan[idx].channel;
	}

	mgmt_req->payload.scan.params = scan_params;
	mgmt_req->which_payload = ros_wifi_v1_WifiMgmtRequest_scan_tag;
	mgmt_req->op = ros_wifi_v1_RequestType_REQUEST_TYPE_SCAN;
	mgmt_req->txn_id = 0;
}

static void build_wifi_set_reg_domain_req(const struct rcp_wifi_reg_domain *settings,
					  ros_wifi_v1_WifiMgmtRequest *mgmt_req)
{
	mgmt_req->op = ros_wifi_v1_RequestType_REQUEST_TYPE_SET_REG_DOMAIN;
	mgmt_req->which_payload = ros_wifi_v1_WifiMgmtRequest_set_reg_domain_tag;
	mgmt_req->payload.set_reg_domain.has_reg_domain = true;
	mgmt_req->payload.set_reg_domain.iface_index = settings->iface_index;

	ros_wifi_v1_WifiRegDomain *reg_domain = &mgmt_req->payload.set_reg_domain.reg_domain;

	memcpy(reg_domain->country_code.bytes, settings->country_code, RCP_WIFI_COUNTRY_CODE_LEN);
	reg_domain->country_code.size = RCP_WIFI_COUNTRY_CODE_LEN;
	reg_domain->force_update = settings->force_update;
	reg_domain->channel_info_count = settings->channel_info_count;

	for (int i = 0; i < reg_domain->channel_info_count; i++) {
		reg_domain->channel_info[i].center_frequency_mhz =
			settings->channel_info[i].center_frequency_mhz;
		reg_domain->channel_info[i].max_power_dbm = settings->channel_info[i].max_power_dbm;
		reg_domain->channel_info[i].supported = settings->channel_info[i].supported;
		reg_domain->channel_info[i].passive_only = settings->channel_info[i].passive_only;
		reg_domain->channel_info[i].dfs = settings->channel_info[i].dfs;
	}
}
static void build_wifi_ap_enable_req(uint32_t if_index, const struct rcp_wifi_ap_config *cfg,
				     ros_wifi_v1_WifiMgmtRequest *mgmt_req)
{
	ros_wifi_v1_WifiApEnableRequest req = ros_wifi_v1_WifiApEnableRequest_init_default;
	ros_wifi_v1_WifiApEnableParams params = ros_wifi_v1_WifiApEnableParams_init_default;

	req.iface_index = if_index;
	req.has_params = true;

	params.channel = cfg->channel;
	params.band = (ros_wifi_v1_WifiBand)cfg->band;
	params.security_type = (ros_wifi_v1_WifiSecurityType)cfg->sec_type;
	params.hidden_ssid = cfg->hidden_ssid;
	params.beacon_interval_tu = cfg->beacon_interval_tu;
	params.dtim_period = cfg->dtim_period;

	params.ssid.size = cfg->ssid_len;
	memcpy(params.ssid.bytes, cfg->ssid, cfg->ssid_len);

	params.psk.size = cfg->psk_len;
	if (cfg->psk_len > 0) {
		memcpy(params.psk.bytes, cfg->psk, cfg->psk_len);
	}

	req.params = params;

	mgmt_req->payload.ap_enable = req;
	mgmt_req->which_payload = ros_wifi_v1_WifiMgmtRequest_ap_enable_tag;
	mgmt_req->op = ros_wifi_v1_RequestType_REQUEST_TYPE_AP_ENABLE;
	mgmt_req->txn_id = 0;
}

static void build_wifi_ap_disable_req(uint32_t if_index, ros_wifi_v1_WifiMgmtRequest *mgmt_req)
{
	ros_wifi_v1_WifiApDisableRequest req = ros_wifi_v1_WifiApDisableRequest_init_default;

	req.iface_index = if_index;
	mgmt_req->payload.ap_disable = req;
	mgmt_req->which_payload = ros_wifi_v1_WifiMgmtRequest_ap_disable_tag;
	mgmt_req->op = ros_wifi_v1_RequestType_REQUEST_TYPE_AP_DISABLE;
	mgmt_req->txn_id = 0;
}

/* Call all registered callbacks */
static void call_evt_callbacks(enum rcp_wifi_evt evt, const void *data);

static bool wifi_initialized = false;

enum rcp_wifi_status rcp_wifi_init(void)
{
	if (wifi_initialized) {
		return RCP_WIFI_OK;
	}
	RCP_LOG_INF("nanopb sizeof: WifiMgmtRequest=%u WifiConnectRequest=%u WifiMgmtUplink=%u "
		    "rcp_wifi_conn_req=%u",
		    (unsigned)sizeof(ros_wifi_v1_WifiMgmtRequest),
		    (unsigned)sizeof(ros_wifi_v1_WifiConnectRequest),
		    (unsigned)sizeof(ros_wifi_v1_WifiMgmtUplink),
		    (unsigned)sizeof(struct rcp_wifi_conn_req));

	num_cb = 0;
	link_result_t reg_res = link_register_rx_handler(RCP_STREAM_WIFI_MGMT, handle_rcp_msg);
	if (reg_res != RCP_LNK_SUCCESS) {
		RCP_LOG_ERR("Failed to register WiFi mgmt RX handler (%d)", reg_res);
		return RCP_WIFI_ERR;
	}

	wifi_initialized = true;
	return RCP_WIFI_OK;
}

enum rcp_wifi_status rcp_wifi_register_callback(rcp_wifi_event_callback evt_cb)
{
	if (num_cb == RCP_WIFI_MAX_CB_CNT) {
		return RCP_WIFI_ERR_MEM;
	}

	evt_callback[num_cb++] = evt_cb;
	return RCP_WIFI_OK;
}

enum rcp_wifi_status rcp_wifi_connect(struct rcp_wifi_conn_req *req)
{
	uint16_t encoded = 0;
	link_result_t res;
	rcp_buffer_hdl_t *msg_buffer;
	msg_buffer = get_buffer(RCP_BUF_SIZE);

	if (msg_buffer == NULL) {
		RCP_LOG_ERR("Failed to allocate buffer");
		return RCP_WIFI_ERR_MEM;
	}

	if (!encode_conn_req(req, msg_buffer->data_ptr, msg_buffer->max_size, &encoded)) {
		RCP_LOG_ERR("Failed to encode WiFi connect request");
		release_buffer(&msg_buffer);
		return RCP_WIFI_ERR;
	}
	msg_buffer->size = encoded;
	RCP_LOG_INF("WiFi connect TX: payload_len=%u iface=%u sec=%u ssid_len=%u psk_len=%u",
		    (unsigned)encoded, (unsigned)req->iface_index, (unsigned)req->sec_type,
		    (unsigned)req->ssid_len, (unsigned)req->psk_len);

	res = link_send(RCP_STREAM_WIFI_MGMT, msg_buffer);
	if (res != RCP_LNK_SUCCESS) {
		RCP_LOG_ERR("WiFi connect send failed (%d)", res);
		release_buffer(&msg_buffer);
		return RCP_WIFI_ERR;
	}
	return RCP_WIFI_OK;
}

enum rcp_wifi_status rcp_wifi_disconnect(void)
{
	uint16_t encoded = 0;
	link_result_t res;
	rcp_buffer_hdl_t *msg_buffer;
	msg_buffer = get_buffer(RCP_BUF_SIZE);

	if (msg_buffer == NULL) {
		RCP_LOG_ERR("Failed to allocate buffer");
		return RCP_WIFI_ERR_MEM;
	}

	if (!encode_disconn_req(msg_buffer->data_ptr, msg_buffer->max_size, &encoded)) {
		RCP_LOG_ERR("Failed to encode WiFi disconnect request");
		release_buffer(&msg_buffer);
		return RCP_WIFI_ERR;
	}
	msg_buffer->size = encoded;
	RCP_LOG_INF("WiFi disconnect TX: payload_len=%u", (unsigned)encoded);

	res = link_send(RCP_STREAM_WIFI_MGMT, msg_buffer);
	if (res != RCP_LNK_SUCCESS) {
		RCP_LOG_ERR("WiFi disconnect send failed (%d)", res);
		release_buffer(&msg_buffer);
		return RCP_WIFI_ERR;
	}
	return RCP_WIFI_OK;
}

enum rcp_wifi_status rcp_wifi_ap_enable(uint32_t if_index, const struct rcp_wifi_ap_config *cfg)
{
	if (cfg == NULL) {
		return RCP_WIFI_ERR;
	}

	ros_wifi_v1_WifiMgmtRequest mgmt_req = ros_wifi_v1_WifiMgmtRequest_init_default;
	build_wifi_ap_enable_req(if_index, cfg, &mgmt_req);
	return encode_and_send(&mgmt_req);
}

enum rcp_wifi_status rcp_wifi_ap_disable(uint32_t if_index)
{
	ros_wifi_v1_WifiMgmtRequest mgmt_req = ros_wifi_v1_WifiMgmtRequest_init_default;
	build_wifi_ap_disable_req(if_index, &mgmt_req);
	return encode_and_send(&mgmt_req);
}

enum rcp_wifi_status rcp_wifi_get_l2_interfaces(enum rcp_netif_type net_if_type, uint32_t index)

{
	ros_wifi_v1_WifiMgmtRequest *mgmt_req = &tx_mgmt_req;
	*mgmt_req = (ros_wifi_v1_WifiMgmtRequest)ros_wifi_v1_WifiMgmtRequest_init_default;

	build_get_l2_interfaces_req(net_if_type, index, mgmt_req);
	return encode_and_send(mgmt_req);
}

enum rcp_wifi_status rcp_wifi_get_iface_status(uint32_t if_index)
{
	ros_wifi_v1_WifiMgmtRequest *mgmt_req = &tx_mgmt_req;
	*mgmt_req = (ros_wifi_v1_WifiMgmtRequest)ros_wifi_v1_WifiMgmtRequest_init_default;

	build_get_iface_status_req(if_index, mgmt_req);
	return encode_and_send(mgmt_req);
}

enum rcp_wifi_status rcp_wifi_scan(uint32_t if_index, struct rcp_wifi_scan_params *params)
{
	ros_wifi_v1_WifiMgmtRequest *mgmt_req = &tx_mgmt_req;
	*mgmt_req = (ros_wifi_v1_WifiMgmtRequest)ros_wifi_v1_WifiMgmtRequest_init_default;
	build_wifi_scan_req(if_index, params, mgmt_req);
	return encode_and_send(mgmt_req);
}

enum rcp_wifi_status rcp_wifi_set_reg_domain(const struct rcp_wifi_reg_domain *reg_domain_settings)
{
	ros_wifi_v1_WifiMgmtRequest mgmt_req = ros_wifi_v1_WifiMgmtRequest_init_default;
	build_wifi_set_reg_domain_req(reg_domain_settings, &mgmt_req);
	return encode_and_send(&mgmt_req);
}

enum rcp_wifi_status rcp_wifi_get_reg_domain(uint32_t if_index)
{
	ros_wifi_v1_WifiMgmtRequest mgmt_req = {};
	mgmt_req.op = ros_wifi_v1_RequestType_REQUEST_TYPE_SET_REG_DOMAIN;
	mgmt_req.which_payload = ros_wifi_v1_WifiMgmtRequest_get_reg_domain_tag;
	mgmt_req.payload.get_reg_domain.iface_index = if_index;

	return encode_and_send(&mgmt_req);
}

enum rcp_wifi_status rcp_wifi_get_version()
{
	ros_wifi_v1_WifiMgmtRequest mgmt_req = {};
	mgmt_req.op = ros_wifi_v1_RequestType_REQUEST_TYPE_GET_WIFI_VERSION;
	mgmt_req.which_payload = ros_wifi_v1_WifiMgmtRequest_get_version_tag;
	return encode_and_send(&mgmt_req);
}

static bool encode_conn_req(struct rcp_wifi_conn_req *req, uint8_t *buf, uint16_t buf_len,
			    uint16_t *enc_len)
{
	/* Reduce stack usage: nanopb structs are large. */
	ros_wifi_v1_WifiMgmtRequest *mgmt_req = &tx_mgmt_req;
	*mgmt_req = (ros_wifi_v1_WifiMgmtRequest)ros_wifi_v1_WifiMgmtRequest_init_default;

	ros_wifi_v1_WifiConnectRequest *conn_req = &mgmt_req->payload.connect;
	*conn_req = (ros_wifi_v1_WifiConnectRequest)ros_wifi_v1_WifiConnectRequest_init_default;

	/* --- Validate lengths to prevent buffer overruns --- */
	if (req->ssid_len == 0U || req->ssid_len > WIFI_MAX_SSID_SIZE) {
		RCP_LOG_ERR("Invalid SSID length: %u", (unsigned)req->ssid_len);
		return false;
	}

	if (req->bssid_len && req->bssid_len != WIFI_BSSID_SIZE) {
		RCP_LOG_ERR("Invalid BSSID length: %u", (unsigned)req->bssid_len);
		return false;
	}

	if (req->psk_len > WIFI_MAX_PSK_SIZE) {
		RCP_LOG_ERR("Invalid PSK length: %u", (unsigned)req->psk_len);
		return false;
	}

	if (req->sae_password_len > WIFI_MAX_SAE_PASSWORD_SIZE) {
		RCP_LOG_ERR("Invalid SAE password length: %u", (unsigned)req->sae_password_len);
		return false;
	}

	if (req->anonymous_id_len > WIFI_MAX_ANON_ID_SIZE) {
		RCP_LOG_ERR("Invalid anonymous_id length: %u", (unsigned)req->anonymous_id_len);
		return false;
	}

	if (req->key_password_len > WIFI_MAX_KEY_PASSWORD_SIZE) {
		RCP_LOG_ERR("Invalid key_password length: %u", (unsigned)req->key_password_len);
		return false;
	}

	if (req->key2_password_len > WIFI_MAX_KEY_PASSWORD_SIZE) {
		RCP_LOG_ERR("Invalid key2_password length: %u", (unsigned)req->key2_password_len);
		return false;
	}

	if (req->eap_identity_len > WIFI_MAX_EAP_IDENTITY_SIZE) {
		RCP_LOG_ERR("Invalid eap_identity length: %u", (unsigned)req->eap_identity_len);
		return false;
	}

	if (req->eap_password_len > WIFI_MAX_EAP_PASSWORD_SIZE) {
		RCP_LOG_ERR("Invalid eap_password length: %u", (unsigned)req->eap_password_len);
		return false;
	}

	/* --- Scalars / enums --- */
	conn_req->iface_index = req->iface_index;
	conn_req->band = req->freq_band;
	conn_req->bandwidth = req->freq_bw;
	conn_req->channel = req->channel;
	conn_req->timeout_ms = req->timeout_ms;
	conn_req->security_type = req->sec_type;
	conn_req->mfp = req->mfp;
	conn_req->wpa3_enterprise_type = req->wpa3_ent_type;
	conn_req->tls_cipher_suite = req->tls_cipher_suite;
	conn_req->eap_version = req->eap_version;
	conn_req->ft_enabled = req->ft_enabled;
	conn_req->verify_peer_cert = req->verify_peer_cert;

	/* ignore_broadcast_ssid: keep legacy bool populated for backwards compatibility */
	conn_req->ignore_broadcast_ssid = (req->ignore_broadcast_ssid != 0U);
	conn_req->ignore_broadcast_ssid_mode = req->ignore_broadcast_ssid;

	/* --- Bytes / strings --- */
	memcpy(conn_req->ssid.bytes, req->ssid, req->ssid_len);
	conn_req->ssid.size = req->ssid_len;

	if (req->bssid_len) {
		memcpy(conn_req->bssid.bytes, req->bssid, req->bssid_len);
		conn_req->bssid.size = req->bssid_len;
	}

	if (req->psk_len) {
		memcpy(conn_req->psk.bytes, req->psk, req->psk_len);
		conn_req->psk.size = req->psk_len;
	}

	if (req->sae_password_len) {
		memcpy(conn_req->sae_password.bytes, req->sae_password, req->sae_password_len);
		conn_req->sae_password.size = req->sae_password_len;
	}

	if (req->anonymous_id_len) {
		memcpy(conn_req->anonymous_id.bytes, req->anonymous_id, req->anonymous_id_len);
		conn_req->anonymous_id.size = req->anonymous_id_len;
	}

	if (req->key_password_len) {
		memcpy(conn_req->key_password.bytes, req->key_password, req->key_password_len);
		conn_req->key_password.size = req->key_password_len;
	}

	if (req->key2_password_len) {
		memcpy(conn_req->key2_password.bytes, req->key2_password, req->key2_password_len);
		conn_req->key2_password.size = req->key2_password_len;
	}

	if (req->eap_identity_len) {
		memcpy(conn_req->eap_identity.bytes, req->eap_identity, req->eap_identity_len);
		conn_req->eap_identity.size = req->eap_identity_len;
	}

	if (req->eap_password_len) {
		memcpy(conn_req->eap_password.bytes, req->eap_password, req->eap_password_len);
		conn_req->eap_password.size = req->eap_password_len;
	}

	/* --- EAP identities/passwords arrays (optional) --- */
	if (req->eap_identities_count) {
		uint32_t count = req->eap_identities_count;

		if (count > RCP_WIFI_MAX_EAP_USERS) {
			count = RCP_WIFI_MAX_EAP_USERS;
		}

		conn_req->eap_identities_count = count;
		for (uint32_t i = 0; i < count; i++) {
			uint32_t len = req->eap_identities_len[i];

			if (len > WIFI_MAX_EAP_IDENTITY_SIZE) {
				RCP_LOG_ERR("Invalid eap_identities[%u] length: %u", (unsigned)i,
					    (unsigned)len);
				return false;
			}

			memcpy(conn_req->eap_identities[i].bytes, req->eap_identities[i], len);
			conn_req->eap_identities[i].size = len;
		}
	}

	if (req->eap_passwords_count) {
		uint32_t count = req->eap_passwords_count;

		if (count > RCP_WIFI_MAX_EAP_USERS) {
			count = RCP_WIFI_MAX_EAP_USERS;
		}

		conn_req->eap_passwords_count = count;
		for (uint32_t i = 0; i < count; i++) {
			uint32_t len = req->eap_passwords_len[i];

			if (len > WIFI_MAX_EAP_PASSWORD_SIZE) {
				RCP_LOG_ERR("Invalid eap_passwords[%u] length: %u", (unsigned)i,
					    (unsigned)len);
				return false;
			}

			memcpy(conn_req->eap_passwords[i].bytes, req->eap_passwords[i], len);
			conn_req->eap_passwords[i].size = len;
		}
	}

	mgmt_req->which_payload = ros_wifi_v1_WifiMgmtRequest_connect_tag;
	mgmt_req->op = ros_wifi_v1_RequestType_REQUEST_TYPE_CONNECT;
	mgmt_req->txn_id = 0;

	pb_ostream_t stream = pb_ostream_from_buffer(buf, buf_len);
	bool ok = pb_encode(&stream, ros_wifi_v1_WifiMgmtRequest_fields, mgmt_req);
	if (!ok) {
		RCP_LOG_ERR("Failed to encode connect request: %s", PB_GET_ERROR(&stream));
		return false;
	}

	*enc_len = (uint16_t)stream.bytes_written;
	RCP_LOG_INF("encode_conn_req: encoded=%u bytes (buf=%u)", (unsigned)*enc_len,
		    (unsigned)buf_len);
	return true;
}

static bool encode_disconn_req(uint8_t *buf, uint16_t buf_len, uint16_t *enc_len)
{
	/* Reduce stack usage: nanopb structs are large. */
	ros_wifi_v1_WifiMgmtRequest *mgmt_req = &tx_mgmt_req;
	*mgmt_req = (ros_wifi_v1_WifiMgmtRequest)ros_wifi_v1_WifiMgmtRequest_init_default;

	mgmt_req->payload.disconnect =
		(ros_wifi_v1_WifiDisconnectRequest)ros_wifi_v1_WifiDisconnectRequest_init_default;
	mgmt_req->which_payload = ros_wifi_v1_WifiMgmtRequest_disconnect_tag;
	mgmt_req->op = ros_wifi_v1_RequestType_REQUEST_TYPE_DISCONNECT;
	mgmt_req->txn_id = 0;

	pb_ostream_t stream = pb_ostream_from_buffer(buf, buf_len);
	bool ok = pb_encode(&stream, ros_wifi_v1_WifiMgmtRequest_fields, mgmt_req);
	if (!ok) {
		RCP_LOG_ERR("Failed to encode disconnect request: %s", PB_GET_ERROR(&stream));
		return false;
	}
	*enc_len = (uint16_t)stream.bytes_written;
	RCP_LOG_INF("encode_disconn_req: encoded=%u bytes (buf=%u)", (unsigned)*enc_len,
		    (unsigned)buf_len);
	return true;
}

static void call_evt_callbacks(enum rcp_wifi_evt evt, const void *data)
{
	for (int i = 0; i < num_cb; i++) {
		evt_callback[i](evt, data);
	}
}

static void get_scan_result(struct rcp_wifi_evt_scan_result *result,
			    ros_wifi_v1_WifiScanResult *event)
{
	result->ssid.size = event->ssid.size;
	memcpy(result->ssid.bytes, event->ssid.bytes, event->ssid.size);
	result->rssi = event->rssi;
	result->channel = event->channel;
	result->band = (enum rcp_wifi_band)event->band;
	memcpy(result->bssid, event->bssid.bytes, WIFI_BSSID_SIZE);
	result->sec_type = (enum rcp_wifi_sec_type)event->security;
	result->wpa_ent_type = (enum rcp_wifi_wpa_ent_type)event->wpa3_enterprise_type;
	result->mfp = (enum rcp_wifi_mfp_options)event->mfp;
}

static void handle_wifi_notification(ros_wifi_v1_WifiMgmtNotification *notif)
{
	if (!notif->has_event) {
		return;
	}

	RCP_LOG_INF("WiFi notification: iface=%u event_tag=%u", (unsigned)notif->event.iface_index,
		    (unsigned)notif->event.which_event_data);

	switch (notif->event.which_event_data) {
	case ros_wifi_v1_WifiEvent_connect_result_event_tag: {
		const struct rcp_wifi_evt_conn conn = {
			.status = notif->event.event_data.connect_result_event.status,
		};
		call_evt_callbacks(RCP_WIFI_CONNECTED, &conn);
		break;
	}
	case ros_wifi_v1_WifiEvent_disconnect_result_event_tag: {
		const struct rcp_wifi_evt_conn conn = {
			.status = notif->event.event_data.disconnect_result_event.reason_code,
		};
		call_evt_callbacks(RCP_WIFI_DISCONNECTED, &conn);
		break;
	}
	case ros_wifi_v1_WifiEvent_ap_enable_result_event_tag: {
		const struct rcp_wifi_evt_ap_result res = {
			.status = notif->event.event_data.ap_enable_result_event.status,
		};
		call_evt_callbacks(RCP_WIFI_AP_ENABLED, &res);
		break;
	}
	case ros_wifi_v1_WifiEvent_ap_disable_result_event_tag: {
		const struct rcp_wifi_evt_ap_result res = {
			.status = notif->event.event_data.ap_disable_result_event.status,
		};
		call_evt_callbacks(RCP_WIFI_AP_DISABLED, &res);
		break;
	}
	case ros_wifi_v1_WifiEvent_ap_sta_connected_event_tag: {
		struct rcp_wifi_evt_ap_sta sta = {0};
		if (notif->event.event_data.ap_sta_connected_event.sta_mac_address.size >=
		    WIFI_BSSID_SIZE) {
			memcpy(sta.sta_mac_address,
			       notif->event.event_data.ap_sta_connected_event.sta_mac_address.bytes,
			       WIFI_BSSID_SIZE);
		} else if (notif->event.event_data.ap_sta_connected_event.sta_mac_address.size >
			   0) {
			memcpy(sta.sta_mac_address,
			       notif->event.event_data.ap_sta_connected_event.sta_mac_address.bytes,
			       notif->event.event_data.ap_sta_connected_event.sta_mac_address.size);
		}
		call_evt_callbacks(RCP_WIFI_AP_STA_CONNECTED, &sta);
		break;
	}
	case ros_wifi_v1_WifiEvent_ap_sta_disconnected_event_tag: {
		struct rcp_wifi_evt_ap_sta sta = {0};
		if (notif->event.event_data.ap_sta_disconnected_event.sta_mac_address.size >=
		    WIFI_BSSID_SIZE) {
			memcpy(sta.sta_mac_address,
			       notif->event.event_data.ap_sta_disconnected_event.sta_mac_address
				       .bytes,
			       WIFI_BSSID_SIZE);
		} else if (notif->event.event_data.ap_sta_disconnected_event.sta_mac_address.size >
			   0) {
			memcpy(sta.sta_mac_address,
			       notif->event.event_data.ap_sta_disconnected_event.sta_mac_address
				       .bytes,
			       notif->event.event_data.ap_sta_disconnected_event.sta_mac_address
				       .size);
		}
		call_evt_callbacks(RCP_WIFI_AP_STA_DISCONNECTED, &sta);
		break;
	}
	case ros_wifi_v1_WifiEvent_scan_result_event_tag: {
		struct rcp_wifi_evt_scan_result result;
		get_scan_result(&result, &notif->event.event_data.scan_result_event);
		call_evt_callbacks(RCP_WIFI_SCAN_RESULT, &result);
		break;
	}
	case ros_wifi_v1_WifiEvent_scan_done_event_tag: {
		call_evt_callbacks(RCP_WIFI_SCAN_DONE, NULL);
		break;
	}
	default:
		printf("Unhandled notification %d\n", notif->event.which_event_data);
	}
}

static void handle_wifi_response(ros_wifi_v1_WifiMgmtResponse *resp)
{
	if (resp->has_status) {
		struct rcp_wifi_evt_status status = {0};
		status.error_code = resp->status.error_code;
		status.success = resp->status.success;

		/*
		 * Safe copy of nanopb string.
		 * NOTE: nanopb strings SHOULD be NUL-terminated, but do not trust it blindly.
		 */
		size_t src_max = sizeof(resp->status.error_message);
		size_t n = 0;
		while (n < src_max && resp->status.error_message[n] != '\0') {
			n++;
		}
		if (n >= WIFI_MAX_ERR_MSG_SIZE) {
			n = WIFI_MAX_ERR_MSG_SIZE - 1;
		}
		memcpy(status.error_msg, resp->status.error_message, n);
		status.error_msg[n] = '\0';

		call_evt_callbacks(RCP_WIFI_STATUS_RESP, &status);
		if (status.success) {
			RCP_LOG_INF("WiFi command success");
		} else {
			RCP_LOG_DBG("WiFi command failure err=%ld msg='%s'",
				    (long)status.error_code, status.error_msg);
		}
	}

	switch (resp->which_payload) {

	case ros_wifi_v1_WifiMgmtResponse_l2_interfaces_tag: {
		struct rcp_wifi_l2_interfaces ifs = {0};
		ifs.count = resp->payload.l2_interfaces.net_ifs_count;
		if (ifs.count > RCP_WIFI_MAX_L2_INTERFACES) {
			ifs.count = RCP_WIFI_MAX_L2_INTERFACES;
		}

		for (pb_size_t i = 0; i < ifs.count; i++) {
			ros_wifi_v1_RequestInterfacesResponse resp_ifs =
				resp->payload.l2_interfaces;
			ifs.net_ifs[i].index = resp_ifs.net_ifs[i].index;
			ifs.net_ifs[i].net_if_type =
				(enum rcp_netif_type)resp_ifs.net_ifs[i].net_if_type;
			memcpy(ifs.net_ifs[i].mac_address, resp_ifs.net_ifs[i].mac_address.bytes,
			       6);
			ifs.net_ifs[i].offload_supported = resp_ifs.net_ifs[i].offload_supported;
			ifs.net_ifs[i].mtu = resp_ifs.net_ifs[i].mtu;
		}

		call_evt_callbacks(RCP_WIFI_L2_INTERFACES_RESP, &ifs);
		break;
	}

	case ros_wifi_v1_WifiMgmtResponse_iface_status_tag: {
		struct rcp_wifi_evt_iface_status evt = {0};

		if (!resp->payload.iface_status.has_status) {
			RCP_LOG_WRN("iface_status response missing status payload");
			/* Still notify, but mark as UNKNOWN to avoid misleading defaults. */
			evt.status.state = RCP_WIFI_STATE_UNKNOWN;
			call_evt_callbacks(RCP_WIFI_IFACE_STATUS_RESP, &evt);
			break;
		}

		ros_wifi_v1_WifiInterfaceStatus *st = &resp->payload.iface_status.status;

		evt.iface_index = 0;
		evt.status.state = (enum rcp_wifi_iface_state)st->state;
		evt.status.iface_mode = (enum rcp_wifi_iface_mode)st->mode;
		evt.status.link_mode = (enum rcp_wifi_link_mode)st->link_mode;

		/* SSID */
		evt.status.ssid_len = 0;
		if (st->ssid.size > 0) {
			uint32_t len = st->ssid.size;
			if (len > WIFI_MAX_SSID_SIZE) {
				len = WIFI_MAX_SSID_SIZE;
			}
			memcpy(evt.status.ssid, st->ssid.bytes, len);
			evt.status.ssid_len = (uint8_t)len;
		}

		/* BSSID (6 bytes) */
		memset(evt.status.bssid, 0, WIFI_BSSID_SIZE);
		if (st->bssid.size >= WIFI_BSSID_SIZE) {
			memcpy(evt.status.bssid, st->bssid.bytes, WIFI_BSSID_SIZE);
		} else if (st->bssid.size > 0) {
			memcpy(evt.status.bssid, st->bssid.bytes, st->bssid.size);
		}

		evt.status.band = (enum rcp_wifi_band)st->band;
		evt.status.channel = st->channel;
		evt.status.security = (enum rcp_wifi_sec_type)st->security;
		evt.status.mfp = (enum rcp_wifi_mfp_options)st->mfp;
		evt.status.rssi = st->rssi;
		evt.status.wpa3_ent_type = (enum rcp_wifi_wpa_ent_type)st->wpa3_enterprise_type;
		evt.status.dtim_period = st->dtim_period;
		evt.status.beacon_interval = st->beacon_interval;
		evt.status.twt_capable = st->twt_capable;
		evt.status.tx_bitrate_kbps = st->tx_bitrate_kbps;

		call_evt_callbacks(RCP_WIFI_IFACE_STATUS_RESP, &evt);
		break;
	}

	case ros_wifi_v1_WifiMgmtResponse_version_tag: {
		struct rcp_wifi_version evt = {};

		memcpy(evt.driver_ver, resp->payload.version.version.driver_version,
		       RCP_WIFI_VERSION_MAX_LEN);
		memcpy(evt.firmware_ver, resp->payload.version.version.firmware_version,
		       RCP_WIFI_VERSION_MAX_LEN);

		call_evt_callbacks(RCP_WIFI_VERSION, &evt);
		break;
	}

	case ros_wifi_v1_WifiMgmtResponse_reg_domain_tag: {
		struct rcp_wifi_reg_domain evt = {};
		ros_wifi_v1_WifiRegDomain *reg_domain = &resp->payload.reg_domain.reg_domain;
		memcpy(evt.country_code, reg_domain->country_code.bytes, RCP_WIFI_COUNTRY_CODE_LEN);
		if (reg_domain->channel_info_count <= RCP_MAX_REG_CHAN_NUM) {
			evt.channel_info_count = reg_domain->channel_info_count;

			for (int i = 0; i < evt.channel_info_count; i++) {
				evt.channel_info[i].center_frequency_mhz =
					reg_domain->channel_info[i].center_frequency_mhz;
				evt.channel_info[i].max_power_dbm =
					reg_domain->channel_info[i].max_power_dbm;
				evt.channel_info[i].supported =
					reg_domain->channel_info[i].supported;
				evt.channel_info[i].passive_only =
					reg_domain->channel_info[i].passive_only;
				evt.channel_info[i].dfs = reg_domain->channel_info[i].dfs;
			}
		}
		call_evt_callbacks(RCP_WIFI_REG_DOMAIN, &evt);
		break;
	}

	default:
		RCP_LOG_DBG("Unsupported tag = %d", resp->which_payload);
	}
}
static uint8_t handle_rcp_msg(enum rcp_rx_status status, rcp_buffer_hdl_t *payload_ptr,
			      rcp_error_info_t *err_info_ptr)
{
	(void)status;

	if (payload_ptr == NULL) {
		RCP_LOG_INF("Discarding empty RCP message");
		return RCP_RX_NO_ACTION;
	}

	ros_wifi_v1_WifiMgmtUplink *msg = &rx_uplink_msg;
	*msg = (ros_wifi_v1_WifiMgmtUplink)ros_wifi_v1_WifiMgmtUplink_init_zero;
	pb_istream_t stream = pb_istream_from_buffer(payload_ptr->data_ptr, payload_ptr->size);

	if (!pb_decode(&stream, ros_wifi_v1_WifiMgmtUplink_fields, msg)) {
		RCP_LOG_ERR("Failed to decode WiFi mgmt uplink (len=%u): %s",
			    (unsigned)payload_ptr->size, PB_GET_ERROR(&stream));
		return RCP_RX_NO_ACTION;
	}

	RCP_LOG_DBG("WiFi mgmt RX: which_payload=%u", (unsigned)msg->which_payload);

	switch (msg->which_payload) {
	case ros_wifi_v1_WifiMgmtUplink_notification_tag:
		handle_wifi_notification(&msg->payload.notification);
		break;
	case ros_wifi_v1_WifiMgmtUplink_response_tag:
		handle_wifi_response(&msg->payload.response);
		break;
	default:
		RCP_LOG_WRN("Unsupported payload tag=%d", msg->which_payload);
		break;
	}

	return RCP_RX_ACK;
}
