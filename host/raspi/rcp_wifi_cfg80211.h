/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef RCP_WIFI_CFG80211_H
#define RCP_WIFI_CFG80211_H

#include <linux/netdevice.h>
#include <linux/wireless.h>
#include <net/cfg80211.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <rcp_interface.h>
#include <wifi/rcp_wifi_mgmt.h>
#include "rcp_chip_caps.h"

#define RCP_WIFI_DRIVER_NAME    "rcp_wifi_cfg80211"
#define RCP_WIFI_MAX_SSID_LEN   32

/* Kernel compat: Zephyr macro used in some rcp_protocol files */
#ifndef ARG_UNUSED
#define ARG_UNUSED(x) ((void)(x))
#endif

struct rcp_priv {
	struct wiphy                      *wiphy;
	struct net_device                 *netdev;
	struct wireless_dev                wdev;
	struct spi_device                 *spi;
	struct cfg80211_scan_request      *scan_request;
	struct task_struct                *event_thread;
	wait_queue_head_t                  wait_queue;
	spinlock_t                         lock;
	/* TX skbs queued by ndo_start_xmit (may run in softirq/atomic context)
	 * and drained by the event loop kthread (process context), which does the
	 * sleeping get_buffer()/link_send() work. sk_buff_head has its own lock. */
	struct sk_buff_head                tx_skb_q;
	atomic_t                           rx_events;
	/* Chip capability description (selected via of_match .data at probe).
	 * Const; the generic code reads chip params from here instead of
	 * hardcoding QCC730 constants. */
	const struct rcp_chip_caps        *chip_caps;
	/* Writable band state assembled from chip_caps at probe. cfg80211
	 * mutates channel flags at runtime (reg/DFS), so these cannot be
	 * const/shared with the caps templates. */
	struct ieee80211_supported_band    band_2g;
	struct ieee80211_supported_band    band_5g;
	struct ieee80211_channel          *chans_2g;	/* kmemdup of caps */
	struct ieee80211_channel          *chans_5g;
	struct ieee80211_rate             *rates_bg;	/* kmemdup; cfg80211
							 * writes bitrate flags */
	struct ieee80211_rate             *rates_ofdm;
	uint8_t                            mac_addr[ETH_ALEN];
	/* QCC730 STA and SAP ifaces have distinct MACs (SAP = STA-1). The single
	 * netdev uses the STA MAC by default; .start_ap switches it to the SAP MAC
	 * so downlink frames' source MAC matches the over-the-air BSSID. */
	uint8_t                            sta_mac[ETH_ALEN];
	uint8_t                            ap_mac[ETH_ALEN];
	bool                               ap_mac_valid;
	/* Security type of the running SoftAP (cached at .start_ap). Used to
	 * synthesize the RSN IE reported to hostapd via cfg80211_new_sta's
	 * assoc_req_ies — the firmware AP STA-connected event carries no IEs,
	 * and hostapd kicks a STA whose (Re)Assoc lacks the expected RSN IE. */
	enum rcp_wifi_sec_type             ap_sec_type;
	/* Cached iface status from RCP_WIFI_IFACE_STATUS_RESP event */
	struct rcp_wifi_iface_status       iface_status;
	bool                               iface_status_valid;
	struct completion                  iface_status_done; /* signaled on each RESP */
	/* TX power GET: synchronous round-trip over WIFI_MGMT proto. .get_tx_power
	 * sends the request then waits on tx_power_done; the response event fills
	 * tx_power and signals completion. */
	struct completion                  tx_power_done;
	struct rcp_wifi_evt_tx_power       tx_power;
	bool                               tx_power_valid;
	/* BSS reference held during connection: obtained in .connect(),
	 * used in cfg80211_connect_bss() on RCP_WIFI_CONNECTED.
	 * Kernel 6.12 requires a valid BSS for WLAN_STATUS_SUCCESS. */
	struct cfg80211_bss               *connect_bss;
	uint8_t                            connect_ssid[WIFI_MAX_SSID_SIZE];
	size_t                             connect_ssid_len;
	/* Deferred connect result: set when BSS was not in cache at connect
	 * time. Cleared when RCP_WIFI_IFACE_STATUS_RESP provides the BSSID. */
	bool                               pending_connect_result;
	/* Connect-in-progress guard for the firmware "fail-then-success" quirk.
	 * The QCC730 WLAN firmware occasionally raises two connect_result events
	 * for a single connect: first WIFI_STATUS_CONN_FAIL, then CONN_SUCCESS
	 * ~5s later. Reporting the first failure to cfg80211 tears down its SME
	 * state (ssid_len=0), after which the later success leaves the interface
	 * unable to disconnect. While connect_pending is set we do NOT report a
	 * failure immediately: it is deferred via connect_fail_work, and cancelled
	 * if a success arrives within the grace window.
	 *
	 * connect_pending / connect_gen / fail_gen are all protected by ->lock.
	 * connect_gen is bumped whenever an attempt is superseded (new connect,
	 * disconnect, or success); the deferred work only fires if the generation
	 * it was armed with (fail_gen) still matches — this invalidates a work
	 * item that was already running when a new attempt started. */
	bool                               connect_pending;
	u32                                connect_gen;
	u32                                fail_gen;
	struct delayed_work                connect_fail_work;
	/* Set once the WIFI_MGMT stream has synced (first L2_INTERFACES_RESP),
	 * cleared at remove time. Gates synchronous vendor commands so they do
	 * not block for the full timeout while the QcSPI link is unavailable
	 * (probe before sync, or teardown after the event kthread is stopped). */
	bool                               mgmt_ready;
};

/* Map an RCP band enum to the nl80211 band. */
static inline enum nl80211_band rcp_to_nl_band(enum rcp_wifi_band band)
{
	return (band == RCP_WIFI_BAND_5_GHZ) ?
	       NL80211_BAND_5GHZ : NL80211_BAND_2GHZ;
}

/* Supported interface modes */
extern const uint32_t rcp_cipher_suites[];
extern int            rcp_cipher_suites_count;

/* Global driver private context (set in probe, cleared in remove) */
extern struct rcp_priv *g_priv;

/* cfg80211_ops */
extern struct cfg80211_ops rcp_cfg80211_ops;

/* Regulatory domain notifier */
void rcp_reg_notifier(struct wiphy *wiphy, struct regulatory_request *request);

/* net_device_ops */
extern const struct net_device_ops rcp_ndo_ops;

/* Drain queued TX skbs into RCP buffers — called from the event loop kthread
 * (process context only; does sleeping get_buffer()/link_send()). */
void rcp_tx_drain_skb_q(struct rcp_priv *priv);

/* RCP WiFi event handler (called from event kthread) */
void rcp_wifi_event_to_cfg80211(struct rcp_priv *priv, int evt, void *data);

/* Delayed-work handler that reports a connect failure to cfg80211 if no
 * success arrived within the grace window (firmware fail-then-success quirk). */
void rcp_connect_fail_work(struct work_struct *work);

/* RX data handler — registered with RCP_STREAM_WIFI_DATA in probe */
uint8_t wifi_data_rx_handler(enum rcp_rx_status status,
			     rcp_buffer_hdl_t *payload,
			     rcp_error_info_t *err_info);

#endif /* RCP_WIFI_CFG80211_H */
