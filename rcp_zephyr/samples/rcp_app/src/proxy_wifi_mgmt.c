/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/logging/log.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/wifi_utils.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/pm/device.h>
#include <string.h>
#include <errno.h>

#include "wifi_mgmt.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

#include <rcp_config.h>
#include <rcp_error_codes.h>
#include <rcp_interface.h>
#include <rcp_interface_impl.h>
#include <rcp_mux_dispatcher.h>

#include "proxy_wifi_mgmt.h"
#include "proxy_wifi_data.h"
#include <qcom_wifi_mgmt.h>
#include <rcp_vendor_cmds.h>

/* Events supported by the management proxy */
#define WIFI_MGMT_EVENTS                                                                           \
	(NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |                        \
	 NET_EVENT_WIFI_SCAN_DONE | NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_AP_ENABLE_RESULT | \
	 NET_EVENT_WIFI_AP_DISABLE_RESULT | NET_EVENT_WIFI_AP_STA_CONNECTED |                      \
	 NET_EVENT_WIFI_AP_STA_DISCONNECTED)

#define MAX_SSID_SIZE         32
#define MAX_PSK_SIZE          64
#define MAX_SAE_PASSWORD_SIZE 128
#define MAX_ANON_ID_SIZE      128
#define MAX_KEY_PASSWORD_SIZE 64
#define MAX_EAP_IDENTITY_SIZE 128
#define MAX_EAP_PASSWORD_SIZE 64

#define MAX_VERSION_LEN 20

#define PROXY_WIFI_RAM_SECTION __attribute__((section(".wifi_ram")))

LOG_MODULE_REGISTER(wifi_mgmt_proxy, CONFIG_QUALCOMM_RCP_APP_LOG_LEVEL);

static struct net_mgmt_event_callback wifi_mgmt_cb;

/* Host-driven scan gating:
 *
 * A station connect commonly triggers internal scans (scan_ssid=1), which
 * generate a lot of NET_EVENT_WIFI_SCAN_RESULT events. Forward scan events to
 * the host only when the host explicitly requested a scan.
 */
static atomic_t scan_requested = ATOMIC_INIT(0);

static uint32_t iface_ptr_to_index(struct net_if *iface);
static const char *wifi_mgmt_event_name(uint64_t mgmt_event);

#define SEND_UPLINK_WARN_INTERVAL_MS 1000U

static atomic_t scan_uplink_suppressed = ATOMIC_INIT(0);
static atomic_t scan_uplink_dropped = ATOMIC_INIT(0);

static void log_send_uplink_drop(link_result_t res);
static void reset_scan_uplink_suppression(void);
static link_result_t send_uplink_result(ros_wifi_v1_WifiMgmtUplink *uplink);

/* Handle WiFi management events */
static void wifi_mgmt_evt_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
				  struct net_if *iface);
static void handle_wifi_connect_event(struct net_mgmt_event_callback *cb, uint32_t iface_index);
static void handle_wifi_disconnect_event(struct net_mgmt_event_callback *cb, uint32_t iface_index);
static void handle_wifi_scan_result(struct net_mgmt_event_callback *cb, uint32_t iface_index);
static void handle_wifi_ap_enable_event(struct net_mgmt_event_callback *cb, uint32_t iface_index);
static void handle_wifi_ap_disable_event(struct net_mgmt_event_callback *cb, uint32_t iface_index);
static void handle_wifi_ap_sta_connected_event(struct net_mgmt_event_callback *cb,
					       uint32_t iface_index);
static void handle_wifi_ap_sta_disconnected_event(struct net_mgmt_event_callback *cb,
						  uint32_t iface_index);

/* Handle requests from host */
static uint8_t handle_mgmt_msg(enum rcp_rx_status status, rcp_buffer_hdl_t *payload_ptr,
			       rcp_error_info_t *err_info_ptr);
static void handle_disconn_request(void);
static void handle_connect_request(ros_wifi_v1_WifiConnectRequest *conn_req);
static void handle_ap_enable_request(ros_wifi_v1_WifiApEnableRequest *ap_req);
static void handle_ap_disable_request(ros_wifi_v1_WifiApDisableRequest *ap_req);
static void handle_ap_sta_disconnect_request(ros_wifi_v1_WifiApStaDisconnectRequest *req);
static void handle_get_l2_interface_request(ros_wifi_v1_RequestInterfaces *get_l2_req);
static void handle_get_iface_status_request(ros_wifi_v1_GetInterfaceStatusRequest *get_status_req);
static void handle_get_version(ros_wifi_v1_GetWifiVersionRequest *);
static void handle_vendor_cmd_request(ros_wifi_v1_VendorCmd *proto);
static void handle_set_rts_threshold(ros_wifi_v1_SetRtsThresholdRequest *proto);
static void handle_set_tx_power(ros_wifi_v1_SetTxPowerRequest *proto);
static void handle_get_tx_power(ros_wifi_v1_GetTxPowerRequest *proto);
static void handle_set_ps_config_request(ros_wifi_v1_SetPowerSaveConfigRequest *proto);
static void handle_get_ps_config_request(ros_wifi_v1_GetPowerSaveConfigRequest *proto);

/* Encode / decode management requests and events */
static bool encode_wifi_mgmt_evt(ros_wifi_v1_WifiMgmtUplink *evt, uint8_t *buf, uint16_t buf_size,
				 uint16_t *enc_len);
static bool decode_wifi_mgmt_req(ros_wifi_v1_WifiMgmtRequest *evt, uint8_t *buf, uint16_t buf_size);

/* Convert protobuf <-> Zephyr structures */
struct conn_req_storage {
	struct wifi_connect_req_params params;
	uint8_t ssid[MAX_SSID_SIZE];
	uint8_t psk[MAX_PSK_SIZE];
	uint8_t sae_password[MAX_SAE_PASSWORD_SIZE];
	uint8_t anon_id[MAX_ANON_ID_SIZE];
	uint8_t key_password[MAX_KEY_PASSWORD_SIZE];
	uint8_t key2_password[MAX_KEY_PASSWORD_SIZE];
	uint8_t eap_identity[MAX_EAP_IDENTITY_SIZE];
	uint8_t eap_password[MAX_EAP_PASSWORD_SIZE];
};

static int conn_req_from_proto(const ros_wifi_v1_WifiConnectRequest *proto,
			       struct conn_req_storage *store, const char **err_msg);

static void disconn_event_to_proto(const struct wifi_status *status,
				   ros_wifi_v1_WifiMgmtNotification *proto);
static void conn_event_to_proto(const struct wifi_status *status,
				ros_wifi_v1_WifiMgmtNotification *proto);
static void ap_enable_event_to_proto(const struct wifi_status *status,
				     ros_wifi_v1_WifiMgmtNotification *proto);
static void ap_disable_event_to_proto(const struct wifi_status *status,
				      ros_wifi_v1_WifiMgmtNotification *proto);
static void ap_sta_connected_event_to_proto(const struct wifi_ap_sta_info *sta,
					    ros_wifi_v1_WifiMgmtNotification *proto);
static void ap_sta_disconnected_event_to_proto(const struct wifi_ap_sta_info *sta,
					       ros_wifi_v1_WifiMgmtNotification *proto);
static void resp_set_status(int err, ros_wifi_v1_WifiMgmtResponse *resp, const char *err_msg);

/* Send uplink messages over link. */

bool proxy_wifi_mgmt_init(void)
{
	net_mgmt_init_event_callback(&wifi_mgmt_cb, wifi_mgmt_evt_handler, WIFI_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_mgmt_cb);
	register_data_msg_handler(RCP_STREAM_WIFI_MGMT, handle_mgmt_msg);
	LOG_INF("nanopb sizeof: WifiMgmtRequest=%u WifiConnectRequest=%u WifiMgmtUplink=%u",
		(unsigned)sizeof(ros_wifi_v1_WifiMgmtRequest),
		(unsigned)sizeof(ros_wifi_v1_WifiConnectRequest),
		(unsigned)sizeof(ros_wifi_v1_WifiMgmtUplink));
	LOG_INF("sizeof(conn_req_storage)=%u sizeof(wifi_connect_req_params)=%u",
		(unsigned)sizeof(struct conn_req_storage),
		(unsigned)sizeof(struct wifi_connect_req_params));
	return true;
}

static int conn_req_from_proto(const ros_wifi_v1_WifiConnectRequest *proto,
			       struct conn_req_storage *store, const char **err_msg)
{
	struct wifi_connect_req_params *conn = &store->params;

	if (err_msg) {
		*err_msg = NULL;
	}

	/* Clear previous request data (also clears secrets) */
	memset(store, 0, sizeof(*store));

	if (!proto) {
		if (err_msg) {
			*err_msg = "Missing connect request";
		}
		return -EINVAL;
	}

	/* SSID is required */
	if (proto->ssid.size == 0 || proto->ssid.size > MAX_SSID_SIZE) {
		if (err_msg) {
			*err_msg = "Invalid SSID length";
		}
		return -EINVAL;
	}

	memcpy(store->ssid, proto->ssid.bytes, proto->ssid.size);
	conn->ssid = store->ssid;
	conn->ssid_length = proto->ssid.size;

	/* Basic selectors */
	conn->security = (enum wifi_security_type)proto->security_type;
	/* Map protobuf band enum to Zephyr band enum.
	 * NOTE: protobuf enum extends Zephyr with 60GHz and ANY values, so we cannot cast directly.
	 */
	switch ((uint32_t)proto->band) {
	case 0U: /* WIFI_BAND_2_4_GHZ */
		conn->band = WIFI_FREQ_BAND_2_4_GHZ;
		break;
	case 1U: /* WIFI_BAND_5_GHZ */
		conn->band = WIFI_FREQ_BAND_5_GHZ;
		break;
	case 2U: /* WIFI_BAND_6_GHZ */
		conn->band = WIFI_FREQ_BAND_6_GHZ;
		break;
	case 3U: /* WIFI_BAND_UNKNOWN */
	case 5U: /* WIFI_BAND_ANY */
		conn->band = WIFI_FREQ_BAND_UNKNOWN;
		break;
	case 4U: /* WIFI_BAND_60_GHZ (not supported by Zephyr enum) */
	default:
		if (err_msg) {
			*err_msg = "Unsupported WiFi band";
		}
		return -ENOTSUP;
	}
	conn->bandwidth = (enum wifi_frequency_bandwidths)proto->bandwidth;
	/* proto: channel=0 means "any"; Zephyr uses WIFI_CHANNEL_ANY (255). */
	conn->channel = (proto->channel == 0U) ? WIFI_CHANNEL_ANY : (uint8_t)proto->channel;
	conn->mfp = (enum wifi_mfp_options)proto->mfp;
	conn->ft_used = proto->ft_enabled;

	/* timeout_ms -> seconds (wifi_shell.c uses seconds). Round up to preserve non-zero values.
	 */
	if (proto->timeout_ms != 0U) {
		conn->timeout = (int)((proto->timeout_ms + 999U) / 1000U);
	}

	/* Optional target BSSID */
	if (proto->bssid.size) {
		if (proto->bssid.size != WIFI_MAC_ADDR_LEN) {
			if (err_msg) {
				*err_msg = "Invalid BSSID length";
			}
			return -EINVAL;
		}
		memcpy(conn->bssid, proto->bssid.bytes, WIFI_MAC_ADDR_LEN);
	}

	/* PSK/WEP */
	if (proto->psk.size) {
		if (proto->psk.size > MAX_PSK_SIZE) {
			if (err_msg) {
				*err_msg = "Invalid PSK length";
			}
			return -EINVAL;
		}
		memcpy(store->psk, proto->psk.bytes, proto->psk.size);
		conn->psk = store->psk;
		conn->psk_length = proto->psk.size;
	}

	/* SAE */
	if (proto->sae_password.size) {
		if (proto->sae_password.size > MAX_SAE_PASSWORD_SIZE) {
			if (err_msg) {
				*err_msg = "Invalid SAE password length";
			}
			return -EINVAL;
		}
		memcpy(store->sae_password, proto->sae_password.bytes, proto->sae_password.size);
		conn->sae_password = store->sae_password;
		conn->sae_password_length = proto->sae_password.size;
	} else if ((conn->security == WIFI_SECURITY_TYPE_SAE_HNP ||
		    conn->security == WIFI_SECURITY_TYPE_SAE_H2E ||
		    conn->security == WIFI_SECURITY_TYPE_SAE_AUTO ||
		    conn->security == WIFI_SECURITY_TYPE_FT_SAE ||
		    conn->security == WIFI_SECURITY_TYPE_SAE_EXT_KEY) &&
		   conn->psk && conn->psk_length) {
		/* Convenience: allow using 'psk' field for SAE password (matches Zephyr wifi_shell
		 * usage). */
		conn->sae_password = conn->psk;
		conn->sae_password_length = conn->psk_length;
	}

	/* EAP: anonymous identity */
	if (proto->anonymous_id.size) {
		if (proto->anonymous_id.size > MAX_ANON_ID_SIZE) {
			if (err_msg) {
				*err_msg = "Invalid anonymous_id length";
			}
			return -EINVAL;
		}
		memcpy(store->anon_id, proto->anonymous_id.bytes, proto->anonymous_id.size);
		conn->anon_id = store->anon_id;
		conn->aid_length = proto->anonymous_id.size;
	}

	/* EAP key passwords */
	if (proto->key_password.size) {
		if (proto->key_password.size > MAX_KEY_PASSWORD_SIZE) {
			if (err_msg) {
				*err_msg = "Invalid key_password length";
			}
			return -EINVAL;
		}
		memcpy(store->key_password, proto->key_password.bytes, proto->key_password.size);
		conn->key_passwd = store->key_password;
		conn->key_passwd_length = proto->key_password.size;
	}

	if (proto->key2_password.size) {
		if (proto->key2_password.size > MAX_KEY_PASSWORD_SIZE) {
			if (err_msg) {
				*err_msg = "Invalid key2_password length";
			}
			return -EINVAL;
		}
		memcpy(store->key2_password, proto->key2_password.bytes, proto->key2_password.size);
		conn->key2_passwd = store->key2_password;
		conn->key2_passwd_length = proto->key2_password.size;
	}

	/* Enterprise selectors */
	conn->wpa3_ent_mode = (uint8_t)proto->wpa3_enterprise_type;
	conn->eap_ver = (uint8_t)proto->eap_version;
	conn->TLS_cipher = (uint8_t)proto->tls_cipher_suite;
	conn->verify_peer_cert = proto->verify_peer_cert;

	/* EAP identity/password (singular) */
	if (proto->eap_identity.size) {
		if (proto->eap_identity.size > MAX_EAP_IDENTITY_SIZE) {
			if (err_msg) {
				*err_msg = "Invalid eap_identity length";
			}
			return -EINVAL;
		}
		memcpy(store->eap_identity, proto->eap_identity.bytes, proto->eap_identity.size);
		conn->eap_identity = store->eap_identity;
		conn->eap_id_length = proto->eap_identity.size;
	}

	if (proto->eap_password.size) {
		if (proto->eap_password.size > MAX_EAP_PASSWORD_SIZE) {
			if (err_msg) {
				*err_msg = "Invalid eap_password length";
			}
			return -EINVAL;
		}
		memcpy(store->eap_password, proto->eap_password.bytes, proto->eap_password.size);
		conn->eap_password = store->eap_password;
		conn->eap_passwd_length = proto->eap_password.size;
	}

	/* EAP identities/passwords arrays (optional)
	 * NOTE: To keep static RAM usage low on this target, the proxy only uses the
	 * first entry from the repeated fields and maps it to the singular
	 * eap_identity / eap_password fields (and to identities[0]/passwords[0]).
	 */
	if (proto->eap_identities_count) {
		pb_size_t len = proto->eap_identities[0].size;

		if (len == 0 || len > MAX_EAP_IDENTITY_SIZE) {
			if (err_msg) {
				*err_msg = "Invalid eap_identities length";
			}
			return -EINVAL;
		}

		if (!conn->eap_identity) {
			memcpy(store->eap_identity, proto->eap_identities[0].bytes, len);
			conn->eap_identity = store->eap_identity;
			conn->eap_id_length = len;
		}

		if (conn->eap_identity) {
			conn->identities[0] = conn->eap_identity;
			conn->nusers = 1;
		}
	}

	if (proto->eap_passwords_count) {
		pb_size_t len = proto->eap_passwords[0].size;

		if (len == 0 || len > MAX_EAP_PASSWORD_SIZE) {
			if (err_msg) {
				*err_msg = "Invalid eap_passwords length";
			}
			return -EINVAL;
		}

		if (!conn->eap_password) {
			memcpy(store->eap_password, proto->eap_passwords[0].bytes, len);
			conn->eap_password = store->eap_password;
			conn->eap_passwd_length = len;
		}

		if (conn->eap_password) {
			conn->passwords[0] = conn->eap_password;
			conn->passwds = 1;
		}
	}

	/* ignore_broadcast_ssid: prefer numeric mode when available */
	uint32_t ibss = proto->ignore_broadcast_ssid_mode;
	if (ibss == 0U && proto->ignore_broadcast_ssid) {
		ibss = 1U;
	}
	if (ibss > 2U) {
		if (err_msg) {
			*err_msg = "Invalid ignore_broadcast_ssid";
		}
		return -EINVAL;
	}
	conn->ignore_broadcast_ssid = (uint8_t)ibss;

	/* Basic validation from Zephyr wifi_shell.c: MFP required for WPA3. */
	if ((conn->security == WIFI_SECURITY_TYPE_SAE_HNP ||
	     conn->security == WIFI_SECURITY_TYPE_SAE_H2E ||
	     conn->security == WIFI_SECURITY_TYPE_SAE_AUTO ||
	     conn->security == WIFI_SECURITY_TYPE_FT_SAE ||
	     conn->security == WIFI_SECURITY_TYPE_SAE_EXT_KEY ||
	     conn->wpa3_ent_mode != WIFI_WPA3_ENTERPRISE_NA) &&
	    conn->mfp != WIFI_MFP_REQUIRED) {
		if (err_msg) {
			*err_msg = "MFP required for WPA3";
		}
		return -EINVAL;
	}

	/* Channel validation (mirrors Zephyr wifi_shell.c behavior):
	 * - If a specific band is provided, validate channel for that band.
	 * - If band is UNKNOWN/ANY, accept the channel if it is valid in *any* band.
	 */
	if (conn->channel != WIFI_CHANNEL_ANY) {
		bool chan_ok = false;

		switch (conn->band) {
		case WIFI_FREQ_BAND_2_4_GHZ:
		case WIFI_FREQ_BAND_5_GHZ:
		case WIFI_FREQ_BAND_6_GHZ:
			chan_ok = wifi_utils_validate_chan((uint8_t)conn->band, conn->channel);
			break;
		case WIFI_FREQ_BAND_UNKNOWN:
		default:
			/* Unknown/any band: accept channel if valid in any supported band. */
			chan_ok = wifi_utils_validate_chan(WIFI_FREQ_BAND_2_4_GHZ, conn->channel) ||
				  wifi_utils_validate_chan(WIFI_FREQ_BAND_5_GHZ, conn->channel) ||
				  wifi_utils_validate_chan(WIFI_FREQ_BAND_6_GHZ, conn->channel);
			break;
		}

		if (!chan_ok) {
			if (err_msg) {
				*err_msg = "Invalid channel";
			}
			return -EINVAL;
		}
	}

	return 0;
}

static void scan_req_from_proto(ros_wifi_v1_WifiScanParams *proto, struct wifi_scan_params *scan)
{
	/* Proto: PASSIVE=0, ACTIVE=1  |  Zephyr: ACTIVE=0, PASSIVE=1 — invert */
	scan->scan_type = (proto->scan_type == ros_wifi_v1_WifiScanType_WIFI_SCAN_TYPE_ACTIVE)
			? WIFI_SCAN_TYPE_ACTIVE : WIFI_SCAN_TYPE_PASSIVE;
	/* QCC730 WiFi driver only supports active scan with default params —
	 * ignore dwell_time and bands to avoid -EINVAL. */
	scan->dwell_time_active = 0;
	scan->dwell_time_passive = 0;
	scan->bands = 0;

	if (proto->ssids_count == 1) {
		scan->ssids[0] = proto->ssids[0].bytes;
	}

	scan->max_bss_cnt = proto->max_bss_cnt;

	for (pb_size_t idx = 0; idx < proto->band_chan_count; idx++) {
		scan->band_chan[idx].band = (ros_wifi_v1_WifiBand)proto->band_chan[idx].band;
		scan->band_chan[idx].channel = proto->band_chan[idx].channel;
	}
}

static void disconn_event_to_proto(const struct wifi_status *status,
				   ros_wifi_v1_WifiMgmtNotification *proto)
{
	ros_wifi_v1_DisconnectResultEvent evt = ros_wifi_v1_DisconnectResultEvent_init_default;
	evt.reason_code = status->status;
	proto->has_event = true;
	proto->event.event_data.disconnect_result_event = evt;
	proto->event.which_event_data = ros_wifi_v1_WifiEvent_disconnect_result_event_tag;
}

static void conn_event_to_proto(const struct wifi_status *status,
				ros_wifi_v1_WifiMgmtNotification *proto)
{
	ros_wifi_v1_ConnectResultEvent evt = ros_wifi_v1_ConnectResultEvent_init_default;
	evt.status = status->status;
	proto->has_event = true;
	proto->event.event_data.connect_result_event = evt;
	proto->event.which_event_data = ros_wifi_v1_WifiEvent_connect_result_event_tag;
}

static void ap_enable_event_to_proto(const struct wifi_status *status,
				     ros_wifi_v1_WifiMgmtNotification *proto)
{
	ros_wifi_v1_ApEnableResultEvent evt = ros_wifi_v1_ApEnableResultEvent_init_zero;
	evt.status = status->status;
	proto->has_event = true;
	proto->event.event_data.ap_enable_result_event = evt;
	proto->event.which_event_data = ros_wifi_v1_WifiEvent_ap_enable_result_event_tag;
}

static void ap_disable_event_to_proto(const struct wifi_status *status,
				      ros_wifi_v1_WifiMgmtNotification *proto)
{
	ros_wifi_v1_ApDisableResultEvent evt = ros_wifi_v1_ApDisableResultEvent_init_zero;
	evt.status = status->status;
	proto->has_event = true;
	proto->event.event_data.ap_disable_result_event = evt;
	proto->event.which_event_data = ros_wifi_v1_WifiEvent_ap_disable_result_event_tag;
}

static void ap_sta_connected_event_to_proto(const struct wifi_ap_sta_info *sta,
					    ros_wifi_v1_WifiMgmtNotification *proto)
{
	ros_wifi_v1_ApStaConnectedEvent evt = ros_wifi_v1_ApStaConnectedEvent_init_zero;
	uint8_t len = sta->mac_length;
	if (len > WIFI_MAC_ADDR_LEN) {
		len = WIFI_MAC_ADDR_LEN;
	}
	evt.sta_mac_address.size = len;
	memcpy(evt.sta_mac_address.bytes, sta->mac, len);
	proto->has_event = true;
	proto->event.event_data.ap_sta_connected_event = evt;
	proto->event.which_event_data = ros_wifi_v1_WifiEvent_ap_sta_connected_event_tag;
}

static void ap_sta_disconnected_event_to_proto(const struct wifi_ap_sta_info *sta,
					       ros_wifi_v1_WifiMgmtNotification *proto)
{
	ros_wifi_v1_ApStaDisconnectedEvent evt = ros_wifi_v1_ApStaDisconnectedEvent_init_zero;
	uint8_t len = sta->mac_length;
	if (len > WIFI_MAC_ADDR_LEN) {
		len = WIFI_MAC_ADDR_LEN;
	}
	evt.sta_mac_address.size = len;
	memcpy(evt.sta_mac_address.bytes, sta->mac, len);
	proto->has_event = true;
	proto->event.event_data.ap_sta_disconnected_event = evt;
	proto->event.which_event_data = ros_wifi_v1_WifiEvent_ap_sta_disconnected_event_tag;
}

static void scan_event_to_proto(const struct wifi_scan_result *result,
				ros_wifi_v1_WifiMgmtNotification *proto)
{
	ros_wifi_v1_WifiScanResult evt = ros_wifi_v1_WifiScanResult_init_default;
	evt.ssid.size = result->ssid_length;
	memcpy(evt.ssid.bytes, result->ssid, result->ssid_length);
	memcpy(evt.bssid.bytes, result->mac, WIFI_MAC_ADDR_LEN);
	evt.bssid.size = WIFI_MAC_ADDR_LEN;
	evt.rssi = result->rssi;
	evt.band = result->band;
	evt.channel = result->channel;
	evt.security = result->security;
	evt.wpa3_enterprise_type = result->wpa3_ent_type;
	evt.mfp = result->mfp;

	proto->has_event = true;
	proto->event.event_data.scan_result_event = evt;
	proto->event.which_event_data = ros_wifi_v1_WifiEvent_scan_result_event_tag;
}

static void resp_set_status(int err, ros_wifi_v1_WifiMgmtResponse *resp, const char *err_msg)
{
	resp->has_status = true;
	resp->status.success = (err == 0) ? true : false;
	resp->status.error_code = err;

	/* proto3 + nanopb: string presence is not tracked with a has_* flag.
	 * Encode error_message only when non-empty.
	 */
	resp->status.error_message[0] = '\0';
	if (err_msg && err_msg[0]) {
		strlcpy(resp->status.error_message, err_msg,
			sizeof(resp->status.error_message));
	}
}

static bool encode_wifi_mgmt_evt(ros_wifi_v1_WifiMgmtUplink *evt, uint8_t *buf, uint16_t buf_size,
				 uint16_t *enc_len)
{
	pb_ostream_t stream = pb_ostream_from_buffer(buf, buf_size);

	bool res = pb_encode(&stream, ros_wifi_v1_WifiMgmtUplink_fields, evt);
	if (res == false) {
		LOG_ERR("Failed to encode mgmt event %s", PB_GET_ERROR(&stream));
		return false;
	}
	*enc_len = stream.bytes_written;
	return true;
}

static void log_send_uplink_drop(link_result_t res)
{
	static uint32_t last_log_ms;
	static uint32_t suppressed_logs;
	uint32_t now = k_uptime_get_32();

	if ((last_log_ms == 0U) || ((now - last_log_ms) >= SEND_UPLINK_WARN_INTERVAL_MS)) {
		if (suppressed_logs > 0U) {
			LOG_WRN("send_uplink: dropped %u additional notification(s) while link was busy",
				(unsigned int)suppressed_logs);
			suppressed_logs = 0U;
		}

		LOG_WRN("send_uplink: link_send failed (%d), dropping", (int)res);
		last_log_ms = now;
		return;
	}

	suppressed_logs++;
}

static void reset_scan_uplink_suppression(void)
{
	atomic_set(&scan_uplink_suppressed, 0);
	atomic_set(&scan_uplink_dropped, 0);
}

static bool decode_wifi_mgmt_req(ros_wifi_v1_WifiMgmtRequest *evt, uint8_t *buf, uint16_t buf_size)
{
	pb_istream_t stream = pb_istream_from_buffer(buf, buf_size);

	LOG_DBG("decode sizeof(WifiMgmtRequest)=%u", (unsigned)sizeof(ros_wifi_v1_WifiMgmtRequest));

	if (!pb_decode(&stream, ros_wifi_v1_WifiMgmtRequest_fields, evt)) {
		LOG_ERR("Failed to decode mgmt message %s", PB_GET_ERROR(&stream));
		return false;
	}
	return true;
}

static link_result_t send_uplink_result(ros_wifi_v1_WifiMgmtUplink *uplink)
{
	rcp_buffer_hdl_t *msg_buffer = get_buffer(RCP_BUF_SIZE);
	if (!msg_buffer) {
		return RCP_LNK_ERR_TX_NO_RESOURCES;
	}

	if (!encode_wifi_mgmt_evt(uplink, msg_buffer->data_ptr, msg_buffer->max_size,
				  &msg_buffer->size)) {
		/* encode failed -> release local buffer */
		release_buffer(&msg_buffer);
		return RCP_LNK_ERROR;
	}

	link_result_t res = link_send(RCP_STREAM_WIFI_MGMT, msg_buffer);
	if (res != RCP_LNK_SUCCESS) {
		/* link_send() did not take ownership on failure */
		release_buffer(&msg_buffer);
	}

	return res;
}

static void send_uplink(ros_wifi_v1_WifiMgmtUplink *uplink)
{
	link_result_t res = send_uplink_result(uplink);

	if (res != RCP_LNK_SUCCESS) {
		log_send_uplink_drop(res);
	}
}

#ifdef CONFIG_PM_DEVICE
extern bool spi_is_ext_wakeup(void);
extern void spi_clear_ext_wakeup_flag(void);

static struct pm_notifier rcp_pm_notifier;

static void rcp_pm_state_entry(enum pm_state state)
{
	ARG_UNUSED(state);
	if (spi_is_ext_wakeup()) {
		spi_clear_ext_wakeup_flag();
	}
}

static void rcp_pm_state_exit(enum pm_state state)
{
	const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(qcspi));
	struct net_if *iface = net_if_get_wifi_sta();

	switch (state) {
	case PM_STATE_SUSPEND_TO_RAM:
	case PM_STATE_SOFT_OFF:
		if (spi_is_ext_wakeup()) {
			if (device_is_ready(spi_dev)) {
				if (iface) {
					struct qcom_wifi_pm_bmps_params bp = { .enable = 0u };
					(void)net_mgmt(NET_REQUEST_WIFI_PM_QCOM_SET_BMPS_ENABLE,
						       iface, &bp, sizeof(bp));
					if (!pm_policy_state_lock_is_active(PM_STATE_SUSPEND_TO_RAM,
									    PM_ALL_SUBSTATES)) {
						pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM,
									 PM_ALL_SUBSTATES);
					}
				}
			}
			spi_clear_ext_wakeup_flag();
		}
		break;
	default:
		break;
	}
}

static int rcp_pm_notifier_init(void)
{
	rcp_pm_notifier.state_entry = rcp_pm_state_entry;
	rcp_pm_notifier.state_exit = rcp_pm_state_exit;
	pm_notifier_register(&rcp_pm_notifier);
	return 0;
}
SYS_INIT(rcp_pm_notifier_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);
#endif /* CONFIG_PM_DEVICE */

static uint32_t iface_ptr_to_index(struct net_if *iface)
{
	struct net_if *sta = net_if_get_wifi_sta();
	struct net_if *ap = net_if_get_wifi_sap();

	if (iface == ap) {
		return PROXY_WIFI_IFACE_AP;
	}
	if ((iface == sta) || (iface == NULL)) {
		return PROXY_WIFI_IFACE_STA;
	}

	LOG_WRN("Unknown iface pointer %p, defaulting to STA index", iface);
	return PROXY_WIFI_IFACE_STA;
}

static const char *wifi_mgmt_event_name(uint64_t mgmt_event)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		return "CONNECT_RESULT";
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		return "DISCONNECT_RESULT";
	case NET_EVENT_WIFI_SCAN_RESULT:
		return "SCAN_RESULT";
	case NET_EVENT_WIFI_SCAN_DONE:
		return "SCAN_DONE";
	case NET_EVENT_WIFI_AP_ENABLE_RESULT:
		return "AP_ENABLE_RESULT";
	case NET_EVENT_WIFI_AP_DISABLE_RESULT:
		return "AP_DISABLE_RESULT";
	case NET_EVENT_WIFI_AP_STA_CONNECTED:
		return "AP_STA_CONNECTED";
	case NET_EVENT_WIFI_AP_STA_DISCONNECTED:
		return "AP_STA_DISCONNECTED";
	default:
		return "UNKNOWN";
	}
}

static void handle_wifi_disconnect_event(struct net_mgmt_event_callback *cb, uint32_t iface_index)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;
	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtNotification notif = ros_wifi_v1_WifiMgmtNotification_init_zero;

	LOG_INF("WiFi disconnect event: iface=%u status=%d", iface_index,
		status ? status->status : -1);

	disconn_event_to_proto(status, &notif);
	notif.event.iface_index = iface_index;

	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_notification_tag;
	uplink.payload.notification = notif;

	send_uplink(&uplink);
}

static void handle_wifi_connect_event(struct net_mgmt_event_callback *cb, uint32_t iface_index)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;
	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtNotification notif = ros_wifi_v1_WifiMgmtNotification_init_zero;

	LOG_INF("WiFi connect event: iface=%u status=%d", iface_index,
		status ? status->status : -1);

	conn_event_to_proto(status, &notif);
	notif.event.iface_index = iface_index;
	uplink.payload.notification = notif;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_notification_tag;

	send_uplink(&uplink);
}

static void handle_wifi_ap_enable_event(struct net_mgmt_event_callback *cb, uint32_t iface_index)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;
	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtNotification notif = ros_wifi_v1_WifiMgmtNotification_init_zero;

	LOG_INF("SoftAP enable result: iface=%u status=%d", iface_index,
		status ? status->status : -1);

	ap_enable_event_to_proto(status, &notif);
	notif.event.iface_index = iface_index;
	uplink.payload.notification = notif;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_notification_tag;

	send_uplink(&uplink);
}

static void handle_wifi_ap_disable_event(struct net_mgmt_event_callback *cb, uint32_t iface_index)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;
	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtNotification notif = ros_wifi_v1_WifiMgmtNotification_init_zero;

	LOG_INF("SoftAP disable result: iface=%u status=%d", iface_index,
		status ? status->status : -1);

	ap_disable_event_to_proto(status, &notif);
	notif.event.iface_index = iface_index;
	uplink.payload.notification = notif;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_notification_tag;

	send_uplink(&uplink);
}

static void handle_wifi_ap_sta_connected_event(struct net_mgmt_event_callback *cb,
					       uint32_t iface_index)
{
	const struct wifi_ap_sta_info *sta = (const struct wifi_ap_sta_info *)cb->info;
	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtNotification notif = ros_wifi_v1_WifiMgmtNotification_init_zero;

	if (sta && sta->mac_length >= WIFI_MAC_ADDR_LEN) {
		LOG_INF("SoftAP STA connected: iface=%u mac=%02x:%02x:%02x:%02x:%02x:%02x",
			iface_index, sta->mac[0], sta->mac[1], sta->mac[2], sta->mac[3],
			sta->mac[4], sta->mac[5]);
	} else {
		LOG_INF("SoftAP STA connected: iface=%u mac_len=%u", iface_index,
			sta ? sta->mac_length : 0U);
	}

	ap_sta_connected_event_to_proto(sta, &notif);
	notif.event.iface_index = iface_index;
	uplink.payload.notification = notif;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_notification_tag;

	send_uplink(&uplink);
}

static void handle_wifi_ap_sta_disconnected_event(struct net_mgmt_event_callback *cb,
						  uint32_t iface_index)
{
	const struct wifi_ap_sta_info *sta = (const struct wifi_ap_sta_info *)cb->info;
	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtNotification notif = ros_wifi_v1_WifiMgmtNotification_init_zero;

	if (sta && sta->mac_length >= WIFI_MAC_ADDR_LEN) {
		LOG_INF("SoftAP STA disconnected: iface=%u mac=%02x:%02x:%02x:%02x:%02x:%02x",
			iface_index, sta->mac[0], sta->mac[1], sta->mac[2], sta->mac[3],
			sta->mac[4], sta->mac[5]);
	} else {
		LOG_INF("SoftAP STA disconnected: iface=%u mac_len=%u", iface_index,
			sta ? sta->mac_length : 0U);
	}

	ap_sta_disconnected_event_to_proto(sta, &notif);
	notif.event.iface_index = iface_index;
	uplink.payload.notification = notif;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_notification_tag;

	send_uplink(&uplink);
}

static void handle_wifi_scan_result(struct net_mgmt_event_callback *cb, uint32_t iface_index)
{
	if (!atomic_get(&scan_requested)) {
		LOG_DBG("Ignoring internal scan result (host did not request scan)");
		return;
	}

	if (atomic_get(&scan_uplink_suppressed)) {
		atomic_inc(&scan_uplink_dropped);
		return;
	}

	const struct wifi_scan_result *result = (const struct wifi_scan_result *)cb->info;
	LOG_DBG("scan_result: ssid=%.*s rssi=%d ch=%d", result->ssid_length, result->ssid,
		result->rssi, result->channel);
	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtNotification notif = ros_wifi_v1_WifiMgmtNotification_init_zero;
	link_result_t res;

	scan_event_to_proto(result, &notif);
	notif.event.iface_index = iface_index;
	uplink.payload.notification = notif;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_notification_tag;

	res = send_uplink_result(&uplink);
	if (res != RCP_LNK_SUCCESS) {
		atomic_inc(&scan_uplink_dropped);
		if (atomic_cas(&scan_uplink_suppressed, 0, 1)) {
			LOG_WRN("Scan result uplink saturated, suppressing remaining scan notifications until SCAN_DONE");
		}
	}
}

static void handle_wifi_scan_done(struct net_mgmt_event_callback *cb, uint32_t iface_index)
{
	uint32_t dropped_scan_events = (uint32_t)atomic_get(&scan_uplink_dropped);

	ARG_UNUSED(cb);

	if (!atomic_get(&scan_requested)) {
		LOG_DBG("Ignoring internal scan done (host did not request scan)");
		return;
	}

	LOG_DBG("Scan Done");
	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtNotification notif = ros_wifi_v1_WifiMgmtNotification_init_zero;

	ros_wifi_v1_ScanDoneEvent evt = ros_wifi_v1_ScanDoneEvent_init_default;
	evt.status = 0; // OK
	notif.has_event = true;
	notif.event.iface_index = iface_index;
	notif.event.event_data.scan_done_event = evt;
	notif.event.which_event_data = ros_wifi_v1_WifiEvent_scan_done_event_tag;
	uplink.payload.notification = notif;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_notification_tag;

	/* Retry until the ring has a free slot — the host is still consuming
	 * the burst of scan results.  scan_done is the last event so blocking
	 * here is safe (no more net_mgmt SCAN_RESULT events will arrive). */
	link_result_t res;
	int retries = 0;

	do {
		res = send_uplink_result(&uplink);
		if (res != RCP_LNK_SUCCESS) {
			k_msleep(20);
			retries++;
		}
	} while (res != RCP_LNK_SUCCESS && retries < 50);

	if (res == RCP_LNK_SUCCESS) {
		LOG_INF("scan_done: uplink sent (retries=%d), suppressed=%u dropped=%u",
			retries,
			(unsigned int)atomic_get(&scan_uplink_suppressed),
			(unsigned int)dropped_scan_events);
	} else {
		LOG_WRN("scan_done: failed to send after %d retries, dropping", retries);
	}
	reset_scan_uplink_suppression();
	/* Scan request completed */
	atomic_set(&scan_requested, 0);
}

static void handle_connect_request(ros_wifi_v1_WifiConnectRequest *proto)
{
	struct net_if *iface = net_if_get_wifi_sta();
	const char *err_msg = NULL;
	static struct conn_req_storage store PROXY_WIFI_RAM_SECTION;

	if (!proto) {
		LOG_ERR("Received NULL WiFi connect request");
		ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
		ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

		resp_set_status(-EINVAL, &resp, "Missing connect request");
		uplink.payload.response = resp;
		uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;
		send_uplink(&uplink);
		return;
	}

	if (!iface) {
		LOG_ERR("No WiFi STA interface (net_if_get_wifi_sta returned NULL)");
		ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
		ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

		resp_set_status(-ENODEV, &resp, "No WiFi interface");
		uplink.payload.response = resp;
		uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;
		send_uplink(&uplink);
		return;
	}

	LOG_INF("Received WiFi connection request (iface_index=%u). SSID: %.*s", proto->iface_index,
		proto->ssid.size, proto->ssid.bytes);

	int err = conn_req_from_proto(proto, &store, &err_msg);
	if (err == 0) {
		err = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &store.params,
			       sizeof(store.params));
		if (err) {
			LOG_ERR("NET_REQUEST_WIFI_CONNECT failed: %d", err);
		}
	} else {
		LOG_ERR("Invalid connect params: %d", err);
	}

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

	resp_set_status(err, &resp, err_msg);

	uplink.payload.response = resp;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;

	send_uplink(&uplink);
}

static void handle_ap_enable_request(ros_wifi_v1_WifiApEnableRequest *proto)
{
	struct net_if *sap_iface = net_if_get_wifi_sap();
	struct net_if *sta_iface = net_if_get_wifi_sta();

	if (!proto->has_params) {
		LOG_WRN("AP enable request missing params");
		return;
	}

	LOG_INF("Received SoftAP enable request. SSID: %.*s", proto->params.ssid.size,
		proto->params.ssid.bytes);
	LOG_INF("SoftAP req iface=%u sec=%u ch=%u psk_len=%u hidden=%u sap_if=%p",
		(unsigned)proto->iface_index, (unsigned)proto->params.security_type,
		(unsigned)proto->params.channel, (unsigned)proto->params.psk.size,
		(unsigned)proto->params.hidden_ssid, sap_iface);

	/*
	 * Keep this on the stack to avoid consuming .bss (QCC74x RAM is tight).
	 * net_mgmt() consumes the params synchronously for the request.
	 */
	struct wifi_connect_req_params ap = {0};
	uint8_t requested_channel = (uint8_t)proto->params.channel;
	uint8_t effective_channel = requested_channel;
	bool channel_followed_sta = false;
	ap.ssid = proto->params.ssid.bytes;
	ap.ssid_length = proto->params.ssid.size;
	ap.psk = proto->params.psk.bytes;
	ap.psk_length = proto->params.psk.size;
	ap.security = (enum wifi_security_type)proto->params.security_type;
	ap.band = (enum wifi_frequency_bands)proto->params.band;
	ap.channel = requested_channel;
	ap.ignore_broadcast_ssid = proto->params.hidden_ssid ? 1U : 0U;

	/* Default to 20MHz bandwidth if the driver cares. */
	ap.bandwidth = WIFI_FREQ_BANDWIDTH_20MHZ;

	/* In STA+AP concurrency, follow active STA channel whenever possible. */
	if (sta_iface) {
		struct wifi_iface_status sta_status = {0};
		int sta_err = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, sta_iface, &sta_status,
				       sizeof(sta_status));

		LOG_INF("SoftAP channel decision: sta_status err=%d state=%d band=%d "
			"sta_channel=%u requested=%u",
			sta_err, sta_status.state, sta_status.band, sta_status.channel,
			requested_channel);

		if ((sta_err == 0) && (sta_status.state >= WIFI_STATE_ASSOCIATED) &&
		    (sta_status.state <= WIFI_STATE_COMPLETED) && (sta_status.channel != 0U) &&
		    (sta_status.channel != WIFI_CHANNEL_ANY) &&
		    wifi_utils_validate_chan((uint8_t)ap.band, sta_status.channel)) {
			effective_channel = (uint8_t)sta_status.channel;
			channel_followed_sta = true;
		}
	} else {
		LOG_WRN("SoftAP channel decision: STA iface not available, using requested "
			"channel");
	}

	ap.channel = effective_channel;
	LOG_INF("SoftAP effective channel: requested=%u effective=%u reason=%s", requested_channel,
		effective_channel, channel_followed_sta ? "follow_sta" : "requested_or_fallback");

	int err = sap_iface ? net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, sap_iface, &ap, sizeof(ap))
			    : -ENODEV;
	LOG_INF("NET_REQUEST_WIFI_AP_ENABLE err=%d on sap_if=%p", err, sap_iface);

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

	resp_set_status(err, &resp, NULL);
	uplink.payload.response = resp;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;

	send_uplink(&uplink);
}

static void handle_ap_disable_request(ros_wifi_v1_WifiApDisableRequest *proto)
{
	struct net_if *sap_iface = net_if_get_wifi_sap();

	ARG_UNUSED(proto);
	LOG_INF("Received SoftAP disable request");

	int err = sap_iface ? net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, sap_iface, NULL, 0) : -ENODEV;
	LOG_INF("NET_REQUEST_WIFI_AP_DISABLE err=%d on sap_if=%p", err, sap_iface);

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

	resp_set_status(err, &resp, NULL);
	uplink.payload.response = resp;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;

	send_uplink(&uplink);
}

static void handle_disconn_request(void)
{
	LOG_INF("Received WiFi disconnect request");
	int err = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, net_if_get_wifi_sta(), NULL, 0);

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

	resp_set_status(err, &resp, NULL);

	uplink.payload.response = resp;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;

	send_uplink(&uplink);
}

/* Deauth an associated STA. Fire-and-forget: no response is sent back (the host
 * .del_station path does not wait), matching qapi_WLAN_AP_Disconnect_Station
 * which just emits a deauth frame. */
static void handle_ap_sta_disconnect_request(ros_wifi_v1_WifiApStaDisconnectRequest *proto)
{
	struct net_if *sap_iface = net_if_get_wifi_sap();

	if (!sap_iface || proto->sta_mac_address.size != WIFI_MAC_ADDR_LEN) {
		LOG_WRN("AP STA disconnect: bad iface or mac len=%u",
			(unsigned)proto->sta_mac_address.size);
		return;
	}

	int err = net_mgmt(NET_REQUEST_WIFI_AP_STA_DISCONNECT, sap_iface,
			   proto->sta_mac_address.bytes, proto->sta_mac_address.size);
	LOG_INF("AP STA disconnect %02x:%02x:%02x:%02x:%02x:%02x err=%d",
		proto->sta_mac_address.bytes[0], proto->sta_mac_address.bytes[1],
		proto->sta_mac_address.bytes[2], proto->sta_mac_address.bytes[3],
		proto->sta_mac_address.bytes[4], proto->sta_mac_address.bytes[5], err);
}

static void handle_get_l2_interface_request(ros_wifi_v1_RequestInterfaces *proto)
{
	LOG_INF("Received get L2 interface request type %d", proto->net_if_type);

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;
	resp.which_payload = ros_wifi_v1_WifiMgmtResponse_l2_interfaces_tag;
	resp.has_status = true;
	resp.status.success = true;

	ros_wifi_v1_RequestInterfacesResponse l2_addr_resp =
		ros_wifi_v1_RequestInterfacesResponse_init_zero;

	struct net_if *sta_iface = net_if_get_wifi_sta();
	struct net_if *ap_iface = net_if_get_wifi_sap();
	uint8_t count = 0;

	if (sta_iface) {
		struct net_linkaddr *link_addr_ptr = net_if_get_link_addr(sta_iface);
		l2_addr_resp.net_ifs[count].index = PROXY_WIFI_IFACE_STA;
		l2_addr_resp.net_ifs[count].net_if_type = ros_wifi_v1_NetIfType_NET_IF_TYPE_WIFI;
		l2_addr_resp.net_ifs[count].mac_address.size = WIFI_MAC_ADDR_LEN;
		memcpy(l2_addr_resp.net_ifs[count].mac_address.bytes, link_addr_ptr->addr,
		       WIFI_MAC_ADDR_LEN);
		l2_addr_resp.net_ifs[count].mtu = NET_ETH_MTU;
		LOG_INF("L2 STA idx=%u mtu=%u mac=%02x:%02x:%02x:%02x:%02x:%02x",
			(unsigned)l2_addr_resp.net_ifs[count].index,
			(unsigned)l2_addr_resp.net_ifs[count].mtu, link_addr_ptr->addr[0],
			link_addr_ptr->addr[1], link_addr_ptr->addr[2], link_addr_ptr->addr[3],
			link_addr_ptr->addr[4], link_addr_ptr->addr[5]);
		count++;
	}

	if (ap_iface) {
		struct net_linkaddr *link_addr_ptr = net_if_get_link_addr(ap_iface);
		l2_addr_resp.net_ifs[count].index = PROXY_WIFI_IFACE_AP;
		l2_addr_resp.net_ifs[count].net_if_type = ros_wifi_v1_NetIfType_NET_IF_TYPE_WIFI;
		l2_addr_resp.net_ifs[count].mac_address.size = WIFI_MAC_ADDR_LEN;
		memcpy(l2_addr_resp.net_ifs[count].mac_address.bytes, link_addr_ptr->addr,
		       WIFI_MAC_ADDR_LEN);
		l2_addr_resp.net_ifs[count].mtu = NET_ETH_MTU;
		LOG_INF("L2 AP  idx=%u mtu=%u mac=%02x:%02x:%02x:%02x:%02x:%02x",
			(unsigned)l2_addr_resp.net_ifs[count].index,
			(unsigned)l2_addr_resp.net_ifs[count].mtu, link_addr_ptr->addr[0],
			link_addr_ptr->addr[1], link_addr_ptr->addr[2], link_addr_ptr->addr[3],
			link_addr_ptr->addr[4], link_addr_ptr->addr[5]);
		count++;
	}

	l2_addr_resp.net_ifs_count = count;
	resp.status.success = (count > 0U);
	resp.payload.l2_interfaces = l2_addr_resp;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;
	uplink.payload.response = resp;

	send_uplink(&uplink);
}

static void handle_get_iface_status_request(ros_wifi_v1_GetInterfaceStatusRequest *proto)
{
	uint32_t idx = proto ? proto->iface_index : 0;
	LOG_INF("Received get iface status request (iface_index=%u)", idx);

	struct wifi_iface_status status = {0};
	struct net_if *iface =
		(idx == PROXY_WIFI_IFACE_AP) ? net_if_get_wifi_sap() : net_if_get_wifi_sta();
	int err;

	if (!iface) {
		err = -ENODEV;
		LOG_WRN("No WiFi STA interface (net_if_get_wifi_sta returned NULL)");
	} else {
		err = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status,
			       sizeof(struct wifi_iface_status));
		LOG_INF("NET_REQUEST_WIFI_IFACE_STATUS err=%d state=%d mode=%d link_mode=%d", err,
			status.state, status.iface_mode, status.link_mode);
	}

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

	resp_set_status(err, &resp, NULL);

	if (err == 0) {
		ros_wifi_v1_GetInterfaceStatusResponse iface_status_resp =
			ros_wifi_v1_GetInterfaceStatusResponse_init_zero;
		ros_wifi_v1_WifiInterfaceStatus st = ros_wifi_v1_WifiInterfaceStatus_init_zero;

		/* Map Zephyr status -> protobuf status (proto enums aligned with Zephyr) */
		st.state = (ros_wifi_v1_WifiInterfaceState)status.state;
		st.mode = (ros_wifi_v1_WifiMode)status.iface_mode;
		st.link_mode = (ros_wifi_v1_WifiLinkMode)status.link_mode;

		/* SSID */
		size_t ssid_len = strnlen(status.ssid, sizeof(status.ssid));
		st.ssid.size = ssid_len;
		memcpy(st.ssid.bytes, status.ssid, ssid_len);

		/* BSSID */
		st.bssid.size = WIFI_MAC_ADDR_LEN;
		memcpy(st.bssid.bytes, status.bssid, WIFI_MAC_ADDR_LEN);

		st.band = (ros_wifi_v1_WifiBand)status.band;
		st.channel = status.channel;
		st.security = (ros_wifi_v1_WifiSecurityType)status.security;
		st.wpa3_enterprise_type = (ros_wifi_v1_WifiWpa3EnterpriseType)status.wpa3_ent_type;
		st.mfp = (ros_wifi_v1_WifiMfpOptions)status.mfp;
		st.rssi = status.rssi;
		st.dtim_period = status.dtim_period;
		st.beacon_interval = status.beacon_interval;
		st.twt_capable = status.twt_capable;

		/* proto carries kbps; Zephyr carries Mbps */
		st.tx_bitrate_kbps = (uint32_t)status.current_phy_tx_rate * 1000U;

		iface_status_resp.has_status = true;
		iface_status_resp.status = st;
		resp.which_payload = ros_wifi_v1_WifiMgmtResponse_iface_status_tag;
		resp.payload.iface_status = iface_status_resp;
	}

	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;
	uplink.payload.response = resp;

	send_uplink(&uplink);
}

static void handle_scan_request(ros_wifi_v1_WifiScanRequest *proto)
{
	static struct wifi_scan_params scan = {};

	scan_req_from_proto(&proto->params, &scan);

	/* Only forward scan results/done to host when host requested scan. */
	atomic_set(&scan_requested, 1);
	reset_scan_uplink_suppression();

	int err = net_mgmt(NET_REQUEST_WIFI_SCAN, net_if_get_wifi_sta(), &scan, sizeof(scan));

	if (err != 0) {
		LOG_WRN("Failed to run scan err=%d", err);
		atomic_set(&scan_requested, 0);
	}

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

	resp_set_status(err, &resp, NULL);

	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;
	uplink.payload.response = resp;

	send_uplink(&uplink);
}

static void handle_set_reg_domain(ros_wifi_v1_SetRegulatoryDomainRequest *proto)
{
	static struct wifi_reg_domain domain_data = {};

	struct wifi_reg_chan_info chan_info[MAX_REG_CHAN_NUM];

	domain_data.oper = WIFI_MGMT_SET;
	domain_data.force = proto->reg_domain.force_update;

	memcpy(domain_data.country_code, proto->reg_domain.country_code.bytes,
	       WIFI_COUNTRY_CODE_LEN);

	if (proto->reg_domain.channel_info_count <= MAX_REG_CHAN_NUM) {
		domain_data.num_channels = proto->reg_domain.channel_info_count;
		for (int i = 0; i < domain_data.num_channels; i++) {
			chan_info[i].center_frequency =
				(unsigned short)proto->reg_domain.channel_info[i]
					.center_frequency_mhz;
			chan_info[i].max_power =
				(unsigned short)proto->reg_domain.channel_info[i].max_power_dbm;
			chan_info[i].supported =
				(unsigned short)proto->reg_domain.channel_info[i].supported;
			chan_info[i].passive_only =
				(unsigned short)proto->reg_domain.channel_info[i].passive_only;
			chan_info[i].dfs = (unsigned short)proto->reg_domain.channel_info[i].dfs;
		}
	}

	LOG_INF("Setting Regulatory Domain to %s with %d Channels", domain_data.country_code,
		domain_data.num_channels);

	int err = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, net_if_get_wifi_sta(), &domain_data,
			   sizeof(domain_data));

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

	if (err != 0) {
		LOG_WRN("Failed to set Reg Domain err=%d", err);
	}
	resp_set_status(err, &resp, NULL);

	uplink.payload.response = resp;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;

	send_uplink(&uplink);
}

static void handle_set_rts_threshold(ros_wifi_v1_SetRtsThresholdRequest *proto)
{
	struct net_if *iface = net_if_get_wifi_sta();
	struct qcom_wifi_set_rts_cts_params p;
	uint32_t thr = proto->rts_threshold;
	int err;

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

	/* QCC730 only supports RTS/CTS on/off. Degrade the cfg80211 threshold:
	 * off (0xFFFFFFFF) or >= dot11RTSThreshold max (2347) means "never RTS"
	 * => disable; any smaller threshold => enable RTS protection. */
	p.enable = (thr == 0xFFFFFFFFu || thr >= 2347u) ? 0u : 1u;

	err = iface ? net_mgmt(NET_REQUEST_WIFI_QCOM_SET_RTS_CTS, iface, &p, sizeof(p))
		    : -ENODEV;
	LOG_INF("set_rts_threshold: thr=%u -> enable=%u err=%d",
		(unsigned)thr, (unsigned)p.enable, err);

	resp_set_status(err, &resp, NULL);
	uplink.payload.response = resp;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;
	send_uplink(&uplink);
}

static void handle_set_tx_power(ros_wifi_v1_SetTxPowerRequest *proto)
{
	struct net_if *iface = net_if_get_wifi_sta();
	struct qcom_wifi_set_tx_power_params p = {0};
	int err;

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

	p.txpower = (uint8_t)proto->txpower;
	p.policy  = (uint8_t)proto->policy;
	err = iface ? net_mgmt(NET_REQUEST_WIFI_QCOM_SET_TX_POWER, iface, &p, sizeof(p))
		    : -ENODEV;
	LOG_INF("set_tx_power: txpower=%u policy=%u err=%d",
		(unsigned)p.txpower, (unsigned)p.policy, err);

	resp_set_status(err, &resp, NULL);
	uplink.payload.response = resp;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;
	send_uplink(&uplink);
}

static void handle_get_tx_power(ros_wifi_v1_GetTxPowerRequest *proto)
{
	struct net_if *iface = net_if_get_wifi_sta();
	struct qcom_wifi_get_tx_power_params p = {0};
	int err;

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

	ARG_UNUSED(proto);

	err = iface ? net_mgmt(NET_REQUEST_WIFI_QCOM_GET_TX_POWER, iface, &p, sizeof(p))
		    : -ENODEV;

	resp_set_status(err, &resp, NULL);
	if (err == 0) {
		resp.which_payload = ros_wifi_v1_WifiMgmtResponse_tx_power_tag;
		resp.payload.tx_power.reg_power    = p.reg_power;
		resp.payload.tx_power.ctl_power    = p.ctl_power;
		resp.payload.tx_power.target_power = p.target_power;
		resp.payload.tx_power.real_power   = p.real_power;
	}
	uplink.payload.response = resp;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;
	send_uplink(&uplink);
}

static void handle_set_ps_config_request(ros_wifi_v1_SetPowerSaveConfigRequest *proto)
{
	struct net_if *iface = net_if_get_wifi_sta();
	const struct device *wifi_dev = iface ? net_if_get_device(iface) : NULL;
	int err = 0;
	uint8_t enable;

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

	if (!iface) {
		err = -ENODEV;
		goto out;
	}

	/* proto WifiPsState: DISABLE=0, ENABLE=1 -> our on/off */
	enable = (proto->config.ps_settings.enabled ==
		  ros_wifi_v1_WifiPsState_WIFI_PS_STATE_ENABLE) ? 1u : 0u;

	if (enable) {
		/* Step 1: BMPS idle timeout (iw timeout arg, ms). >0 => set; else firmware default. */
		if (proto->config.ps_settings.timeout_ms > 0) {
			struct wifi_ps_params tparams = {0};
			tparams.type = WIFI_PS_PARAM_TIMEOUT;
			tparams.timeout_ms = proto->config.ps_settings.timeout_ms;
			(void)net_mgmt(NET_REQUEST_WIFI_PS, iface, &tparams, sizeof(tparams));
		}
		/* Step 2: RX filter on (callback fixed firmware-side). */
		{
			struct qcom_wifi_pm_rx_filter_params rxf = {0};
			rxf.enable = 1u;
			rxf.bmps_rx_filter_cb = NULL; /* firmware default rules */
			(void)net_mgmt(NET_REQUEST_WIFI_PM_QCOM_SET_RX_FILTER_IN_BMPS,
				       iface, &rxf, sizeof(rxf));
		}
		/* Step 3: enable BMPS. */
		{
			struct qcom_wifi_pm_bmps_params bp = { .enable = 1u };
			err = net_mgmt(NET_REQUEST_WIFI_PM_QCOM_SET_BMPS_ENABLE,
				       iface, &bp, sizeof(bp));
		}
		/* Step 4: release S2RAM lock (only if held — the wakeup notifier or a
		 * prior disable may have taken it); Step 5: clear wifi busy so S2RAM
		 * can proceed. */
		if (err == 0) {
			if (pm_policy_state_lock_is_active(PM_STATE_SUSPEND_TO_RAM,
							   PM_ALL_SUBSTATES)) {
				pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_RAM,
							 PM_ALL_SUBSTATES);
			}
			if (wifi_dev && pm_device_is_busy(wifi_dev)) {
				pm_device_busy_clear(wifi_dev);
			}
		}
	} else {
		/* Disable BMPS, then pin S2RAM off (mirror qat_ps_exit). Guard the
		 * lock get so we don't stack a second lock if the wakeup notifier
		 * already took it. */
		struct qcom_wifi_pm_bmps_params bp = { .enable = 0u };
		err = net_mgmt(NET_REQUEST_WIFI_PM_QCOM_SET_BMPS_ENABLE,
			       iface, &bp, sizeof(bp));
		if (err == 0 &&
		    !pm_policy_state_lock_is_active(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES)) {
			pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
		}
	}

	LOG_INF("set_ps_config: enable=%u timeout_ms=%d err=%d",
		(unsigned)enable, (int)proto->config.ps_settings.timeout_ms, err);
out:
	resp_set_status(err, &resp, NULL);
	uplink.payload.response = resp;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;
	send_uplink(&uplink);
}

static void handle_get_ps_config_request(ros_wifi_v1_GetPowerSaveConfigRequest *proto)
{
	struct net_if *iface = net_if_get_wifi_sta();
	struct wifi_ps_config cfg = {0};
	int err;

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

	ARG_UNUSED(proto);

	err = iface ? net_mgmt(NET_REQUEST_WIFI_PS_CONFIG, iface, &cfg, sizeof(cfg)) : -ENODEV;

	resp_set_status(err, &resp, NULL);
	if (err == 0) {
		resp.which_payload = ros_wifi_v1_WifiMgmtResponse_ps_config_tag;
		resp.payload.ps_config.has_config = true;
		resp.payload.ps_config.config.has_ps_settings = true;
		resp.payload.ps_config.config.ps_settings.enabled =
			cfg.ps_params.enabled ? ros_wifi_v1_WifiPsState_WIFI_PS_STATE_ENABLE
					      : ros_wifi_v1_WifiPsState_WIFI_PS_STATE_DISABLE;
		resp.payload.ps_config.config.ps_settings.timeout_ms = cfg.ps_params.timeout_ms;
	}
	uplink.payload.response = resp;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;
	send_uplink(&uplink);
}

static void handle_get_reg_domain(ros_wifi_v1_GetRegulatoryDomainRequest *)
{
	static struct wifi_reg_domain domain_data = {};
	domain_data.oper = WIFI_MGMT_GET;

	int err = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, net_if_get_wifi_sta(), &domain_data,
			   sizeof(domain_data));

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

#if CONFIG_USE_DUMMY_WIFI_MGMT_DATA
	LOG_WRN("Using Test code - dummy values");
	err = 0;
	domain_data.num_channels = 3;
	struct wifi_reg_chan_info dummy_chn_info[] = {
		{123, 11, 1, 0, 1},
		{456, 22, 1, 1, 0},
		{789, 33, 0, 1, 0},
	};
	domain_data.chan_info = dummy_chn_info;
	domain_data.country_code[0] = 'I';
	domain_data.country_code[1] = 'S';

#endif

	resp_set_status(err, &resp, NULL);

	if (err == 0) {
		resp.which_payload = ros_wifi_v1_WifiMgmtResponse_reg_domain_tag;
		resp.payload.reg_domain.has_reg_domain = true;

		ros_wifi_v1_WifiRegDomain *resp_data = &resp.payload.reg_domain.reg_domain;
		memcpy(resp_data->country_code.bytes, domain_data.country_code,
		       WIFI_COUNTRY_CODE_LEN);
		resp_data->country_code.size = WIFI_COUNTRY_CODE_LEN;

		if (domain_data.num_channels <= MAX_REG_CHAN_NUM) {
			resp_data->channel_info_count = domain_data.num_channels;
			for (int i = 0; i < domain_data.num_channels; i++) {
				resp_data->channel_info[i].center_frequency_mhz =
					(uint32_t)domain_data.chan_info[i].center_frequency;

				resp_data->channel_info[i].max_power_dbm =
					(uint32_t)domain_data.chan_info[i].max_power;

				resp_data->channel_info[i].supported =
					(bool)domain_data.chan_info[i].supported;

				resp_data->channel_info[i].passive_only =
					(bool)domain_data.chan_info[i].passive_only;

				resp_data->channel_info[i].dfs = (bool)domain_data.chan_info[i].dfs;
			}
		}
	} else {
		LOG_WRN("Failed to get Reg Domain err=%d", err);
	}

	uplink.payload.response = resp;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;

	send_uplink(&uplink);
}

static void handle_get_version(ros_wifi_v1_GetWifiVersionRequest *)
{

	static struct wifi_version wifi_ver = {};

	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;

	int err = net_mgmt(NET_REQUEST_WIFI_VERSION, net_if_get_wifi_sta(), &wifi_ver,
			   sizeof(wifi_ver));

#if CONFIG_USE_DUMMY_WIFI_MGMT_DATA
	LOG_WRN("Using Test Code - Dummy Response");
	static char dummy_drv_ver[MAX_VERSION_LEN] = "1.2.3 TEST";
	static char dummy_fw_ver[MAX_VERSION_LEN] = "4.5.6 TEST";
	wifi_ver.drv_version = dummy_drv_ver;
	wifi_ver.fw_version = dummy_fw_ver;
	err = 0;
#endif

	resp_set_status(err, &resp, NULL);

	if (resp.status.success) {
		resp.which_payload = ros_wifi_v1_WifiMgmtResponse_version_tag;
		resp.payload.version.has_version = true;
		memcpy(resp.payload.version.version.driver_version, wifi_ver.drv_version,
		       MAX_VERSION_LEN);

		memcpy(resp.payload.version.version.firmware_version, wifi_ver.fw_version,
		       MAX_VERSION_LEN);
	}

	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;
	uplink.payload.response = resp;

	send_uplink(&uplink);
}

static void handle_vendor_cmd_request(ros_wifi_v1_VendorCmd *proto)
{
	struct net_if *iface = net_if_get_wifi_sta();
	ros_wifi_v1_WifiMgmtUplink uplink = ros_wifi_v1_WifiMgmtUplink_init_zero;
	ros_wifi_v1_WifiMgmtResponse resp = ros_wifi_v1_WifiMgmtResponse_init_zero;
	ros_wifi_v1_VendorCmd vc_resp = ros_wifi_v1_VendorCmd_init_zero;
	int err = 0;

	LOG_INF("vendor_cmd: subcmd=%u payload_len=%u", (unsigned)proto->subcmd_id,
		(unsigned)proto->payload.size);

	if (!iface) {
		err = -ENODEV;
		goto send_resp;
	}

	vc_resp.subcmd_id = proto->subcmd_id;

	/* Validate and copy a SET request payload into a driver param struct `_p`.
	 * Evaluates to 0 on success, -EINVAL if the payload is too short. */
#define VC_COPY_REQ(_p)                                                                            \
	(proto->payload.size < sizeof(_p)                                                          \
		 ? -EINVAL                                                                         \
		 : (memcpy(&(_p), proto->payload.bytes, sizeof(_p)), 0))

	/* Copy a GET result struct `_p` back as the response payload. */
#define VC_COPY_RESP(_p)                                                                           \
	do {                                                                                       \
		BUILD_ASSERT(sizeof(_p) <= RCP_WIFI_VENDOR_CMD_MAX_PAYLOAD,                         \
			     "resp exceeds vendor_cmd payload size");                              \
		memcpy(vc_resp.response_payload.bytes, &(_p), sizeof(_p));                         \
		vc_resp.response_payload.size = sizeof(_p);                                         \
	} while (0)

	switch (proto->subcmd_id) {
	case QCC730_SUBCMD_SET_PHY_MODE: {
		struct qcom_wifi_set_phy_mode_params p;
		err = VC_COPY_REQ(p);
		if (err == 0) {
			err = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_PHY_MODE, iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_GET_PHY_MODE: {
		struct qcom_wifi_get_phy_mode_params p = {0};
		err = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_PHY_MODE, iface, &p, sizeof(p));
		if (err == 0) {
			VC_COPY_RESP(p);
		}
		break;
	}
	case QCC730_SUBCMD_GET_MAC_ADDRESS: {
		struct qcom_wifi_get_mac_address_params p = {0};
		err = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_MAC_ADDRESS, iface, &p, sizeof(p));
		if (err == 0) {
			BUILD_ASSERT(sizeof(p.mac) <= RCP_WIFI_VENDOR_CMD_MAX_PAYLOAD,
				     "MAC exceeds vendor_cmd payload size");
			memcpy(vc_resp.response_payload.bytes, p.mac, sizeof(p.mac));
			vc_resp.response_payload.size = sizeof(p.mac);
		}
		break;
	}
	case QCC730_SUBCMD_GET_BOOT_REASON: {
		struct qcom_wifi_get_boot_reason_params p = {0};
		err = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_BOOT_REASON, iface, &p, sizeof(p));
		if (err == 0) {
			VC_COPY_RESP(p);
		}
		break;
	}
	case QCC730_SUBCMD_SET_RTS_RATE: {
		struct qcom_wifi_set_rts_rate_params p;
		err = VC_COPY_REQ(p);
		if (err == 0) {
			err = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_RTS_RATE, iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_GET_RTS_RATE: {
		struct qcom_wifi_get_rts_rate_params p = {0};
		err = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_RTS_RATE, iface, &p, sizeof(p));
		if (err == 0) {
			VC_COPY_RESP(p);
		}
		break;
	}
	case QCC730_SUBCMD_SET_RSP_RATE: {
		struct qcom_wifi_set_rsp_rate_params p;
		err = VC_COPY_REQ(p);
		if (err == 0) {
			err = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_RSP_RATE, iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_SET_EDCA_PARAM_CFG: {
		struct qcom_wifi_set_edca_param_cfg_params p;
		err = VC_COPY_REQ(p);
		if (err == 0) {
			err = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_EDCA_PARAM_CFG, iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_GET_EDCA_PARAM_CFG: {
		/* qid is an input selector; the remaining fields are filled on
		 * return. Seed qid from the request payload when provided. */
		struct qcom_wifi_get_edca_param_cfg_params p = {0};
		if (proto->payload.size >= sizeof(p.qid)) {
			p.qid = proto->payload.bytes[0];
		}
		err = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_EDCA_PARAM_CFG, iface, &p, sizeof(p));
		if (err == 0) {
			VC_COPY_RESP(p);
		}
		break;
	}
	case QCC730_SUBCMD_SET_BA_WIN_TIMING: {
		struct qcom_wifi_set_ba_win_timing_params p;
		err = VC_COPY_REQ(p);
		if (err == 0) {
			err = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_BA_WIN_TIMING, iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_GET_BA_WIN_TIMING: {
		struct qcom_wifi_get_ba_win_timing_params p = {0};
		err = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_BA_WIN_TIMING, iface, &p, sizeof(p));
		if (err == 0) {
			VC_COPY_RESP(p);
		}
		break;
	}
	case QCC730_SUBCMD_SET_BA_WIN_SIZE: {
		struct qcom_wifi_set_ba_win_size_params p;
		err = VC_COPY_REQ(p);
		if (err == 0) {
			err = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_BA_WIN_SIZE, iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_SET_SLOT_TIME: {
		struct qcom_wifi_set_slot_time_params p;
		err = VC_COPY_REQ(p);
		if (err == 0) {
			err = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_SLOT_TIME, iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_GET_SLOT_TIME: {
		struct qcom_wifi_get_slot_time_params p = {0};
		err = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_SLOT_TIME, iface, &p, sizeof(p));
		if (err == 0) {
			VC_COPY_RESP(p);
		}
		break;
	}
	case QCC730_SUBCMD_SET_BMISS_THRESHOLD: {
		struct qcom_wifi_set_bmiss_threshold_params p;
		err = VC_COPY_REQ(p);
		if (err == 0) {
			err = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_BMISS_THRESHOLD, iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_GET_BMISS_THRESHOLD: {
		struct qcom_wifi_get_bmiss_threshold_params p = {0};
		err = net_mgmt(NET_REQUEST_WIFI_QCOM_GET_BMISS_THRESHOLD, iface, &p, sizeof(p));
		if (err == 0) {
			VC_COPY_RESP(p);
		}
		break;
	}
	case QCC730_SUBCMD_SET_CTS_TO_SELF: {
		struct qcom_wifi_set_cts_to_self_params p;
		err = VC_COPY_REQ(p);
		if (err == 0) {
			err = net_mgmt(NET_REQUEST_WIFI_QCOM_SET_CTS_TO_SELF, iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_SET_IGNORE_BC_MC_IN_BMPS: {
		struct rcp_vc_bmps_flag f;
		err = VC_COPY_REQ(f);
		if (err == 0) {
			struct qcom_wifi_pm_ignore_bc_mc_params p = { .enable = f.enable };
			err = net_mgmt(NET_REQUEST_WIFI_PM_QCOM_IGNORE_BC_MC_IN_BMPS,
				       iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_SET_POWER_OPT_IN_BMPS: {
		struct rcp_vc_bmps_flag f;
		err = VC_COPY_REQ(f);
		if (err == 0) {
			struct qcom_wifi_pm_power_optimization_params p = { .enable = f.enable };
			err = net_mgmt(NET_REQUEST_WIFI_PM_QCOM_SET_POWER_OPTIMIZATION_ENABLE_IN_BMPS,
				       iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_SET_COMPRESS_QOS_NULL_IN_BMPS: {
		struct rcp_vc_bmps_flag f;
		err = VC_COPY_REQ(f);
		if (err == 0) {
			struct qcom_wifi_pm_compress_qos_null_params p = { .enable = f.enable };
			err = net_mgmt(NET_REQUEST_WIFI_PM_QCOM_SET_COMPRESS_QOS_NULL_ENABLE_IN_BMPS,
				       iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_SET_RX_FILTER_IN_BMPS: {
		struct rcp_vc_bmps_flag f;
		err = VC_COPY_REQ(f);
		if (err == 0) {
			struct qcom_wifi_pm_rx_filter_params p = {0};
			p.enable = f.enable;
			p.bmps_rx_filter_cb = NULL; /* firmware default rules */
			err = net_mgmt(NET_REQUEST_WIFI_PM_QCOM_SET_RX_FILTER_IN_BMPS,
				       iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_SET_LISTEN_INTERVAL: {
		struct rcp_vc_listen_interval li;
		err = VC_COPY_REQ(li);
		if (err == 0) {
			struct wifi_ps_params p = {0};
			p.type = WIFI_PS_PARAM_LISTEN_INTERVAL;
			p.listen_interval = li.listen_interval;
			err = net_mgmt(NET_REQUEST_WIFI_PS, iface, &p, sizeof(p));
		}
		break;
	}
	case QCC730_SUBCMD_UNIT_TEST: {
		/* SET-only: results are logged on the QCC730 console, not returned. */
		struct qcom_wifi_unit_test_params p;
		err = VC_COPY_REQ(p);
		if (err == 0) {
			err = net_mgmt(NET_REQUEST_WIFI_QCOM_UNIT_TEST, iface, &p, sizeof(p));
		}
		break;
	}
	default:
		LOG_WRN("vendor_cmd: unsupported subcmd=%u", (unsigned)proto->subcmd_id);
		err = -EOPNOTSUPP;
		break;
	}

#undef VC_COPY_REQ
#undef VC_COPY_RESP

send_resp:
	resp_set_status(err, &resp, NULL);
	resp.which_payload = ros_wifi_v1_WifiMgmtResponse_vendor_cmd_resp_tag;
	resp.payload.vendor_cmd_resp = vc_resp;
	uplink.which_payload = ros_wifi_v1_WifiMgmtUplink_response_tag;
	uplink.payload.response = resp;
	send_uplink(&uplink);
}

static uint8_t handle_mgmt_msg(enum rcp_rx_status status, rcp_buffer_hdl_t *payload_ptr,
			       rcp_error_info_t *err_info_ptr)
{
	if (status == RCP_RX_STS_NACK_MSG) {
		LOG_WRN("WiFi mgmt: ignoring NACK status=%d", (int)status);
		return RCP_RX_NO_ACTION;
	}

	if (status == RCP_RX_STS_ERROR) {
		/* TSN mismatch: rcp_mux_dispatcher already reset RSN to match the
		 * received TSN, so the payload is valid — process it to avoid
		 * dropping the request (scan/connect) that triggered the mismatch. */
		if (!payload_ptr) {
			LOG_WRN("WiFi mgmt: ERROR status=%d with no payload, ignoring",
				(int)status);
			return RCP_RX_NO_ACTION;
		}
		LOG_WRN("WiFi mgmt: processing ERROR status=%d (TSN resync, payload valid)",
			(int)status);
	}

	if (!payload_ptr) {
		LOG_WRN("WiFi management msg has no payload");
		return RCP_RX_NO_ACTION;
	}

	/*
	 * WifiMgmtRequest is a large nanopb struct (~1kB). Putting it on the stack can
	 * overflow smaller thread stacks (leading to random crashes). Storing it in
	 * BSS (static) can push the image over RAM limits.
	 *
	 * Decode into heap memory instead: avoids stack pressure without increasing
	 * link-time RAM usage. On OOM, we drop the message.
	 */
	ros_wifi_v1_WifiMgmtRequest *evt = k_malloc(sizeof(*evt));
	if (!evt) {
		LOG_ERR("OOM: cannot allocate WifiMgmtRequest (%u bytes)", (unsigned)sizeof(*evt));
		return RCP_RX_NO_ACTION;
	}
	*evt = (ros_wifi_v1_WifiMgmtRequest)ros_wifi_v1_WifiMgmtRequest_init_zero;

	LOG_DBG("wifi mgmt rx len=%u", (unsigned)payload_ptr->size);
	if (!decode_wifi_mgmt_req(evt, payload_ptr->data_ptr, payload_ptr->size)) {
		LOG_WRN("Failed to decode Wifi management msg");
		k_free(evt);
		return RCP_RX_NO_ACTION;
	}

	switch (evt->which_payload) {
	case ros_wifi_v1_WifiMgmtRequest_connect_tag:
		handle_connect_request(&evt->payload.connect);
		break;
	case ros_wifi_v1_WifiMgmtRequest_disconnect_tag:
		handle_disconn_request();
		break;
	case ros_wifi_v1_WifiMgmtRequest_ap_enable_tag:
		handle_ap_enable_request(&evt->payload.ap_enable);
		break;
	case ros_wifi_v1_WifiMgmtRequest_ap_disable_tag:
		handle_ap_disable_request(&evt->payload.ap_disable);
		break;
	case ros_wifi_v1_WifiMgmtRequest_ap_sta_disconnect_tag:
		handle_ap_sta_disconnect_request(&evt->payload.ap_sta_disconnect);
		break;
	case ros_wifi_v1_WifiMgmtRequest_get_l2_interfaces_tag:
		handle_get_l2_interface_request(&evt->payload.get_l2_interfaces);
		break;
	case ros_wifi_v1_WifiMgmtRequest_get_iface_status_tag:
		handle_get_iface_status_request(&evt->payload.get_iface_status);
		break;
	case ros_wifi_v1_WifiMgmtRequest_scan_tag:
		handle_scan_request(&evt->payload.scan);
		break;
	case ros_wifi_v1_WifiMgmtRequest_set_reg_domain_tag:
		handle_set_reg_domain(&evt->payload.set_reg_domain);
		break;
	case ros_wifi_v1_WifiMgmtRequest_set_rts_threshold_tag:
		handle_set_rts_threshold(&evt->payload.set_rts_threshold);
		break;
	case ros_wifi_v1_WifiMgmtRequest_set_tx_power_tag:
		handle_set_tx_power(&evt->payload.set_tx_power);
		break;
	case ros_wifi_v1_WifiMgmtRequest_get_tx_power_tag:
		handle_get_tx_power(&evt->payload.get_tx_power);
		break;
	case ros_wifi_v1_WifiMgmtRequest_get_reg_domain_tag:
		handle_get_reg_domain(&evt->payload.get_reg_domain);
		break;
	case ros_wifi_v1_WifiMgmtRequest_get_version_tag:
		handle_get_version(&evt->payload.get_version);
		break;
	case ros_wifi_v1_WifiMgmtRequest_set_ps_config_tag:
		handle_set_ps_config_request(&evt->payload.set_ps_config);
		break;
	case ros_wifi_v1_WifiMgmtRequest_get_ps_config_tag:
		handle_get_ps_config_request(&evt->payload.get_ps_config);
		break;
	case ros_wifi_v1_WifiMgmtRequest_vendor_cmd_tag:
		handle_vendor_cmd_request(&evt->payload.vendor_cmd);
		break;
	default:
		LOG_WRN("Unsupported WiFi management msg tag=%d", evt->which_payload);
		break;
	}

	k_free(evt);
	return RCP_RX_ACK;
}

static void wifi_mgmt_evt_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
				  struct net_if *iface)
{
	uint32_t iface_index = iface_ptr_to_index(iface);

	LOG_DBG("wifi_mgmt_evt: %s(0x%llx) iface_ptr=%p iface_index=%u",
		wifi_mgmt_event_name(mgmt_event), (unsigned long long)mgmt_event, iface, iface_index);

	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		LOG_DBG("Handling WiFi connect event");
		handle_wifi_connect_event(cb, iface_index);
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_DBG("Handling WiFi disconnect event");
		handle_wifi_disconnect_event(cb, iface_index);
		break;

	case NET_EVENT_WIFI_SCAN_RESULT:
		handle_wifi_scan_result(cb, iface_index);
		break;

	case NET_EVENT_WIFI_SCAN_DONE:
		handle_wifi_scan_done(cb, iface_index);
		break;

	case NET_EVENT_WIFI_AP_ENABLE_RESULT:
		handle_wifi_ap_enable_event(cb, iface_index);
		break;
	case NET_EVENT_WIFI_AP_DISABLE_RESULT:
		handle_wifi_ap_disable_event(cb, iface_index);
		break;
	case NET_EVENT_WIFI_AP_STA_CONNECTED:
		handle_wifi_ap_sta_connected_event(cb, iface_index);
		break;
	case NET_EVENT_WIFI_AP_STA_DISCONNECTED:
		handle_wifi_ap_sta_disconnected_event(cb, iface_index);
		break;

	default:
		LOG_WRN("Unexpected wifi management event %lld", (long long)mgmt_event);
	}
}
