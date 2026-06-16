/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */


/**
 * @file rcp_wifi_mgmt.h
 *
 * @defgroup rcp_wifi_mgmt RCP WiFi management
 * @ingroup rcp_public
 *
 * Provides WiFi management functionality for platforms without native WiFi frameworks
 * (e.g., STM32 applications with lwIP networking).
 * For certain host platforms, a dedicated driver is recommended:
 * - Linux OS: RCP cfg80211 driver
 * - Zephyr RTOS: RCP WiFi device driver
 *
 * @{
 */
#ifndef __RCP_WIFI_MGMT_H__
#define __RCP_WIFI_MGMT_H__

#include <stdbool.h>
#include <stdint.h>

/** Maximum SSID size */
#define WIFI_MAX_SSID_SIZE 32
/** Maximum PSK size */
#define WIFI_MAX_PSK_SIZE  64
/** BSSID size */
#define WIFI_BSSID_SIZE    6

#define WIFI_MAX_SAE_PASSWORD_SIZE 128
#define WIFI_MAX_ANON_ID_SIZE      128
#define WIFI_MAX_KEY_PASSWORD_SIZE 64
#define WIFI_MAX_EAP_IDENTITY_SIZE 128
#define WIFI_MAX_EAP_PASSWORD_SIZE 64

#define RCP_WIFI_MAX_EAP_USERS 8

/** Maximum error message size */
#define WIFI_MAX_ERR_MSG_SIZE      50
/** Maximum number of L2 interfaces */
#define RCP_WIFI_MAX_L2_INTERFACES 2

/** Maximum number of SSIDs for scan filtering */
#define RCP_WIFI_SCAN_SSID_FILT_MAX   8
/** Maximum number of manually configured channels for scan */
#define RCP_WIFI_SCAN_CHAN_MAX_MANUAL 10

/** Country code length */
#define RCP_WIFI_COUNTRY_CODE_LEN 2
/** Maximum regulatory channel number */
#define RCP_MAX_REG_CHAN_NUM      42
/** Maximum WiFi version string length */
#define RCP_WIFI_VERSION_MAX_LEN  20

/** MCU WiFi Management Error Codes
 *   Only defined those that need to be distinguished
*/
#define RCP_WIFI_ERR_NOT_SUPPORTED	-134

/**
 * @enum rcp_wifi_status
 * RCP WiFi status codes
 */
enum rcp_wifi_status {
	RCP_WIFI_OK = 0,       /**< Operation successful */
	RCP_WIFI_ERR,          /**< Generic error */
	RCP_WIFI_ERR_NOT_SUPP, /**< Function or parameter not supported */
	RCP_WIFI_ERR_MEM,      /**< Not enough memory to perform operation */
};

/**
 * @enum rcp_netif_type
 * Network interface type
 */
enum rcp_netif_type {
	RCP_NET_IF_TYPE_UNKNOWN = 0,
	RCP_NET_IF_TYPE_WIFI = 1,
	RCP_NET_IF_TYPE_OPENTHREAD = 2,
};

/**
 * @enum rcp_wifi_sec_type
 * WiFi security type
 */
enum rcp_wifi_sec_type {
	RCP_WIFI_SEC_TYPE_NONE = 0,
	RCP_WIFI_SEC_TYPE_PSK = 1, /**< WPA/WPA2 PSK */
	RCP_WIFI_SEC_TYPE_PSK_SHA256 = 2,
	RCP_WIFI_SEC_TYPE_SAE = 3,     /**< WPA3 SAE */
	RCP_WIFI_SEC_TYPE_SAE_HNP = 3, /**< Alias for SAE */
	RCP_WIFI_SEC_TYPE_SAE_H2E = 4,
	RCP_WIFI_SEC_TYPE_SAE_AUTO = 5,
	RCP_WIFI_SEC_TYPE_WAPI = 6,
	RCP_WIFI_SEC_TYPE_EAP = 7,     /**< Enterprise (EAP) */
	RCP_WIFI_SEC_TYPE_EAP_TLS = 7, /**< Alias for EAP */
	RCP_WIFI_SEC_TYPE_WEP = 8,
	RCP_WIFI_SEC_TYPE_WPA_PSK = 9,
	RCP_WIFI_SEC_TYPE_WPA_AUTO_PERSONAL = 10,
	RCP_WIFI_SEC_TYPE_DPP = 11,
	RCP_WIFI_SEC_TYPE_EAP_PEAP_MSCHAPV2 = 12,
	RCP_WIFI_SEC_TYPE_EAP_PEAP_GTC = 13,
	RCP_WIFI_SEC_TYPE_EAP_TTLS_MSCHAPV2 = 14,
	RCP_WIFI_SEC_TYPE_EAP_PEAP_TLS = 15,
	RCP_WIFI_SEC_TYPE_FT_PSK = 16,
	RCP_WIFI_SEC_TYPE_FT_SAE = 17,
	RCP_WIFI_SEC_TYPE_FT_EAP = 18,
	RCP_WIFI_SEC_TYPE_FT_EAP_SHA384 = 19,
	RCP_WIFI_SEC_TYPE_SAE_EXT_KEY = 20,
	RCP_WIFI_SEC_TYPE_UNKNOWN = 21,
	RCP_WIFI_SEC_TYPE_OSEN = 22, /**< Not directly supported by Zephyr */
	RCP_WIFI_SEC_TYPE_OWE = 23,
	RCP_WIFI_SEC_TYPE_WPA3_EAP_SUITE_B = 24,
};

/**
 * @enum rcp_wifi_wpa_ent_type
 * WPA Enterprise type
 */
enum rcp_wifi_wpa_ent_type {
	RCP_WIFI_WPA3_TYPE_NA = 0,
	RCP_WIFI_WPA3_SUITEB = 1,
	RCP_WIFI_WPA3_SUITEB_192 = 2,
	RCP_WIFI_WPA3_PA3_ONLY = 3,
};

/**
 * @enum rcp_wifi_mfp_options
 * Management frame protection options
 */
enum rcp_wifi_mfp_options {
	RCP_WIFI_MFP_DISABLE = 0,
	RCP_WIFI_MFP_OPTIONAL = 1,
	RCP_WIFI_MFP_REQUIRED = 2,
};

/**
 * @enum rcp_wifi_iface_state
 * WiFi interface state (aligned with Zephyr enum wifi_iface_state)
 */
enum rcp_wifi_iface_state {
	RCP_WIFI_STATE_DISCONNECTED = 0,
	RCP_WIFI_STATE_INTERFACE_DISABLED = 1,
	RCP_WIFI_STATE_INACTIVE = 2,
	RCP_WIFI_STATE_SCANNING = 3,
	RCP_WIFI_STATE_AUTHENTICATING = 4,
	RCP_WIFI_STATE_ASSOCIATING = 5,
	RCP_WIFI_STATE_ASSOCIATED = 6,
	RCP_WIFI_STATE_4WAY_HANDSHAKE = 7,
	RCP_WIFI_STATE_GROUP_HANDSHAKE = 8,
	RCP_WIFI_STATE_COMPLETED = 9,
	RCP_WIFI_STATE_UNKNOWN = 11,
};

/**
 * @enum rcp_wifi_iface_mode
 * WiFi interface mode (aligned with Zephyr enum wifi_iface_mode)
 */
enum rcp_wifi_iface_mode {
	RCP_WIFI_MODE_INFRA = 0, /**< STA */
	RCP_WIFI_MODE_IBSS = 1,  /**< IBSS (ad-hoc) */
	RCP_WIFI_MODE_AP = 2,    /**< SoftAP */
	RCP_WIFI_MODE_P2P_GO = 3,
	RCP_WIFI_MODE_P2P_GROUP_FORMATION = 4,
	RCP_WIFI_MODE_MESH = 5,
};

/**
 * @enum rcp_wifi_link_mode
 * WiFi link mode / PHY, aligned with Zephyr enum wifi_link_mode
 */
enum rcp_wifi_link_mode {
	RCP_WIFI_LINK_MODE_WIFI_0 = 0,  /**< 802.11 (legacy) */
	RCP_WIFI_LINK_MODE_WIFI_1 = 1,  /**< 802.11b */
	RCP_WIFI_LINK_MODE_WIFI_2 = 2,  /**< 802.11a */
	RCP_WIFI_LINK_MODE_WIFI_3 = 3,  /**< 802.11g */
	RCP_WIFI_LINK_MODE_WIFI_4 = 4,  /**< 802.11n (HT) */
	RCP_WIFI_LINK_MODE_WIFI_5 = 5,  /**< 802.11ac (VHT) */
	RCP_WIFI_LINK_MODE_WIFI_6 = 6,  /**< 802.11ax (HE) */
	RCP_WIFI_LINK_MODE_WIFI_6E = 7, /**< 802.11ax 6GHz (Wi-Fi 6E) */
	RCP_WIFI_LINK_MODE_WIFI_7 = 8,  /**< 802.11be (EHT) */
	RCP_WIFI_LINK_MODE_UNKNOWN = 10,
};

/**
 * @enum rcp_wifi_band
 * WiFi frequency band
 */
enum rcp_wifi_band {
	RCP_WIFI_BAND_2_4_GHZ = 0,
	RCP_WIFI_BAND_5_GHZ = 1,
	RCP_WIFI_BAND_6_GHZ = 2,
	RCP_WIFI_BAND_UNKNOWN = 3,
	RCP_WIFI_BAND_60_GHZ = 4,
	RCP_WIFI_BAND_ANY = 5,
};

/**
 * @enum rcp_wifi_bw
 * WiFi channel bandwidth
 */
enum rcp_wifi_bw {
	RCP_WIFI_BW_UNKNOWN = 0,
	RCP_WIFI_BW_20MHZ = 1,
	RCP_WIFI_BW_40MHZ = 2,
	RCP_WIFI_BW_80MHZ = 3,
	RCP_WIFI_BW_160MHZ = 4,
	RCP_WIFI_BW_80_80_MHZ = 5,
};

/**
 * @enum rcp_scan_type
 * WiFi scan type
 */
enum rcp_scan_type {
	RCP_WIFI_SCAN_PASSIVE = 0,
	RCP_WIFI_SCAN_ACTIVE = 1,
};

/**
 * @struct rcp_wifi_band_channel
 * Band and channel pairing
 */
struct rcp_wifi_band_channel {
	enum rcp_wifi_band band; /**< Band */
	uint32_t channel;        /**< Channel number */
};

/**
 * @struct rcp_ssid
 * SSID representation
 */
struct rcp_ssid {
	uint32_t size;                     /**< SSID length */
	uint8_t bytes[WIFI_MAX_SSID_SIZE]; /**< SSID bytes */
};

/**
 * @enum rcp_wifi_evt
 * RCP WiFi events
 */
enum rcp_wifi_evt {
	RCP_WIFI_CONNECTED,
	RCP_WIFI_DISCONNECTED,
	RCP_WIFI_AP_ENABLED,
	RCP_WIFI_AP_DISABLED,
	RCP_WIFI_AP_STA_CONNECTED,
	RCP_WIFI_AP_STA_DISCONNECTED,
	/* Status response to WiFi commands */
	RCP_WIFI_STATUS_RESP,
	RCP_WIFI_L2_INTERFACES_RESP,
	RCP_WIFI_IFACE_STATUS_RESP,
	RCP_WIFI_SCAN_RESULT,
	RCP_WIFI_SCAN_DONE,
	RCP_WIFI_VERSION,
	RCP_WIFI_REG_DOMAIN,
	RCP_TEST_RESULT,
};

/**
 * @struct rcp_wifi_evt_status
 * Command status event data
 */
struct rcp_wifi_evt_status {
	bool success;                          /**< Operation success */
	int32_t error_code;                    /**< Error code if failed */
	char error_msg[WIFI_MAX_ERR_MSG_SIZE]; /**< Error message */
};

/**
 * @struct rcp_wifi_evt_conn
 * Connection / disconnection event data
 */
struct rcp_wifi_evt_conn {
	uint32_t status; /**< Connection status */
};

/**
 * @struct rcp_wifi_evt_ap_result
 * SoftAP enable/disable result event data.
 */
struct rcp_wifi_evt_ap_result {
	int32_t status;
};

/**
 * @struct rcp_wifi_evt_ap_sta
 * SoftAP station (client) connect/disconnect event data.
 * */
struct rcp_wifi_evt_ap_sta {
	uint8_t sta_mac_address[WIFI_BSSID_SIZE];
};

/**
 * @struct rcp_wifi_evt_scan_result
 * Access point information from scan
 */
struct rcp_wifi_evt_scan_result {
	struct rcp_ssid ssid;                    /**< SSID */
	uint8_t rssi;                            /**< Signal strength */
	enum rcp_wifi_band band;                 /**< Band */
	uint32_t channel;                        /**< Channel */
	enum rcp_wifi_wpa_ent_type wpa_ent_type; /**< WPA Enterprise type */
	enum rcp_wifi_mfp_options mfp;           /**< MFP option */
	enum rcp_wifi_sec_type sec_type;         /**< Security type */
	uint8_t bssid[WIFI_BSSID_SIZE];          /**< BSSID */
};

/**
 * @struct rcp_wifi_iface_status
 * WiFi interface status response payload (mirrors Zephyr wifi_iface_status)
 */
struct rcp_wifi_iface_status {
	enum rcp_wifi_iface_state state;
	enum rcp_wifi_iface_mode iface_mode;
	enum rcp_wifi_link_mode link_mode;

	uint8_t ssid[WIFI_MAX_SSID_SIZE];
	uint8_t ssid_len;
	uint8_t bssid[WIFI_BSSID_SIZE];

	enum rcp_wifi_band band;
	uint32_t channel;
	enum rcp_wifi_wpa_ent_type wpa3_ent_type;
	enum rcp_wifi_sec_type security;
	enum rcp_wifi_mfp_options mfp;

	int32_t rssi;
	uint32_t beacon_interval;
	uint32_t dtim_period;
	bool twt_capable;

	/** TX bitrate in kbps (proto carries kbps; host may present Mbps). */
	uint32_t tx_bitrate_kbps;
};

struct rcp_wifi_evt_iface_status {
	uint32_t iface_index;
	struct rcp_wifi_iface_status status;
};

typedef void (*rcp_wifi_event_callback)(enum rcp_wifi_evt evt, const void *data);

/**
 * @struct rcp_wifi_conn_req
 * WiFi connection request parameters
 */
struct rcp_wifi_conn_req {
	uint32_t iface_index;
	uint8_t ssid[WIFI_MAX_SSID_SIZE];
	uint8_t ssid_len;
	enum rcp_wifi_sec_type sec_type;
	uint32_t channel;

	/* PSK/WEP */
	uint8_t psk[WIFI_MAX_PSK_SIZE];
	uint8_t psk_len;

	/* SAE */
	uint8_t sae_password[WIFI_MAX_SAE_PASSWORD_SIZE];
	uint8_t sae_password_len;

	/* Band / channel settings */
	enum rcp_wifi_band freq_band;
	enum rcp_wifi_bw freq_bw;
	uint32_t timeout_ms;

	/* Optional target BSSID (6 bytes) */
	uint8_t bssid[WIFI_BSSID_SIZE];
	uint8_t bssid_len;

	/* MFP / WPA3 */
	enum rcp_wifi_mfp_options mfp;
	bool verify_peer_cert;
	bool ft_enabled;
	uint8_t ignore_broadcast_ssid; /* 0..2 */

	/* Enterprise (EAP) */
	uint8_t anonymous_id[WIFI_MAX_ANON_ID_SIZE];
	uint8_t anonymous_id_len;
	uint8_t key_password[WIFI_MAX_KEY_PASSWORD_SIZE];
	uint8_t key_password_len;
	uint8_t key2_password[WIFI_MAX_KEY_PASSWORD_SIZE];
	uint8_t key2_password_len;
	enum rcp_wifi_wpa_ent_type wpa3_ent_type;
	uint32_t tls_cipher_suite;
	uint32_t eap_version;
	uint8_t eap_identity[WIFI_MAX_EAP_IDENTITY_SIZE];
	uint8_t eap_identity_len;
	uint8_t eap_password[WIFI_MAX_EAP_PASSWORD_SIZE];
	uint8_t eap_password_len;
	uint8_t eap_identities_count;
	uint8_t eap_passwords_count;
	uint8_t eap_identities[RCP_WIFI_MAX_EAP_USERS][WIFI_MAX_EAP_IDENTITY_SIZE];
	uint8_t eap_identities_len[RCP_WIFI_MAX_EAP_USERS];
	uint8_t eap_passwords[RCP_WIFI_MAX_EAP_USERS][WIFI_MAX_EAP_PASSWORD_SIZE];
	uint8_t eap_passwords_len[RCP_WIFI_MAX_EAP_USERS];
};

/**
 * @struct rcp_wifi_ap_config
 * SoftAP (host-managed) configuration.
 *
 * Note: RCP firmware may ignore some timing fields depending on driver support.
 */
struct rcp_wifi_ap_config {
	uint8_t ssid[WIFI_MAX_SSID_SIZE];
	uint8_t ssid_len;
	enum rcp_wifi_sec_type sec_type;
	uint8_t psk[WIFI_MAX_PSK_SIZE];
	uint8_t psk_len;
	uint32_t channel;
	enum rcp_wifi_band band;
	bool hidden_ssid;
	uint32_t beacon_interval_tu;
	uint32_t dtim_period;
};

/**
 * @struct rcp_wifi_l2_interface
 * L2 interface information
 */
struct rcp_wifi_l2_interface {
	uint32_t index;                       /**< Interface index */
	enum rcp_netif_type net_if_type;      /**< Network interface type */
	uint8_t mac_address[WIFI_BSSID_SIZE]; /**< MAC address */
	bool offload_supported;               /**< Offload support flag */
	uint32_t mtu;                         /**< MTU */
};

/**
 * @struct rcp_wifi_l2_interfaces
 * L2 interfaces response data
 */
struct rcp_wifi_l2_interfaces {
	uint32_t count; /**< Number of interfaces */
	struct rcp_wifi_l2_interface net_ifs[RCP_WIFI_MAX_L2_INTERFACES]; /**< Interfaces array */
};

/**
 * @struct rcp_wifi_scan_params
 * Parameters for WiFi scan
 */
struct rcp_wifi_scan_params {
	enum rcp_scan_type scan_type;                       /**< Scan type */
	uint32_t dwell_time_active;                         /**< Active scan dwell time */
	uint32_t dwell_time_passive;                        /**< Passive scan dwell time */
	uint32_t bands_bitmap;                              /**< Bands bitmap */
	uint16_t ssids_count;                               /**< Number of SSIDs for filter */
	struct rcp_ssid ssids[RCP_WIFI_SCAN_SSID_FILT_MAX]; /**< SSIDs array */
	int32_t max_bss_cnt;                                /**< Maximum BSS count */
	uint16_t band_chan_count;                           /**< Number of band-channel entries */
	struct rcp_wifi_band_channel
		band_chan[RCP_WIFI_SCAN_CHAN_MAX_MANUAL]; /**< Band-channel array */
};

struct rcp_wifi_reg_ch_info {
	uint32_t center_frequency_mhz;
	uint32_t max_power_dbm;
	bool supported;
	bool passive_only;
	bool dfs;
};

struct rcp_wifi_reg_domain {
	uint32_t iface_index;
	uint8_t country_code[RCP_WIFI_COUNTRY_CODE_LEN]; /* ISO 3166-1 alpha-2 */
	bool force_update;
	int32_t channel_info_count;
	struct rcp_wifi_reg_ch_info channel_info[RCP_MAX_REG_CHAN_NUM];
};

struct rcp_wifi_version {
	char driver_ver[RCP_WIFI_VERSION_MAX_LEN];
	char firmware_ver[RCP_WIFI_VERSION_MAX_LEN];
};

/**
 * Initialize RCP WiFi management subsystem
 * @return RCP WiFi status code
 */
enum rcp_wifi_status rcp_wifi_init(void);

/**
 * Register RCP WiFi event callback
 * @param evt_cb Event callback function
 * @return RCP WiFi status code
 * @note RCP_WIFI_ERR_MEM if too many callbacks registered
 */
enum rcp_wifi_status rcp_wifi_register_callback(rcp_wifi_event_callback evt_cb);

/**
 * Connect to WiFi network
 * @param req Pointer to connection request parameters
 * @return RCP WiFi status code
 * @note RCP_WIFI_ERR_NOT_SUPP if request parameters not supported
 */
enum rcp_wifi_status rcp_wifi_connect(struct rcp_wifi_conn_req *req);

/**
 * Disconnect from WiFi network
 * @return RCP WiFi status code
 * @note RCP_WIFI_ERR_NOT_SUPP if operation not supported
 */
enum rcp_wifi_status rcp_wifi_disconnect(void);

/**
 * Enable SoftAP (access point) mode.
 * @param if_index - WiFi interface index (typically 1)
 * @param cfg      - SoftAP configuration
 * @return RCP WiFi status code
 */
enum rcp_wifi_status rcp_wifi_ap_enable(uint32_t if_index, const struct rcp_wifi_ap_config *cfg);

/**
 *  Disable SoftAP (access point) mode.
 * @param if_index - WiFi interface index
 * @return RCP WiFi status code
 */
enum rcp_wifi_status rcp_wifi_ap_disable(uint32_t if_index);

/**
 * Query L2 interfaces
 * @param net_if_type Interface type filter (RCP_NET_IF_TYPE_UNKNOWN => all)
 * @param index Interface index filter (0 => any)
 * @return RCP WiFi status code
 */
enum rcp_wifi_status rcp_wifi_get_l2_interfaces(enum rcp_netif_type net_if_type, uint32_t index);

/**
 * Request WiFi scan
 * @param if_index Interface index to scan on
 * @param params Scan parameters
 * @return RCP WiFi status code
 */
enum rcp_wifi_status rcp_wifi_scan(uint32_t if_index, struct rcp_wifi_scan_params *params);

/**
 * Request WiFi interface status (STA/AP state, RSSI, rates, etc.)
 * @param if_index Interface index
 * @return RCP WiFi status code
 */
enum rcp_wifi_status rcp_wifi_get_iface_status(uint32_t if_index);

/**
 * Set WiFi driver and firmware versions
 * @return RCP WiFi status code
 */
enum rcp_wifi_status rcp_wifi_get_version();

/**
 * Set WiFi Regulatory Domain.
 *
 * @param settings Regulatory domain settings
 * @return RCP WiFi status code
 */
enum rcp_wifi_status rcp_wifi_set_reg_domain(const struct rcp_wifi_reg_domain *settings);

/**
 * Request Regulatory Domain info
 *
 * @param if_index Indicates which interface to get reg info
 * @return RCP WiFi status code
 */
enum rcp_wifi_status rcp_wifi_get_reg_domain(uint32_t if_index);

/** @} end of rcp_wifi_mgmt */

#endif
