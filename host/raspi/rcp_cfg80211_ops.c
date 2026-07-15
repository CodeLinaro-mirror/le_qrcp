/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <linux/module.h>
#include <linux/ieee80211.h>
#include <linux/etherdevice.h>
#include <net/cfg80211.h>

#include "rcp_wifi_cfg80211.h"
#include "rcp_bus_spi_master_linux_qcc730.h"
#include <wifi/rcp_wifi_mgmt.h>
#include <rcp_vendor_cmds.h>

static int rcp_cfg80211_scan(struct wiphy *wiphy,
			     struct cfg80211_scan_request *request)
{
	struct rcp_priv *priv = wiphy_priv(wiphy);
	struct rcp_wifi_scan_params params = { 0 };
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&priv->lock, flags);
	priv->scan_request = request;
	spin_unlock_irqrestore(&priv->lock, flags);

	params.scan_type = RCP_WIFI_SCAN_ACTIVE;

	/* Forward directed-scan SSIDs (iw scan ssid <x>). Skip wildcard
	 * (zero-length) entries; an empty list means a plain active scan. */
	if (request) {
		int i;
		for (i = 0; i < request->n_ssids &&
			    params.ssids_count < RCP_WIFI_SCAN_SSID_FILT_MAX; i++) {
			u8 len = request->ssids[i].ssid_len;

			if (len == 0 || len > WIFI_MAX_SSID_SIZE)
				continue;
			params.ssids[params.ssids_count].size = len;
			memcpy(params.ssids[params.ssids_count].bytes,
			       request->ssids[i].ssid, len);
			params.ssids_count++;
		}
	}

	ret = rcp_wifi_scan(0, &params);
	if (ret != RCP_WIFI_OK) {
		spin_lock_irqsave(&priv->lock, flags);
		priv->scan_request = NULL;
		spin_unlock_irqrestore(&priv->lock, flags);
		return -EIO;
	}
	return 0;
}

static int rcp_cfg80211_connect(struct wiphy *wiphy, struct net_device *dev,
				struct cfg80211_connect_params *sme)
{
	struct rcp_wifi_conn_req *req;
	int ret;

	/* struct rcp_wifi_conn_req is ~2KB; allocate on heap to stay within
	 * the kernel stack frame limit (2048 bytes). */
	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req->iface_index = 0;
	req->ssid_len    = min_t(size_t, sme->ssid_len, WIFI_MAX_SSID_SIZE);
	memcpy(req->ssid, sme->ssid, req->ssid_len);

	/* Band/channel: use specific values when the caller provides a channel,
	 * otherwise let QCC730 firmware pick any band/channel. */
	if (sme->channel) {
		req->freq_band = (sme->channel->band == NL80211_BAND_5GHZ) ?
				 RCP_WIFI_BAND_5_GHZ : RCP_WIFI_BAND_2_4_GHZ;
		req->channel   = (uint32_t)sme->channel->hw_value;
	} else {
		req->freq_band = RCP_WIFI_BAND_ANY;
		req->channel   = 0; /* WIFI_CHANNEL_ANY on QCC730 side */
	}

	/* Security: derive from crypto settings.
	 * WPA2-PSK: wpa_supplicant sends 32B PMK via sme->crypto.psk (offload).
	 * WPA3-SAE: detected via sme->auth_type == NL80211_AUTHTYPE_SAE.
	 *   wpa_supplicant 2.10 (patched) sends NL80211_ATTR_SAE_PASSWORD →
	 *   kernel puts it in sme->crypto.sae_pwd. */
	if (sme->auth_type == NL80211_AUTHTYPE_SAE) {
		req->sec_type = RCP_WIFI_SEC_TYPE_SAE;
		/* WPA3-SAE mandates PMF/MFP; without it the AP rejects and the
		 * firmware silently times out. Force MFP=REQUIRED for SAE. */
		req->mfp = RCP_WIFI_MFP_REQUIRED;
		if (sme->crypto.sae_pwd && sme->crypto.sae_pwd_len) {
			req->sae_password_len = min_t(size_t,
						      sme->crypto.sae_pwd_len,
						      sizeof(req->sae_password) - 1);
			memcpy(req->sae_password, sme->crypto.sae_pwd,
			       req->sae_password_len);
		}
	} else if (sme->crypto.wpa_versions &
		   (NL80211_WPA_VERSION_2 | NL80211_WPA_VERSION_1)) {
		req->sec_type = RCP_WIFI_SEC_TYPE_PSK;
		if (sme->crypto.psk) {
			/* wpa_supplicant sends 32B PMK via 4-way-handshake offload;
			 * firmware uses it directly (CONFIG_WIFI_PMK_BYPASS_KDF). */
			req->psk_len = 32;
			memcpy(req->psk, sme->crypto.psk, 32);
		} else {
			pr_warn("rcp: connect: WPA2 no psk source (crypto.psk=NULL)\n");
		}
	} else if (sme->key_len > 0 && sme->key) {
		/* WEP */
		req->sec_type = RCP_WIFI_SEC_TYPE_WEP;
		req->psk_len  = min_t(size_t, sme->key_len, WIFI_MAX_PSK_SIZE);
		memcpy(req->psk, sme->key, req->psk_len);
	} else {
		req->sec_type = RCP_WIFI_SEC_TYPE_NONE;
	}

	/* Cache BSS ref so connect_result can pass a valid BSS to cfg80211
	 * (kernel 6.12 requires a non-NULL BSS for WLAN_STATUS_SUCCESS). */
	{
		struct rcp_priv *priv = wiphy_priv(wiphy);
		unsigned long flags;

		/* Release any stale BSS from a previous connect attempt */
		spin_lock_irqsave(&priv->lock, flags);
		if (priv->connect_bss) {
			cfg80211_put_bss(wiphy, priv->connect_bss);
			priv->connect_bss = NULL;
		}
		priv->connect_ssid_len = req->ssid_len;
		memcpy(priv->connect_ssid, req->ssid, req->ssid_len);
		spin_unlock_irqrestore(&priv->lock, flags);

		priv->connect_bss = cfg80211_get_bss(wiphy, sme->channel,
						     sme->bssid,
						     sme->ssid, sme->ssid_len,
						     IEEE80211_BSS_TYPE_ESS,
						     IEEE80211_PRIVACY_ANY);
	}

	/* Arm the fail-then-success guard: until a connect result completes, a
	 * firmware CONN_FAIL is deferred. Bump the generation to invalidate any
	 * deferred failure from a previous attempt, then cancel its timer. */
	{
		struct rcp_priv *priv = wiphy_priv(wiphy);
		unsigned long flags;

		spin_lock_irqsave(&priv->lock, flags);
		priv->connect_gen++;
		priv->connect_pending = true;
		spin_unlock_irqrestore(&priv->lock, flags);
		cancel_delayed_work(&priv->connect_fail_work);
	}

	ret = (rcp_wifi_connect(req) != RCP_WIFI_OK) ? -EIO : 0;
	kfree(req);
	return ret;
}

static int rcp_cfg80211_disconnect(struct wiphy *wiphy, struct net_device *dev,
				   u16 reason_code)
{
	struct rcp_priv *priv = wiphy_priv(wiphy);

	/* A disconnect ends any in-progress connect: drop the deferred-failure
	 * guard so a late CONN_FAIL cannot fire after we've moved on. */
	{
		unsigned long flags;

		spin_lock_irqsave(&priv->lock, flags);
		priv->connect_pending = false;
		priv->connect_gen++;
		spin_unlock_irqrestore(&priv->lock, flags);
		cancel_delayed_work(&priv->connect_fail_work);
	}

	/* Release cached BSS ref if disconnect is called before connect result */
	if (priv->connect_bss) {
		cfg80211_put_bss(wiphy, priv->connect_bss);
		priv->connect_bss = NULL;
	}

	if (rcp_wifi_disconnect() != RCP_WIFI_OK)
		return -EIO;
	return 0;
}

/* Key management is offloaded to the QCC730 firmware (it runs the 4-way
 * handshake and owns the PTK/GTK). hostapd still issues NEW_KEY/DEL_KEY for the
 * GTK in AP mode; accept them as no-ops so interface setup completes. */
static int rcp_cfg80211_add_key(struct wiphy *wiphy, struct net_device *netdev,
				int link_id, u8 key_index, bool pairwise,
				const u8 *mac_addr, struct key_params *params)
{
	return 0;
}

static int rcp_cfg80211_del_key(struct wiphy *wiphy, struct net_device *netdev,
				int link_id, u8 key_index, bool pairwise,
				const u8 *mac_addr)
{
	return 0;
}

static int rcp_cfg80211_set_default_key(struct wiphy *wiphy, struct net_device *netdev,
					int link_id, u8 key_index, bool unicast,
					bool multicast)
{
	return 0;
}

/* hostapd switches wlan1 in place to AP before start_ap; firmware does the real
 * STA/AP mode switch in ap_enable, so here we only track the iftype. */
static int rcp_cfg80211_change_iface(struct wiphy *wiphy, struct net_device *dev,
				     enum nl80211_iftype type,
				     struct vif_params *params)
{
	if (type != NL80211_IFTYPE_STATION && type != NL80211_IFTYPE_AP)
		return -EOPNOTSUPP;

	dev->ieee80211_ptr->iftype = type;
	return 0;
}

static int rcp_cfg80211_start_ap(struct wiphy *wiphy, struct net_device *dev,
				 struct cfg80211_ap_settings *settings)
{
	struct rcp_priv *priv = wiphy_priv(wiphy);
	struct rcp_wifi_ap_config cfg = { 0 };

	cfg.ssid_len  = min_t(size_t, settings->ssid_len, WIFI_MAX_SSID_SIZE);
	memcpy(cfg.ssid, settings->ssid, cfg.ssid_len);
	cfg.channel   = settings->chandef.chan->hw_value;
	cfg.sec_type  = RCP_WIFI_SEC_TYPE_NONE;

	if (settings->crypto.n_akm_suites > 0) {
		/* WPA3-SAE AP is not supported in phase 1. */
		if (settings->auth_type == NL80211_AUTHTYPE_SAE)
			return -EOPNOTSUPP;

		cfg.sec_type = RCP_WIFI_SEC_TYPE_PSK;
		if (settings->crypto.psk) {
			/* AP-PSK offload: hostapd passes the 32B PMK directly;
			 * firmware uses it as PMK (CONFIG_WIFI_PMK_BYPASS_KDF). */
			cfg.psk_len = 32;
			memcpy(cfg.psk, settings->crypto.psk, 32);
		}
	}

	/* The netdev MAC is aligned to the SAP MAC (the over-the-air BSSID, and
	 * the required source MAC for downlink frames) when the AP_ENABLED event
	 * re-queries L2_INTERFACES; firmware only assigns the SAP MAC once the
	 * SoftAP is up, so it cannot be set reliably here. */
	if (rcp_wifi_ap_enable(0, &cfg) != RCP_WIFI_OK)
		return -EIO;

	/* Remember the AP security type so the AP STA-connected event can
	 * report a matching RSN IE to hostapd (see rcp_wifi_event.c). */
	priv->ap_sec_type = cfg.sec_type;
	return 0;
}

static int rcp_cfg80211_stop_ap(struct wiphy *wiphy, struct net_device *dev,
				unsigned int link_id)
{
	struct rcp_priv *priv = wiphy_priv(wiphy);

	if (rcp_wifi_ap_disable(0) != RCP_WIFI_OK)
		return -EIO;

	/* Restore the STA MAC for subsequent STA-mode use. */
	if (!is_zero_ether_addr(priv->sta_mac))
		dev_addr_set(dev, priv->sta_mac);
	return 0;
}

/* hostapd deauthenticates an associated STA. Fire-and-forget: the firmware
 * emits the deauth frame via qapi_WLAN_AP_Disconnect_Station. A NULL mac
 * (e.g. "kick all") is not forwarded; stop_ap covers tearing down all STAs. */
static int rcp_cfg80211_del_station(struct wiphy *wiphy, struct net_device *dev,
				    struct station_del_parameters *params)
{
	if (!params->mac)
		return 0;

	if (rcp_wifi_ap_sta_disconnect(2U /* PROXY_WIFI_IFACE_AP */, params->mac) != RCP_WIFI_OK)
		return -EIO;
	return 0;
}

/* Query QCC730 iface status synchronously. Returns 0 or -ENODEV/-ENOENT. */
static int rcp_query_iface_status(void)
{
	if (!g_priv)
		return -ENODEV;

	reinit_completion(&g_priv->iface_status_done);
	rcp_wifi_get_iface_status(0);
	wait_for_completion_timeout(&g_priv->iface_status_done, HZ);

	return g_priv->iface_status_valid ? 0 : -ENOENT;
}

/* Fill station_info (signal + tx bitrate) from the cached iface status. */
static void rcp_fill_station_info(struct station_info *sinfo)
{
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL);
	sinfo->signal  = (s8)g_priv->iface_status.rssi;

	if (g_priv->iface_status.tx_bitrate_kbps) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);
		sinfo->txrate.legacy =
			g_priv->iface_status.tx_bitrate_kbps / 100;
	}

	/* TX/RX counts from the netdev stats the data path maintains. tx_failed is
	 * left unset — the host has no over-the-air TX-failure count. */
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_PACKETS) |
			 BIT_ULL(NL80211_STA_INFO_RX_PACKETS) |
			 BIT_ULL(NL80211_STA_INFO_TX_BYTES64) |
			 BIT_ULL(NL80211_STA_INFO_RX_BYTES64);
	sinfo->tx_packets = g_priv->netdev->stats.tx_packets;
	sinfo->rx_packets = g_priv->netdev->stats.rx_packets;
	sinfo->tx_bytes   = g_priv->netdev->stats.tx_bytes;
	sinfo->rx_bytes   = g_priv->netdev->stats.rx_bytes;
}

static int rcp_cfg80211_get_station(struct wiphy *wiphy,
				    struct net_device *dev,
				    const u8 *mac,
				    struct station_info *sinfo)
{
	int ret = rcp_query_iface_status();

	if (ret)
		return ret;

	/* Only the associated AP is a valid station in STA mode. Reject any
	 * other MAC so "station get <bogus-mac>" returns "not found". */
	if (!ether_addr_equal(mac, g_priv->iface_status.bssid))
		return -ENOENT;

	rcp_fill_station_info(sinfo);
	return 0;
}

/* iw dev wlan1 station dump — iterates associated stations.
 * In STA mode there is exactly one: the AP (index 0). */
static int rcp_cfg80211_dump_station(struct wiphy *wiphy,
				     struct net_device *dev,
				     int idx, u8 *mac,
				     struct station_info *sinfo)
{
	int ret;

	if (idx != 0)
		return -ENOENT;

	ret = rcp_query_iface_status();
	if (ret)
		return ret;

	memcpy(mac, g_priv->iface_status.bssid, ETH_ALEN);
	rcp_fill_station_info(sinfo);
	return 0;
}

static int rcp_cfg80211_dump_survey(struct wiphy *wiphy, struct net_device *dev,
				    int idx, struct survey_info *survey)
{
	static const enum nl80211_band bands[] = {
		NL80211_BAND_2GHZ,
		NL80211_BAND_5GHZ,
	};
	int i, offset = 0;
	struct ieee80211_supported_band *band;

	for (i = 0; i < ARRAY_SIZE(bands); i++) {
		band = wiphy->bands[bands[i]];
		if (!band)
			continue;
		if (idx - offset < band->n_channels) {
			survey->channel = &band->channels[idx - offset];
			survey->filled  = SURVEY_INFO_NOISE_DBM;
			survey->noise   = -95;
			return 0;
		}
		offset += band->n_channels;
	}

	return -ENOENT;
}

void rcp_reg_notifier(struct wiphy *wiphy, struct regulatory_request *request)
{
	struct rcp_priv *priv = wiphy_priv(wiphy);
	struct rcp_wifi_reg_domain reg_domain = {};

	if (request->initiator != NL80211_REGDOM_SET_BY_USER)
		return;

	/* Defensive: don't push to QCC730 before the WIFI_MGMT link is synced
	 * (e.g. a stray early hint). User 'iw reg set' always happens post-sync. */
	if (!priv->mgmt_ready)
		return;

	reg_domain.country_code[0] = request->alpha2[0];
	reg_domain.country_code[1] = request->alpha2[1];
	reg_domain.force_update = true;
	reg_domain.channel_info_count = 0;

	rcp_wifi_set_reg_domain(&reg_domain);
}

/* nl80211 TX power over WIFI_MGMT. Firmware sets an absolute dBm; no auto mode.
 * Policy (chip-specific encoding from chip_caps): FIXED -> FREERUN (force value,
 * no regulatory check), else SAFE (obey regulatory/CTL/target limits). */
static int rcp_cfg80211_set_tx_power(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
				     enum nl80211_tx_power_setting type, int mbm)
{
	struct rcp_priv *priv = wiphy_priv(wiphy);
	uint8_t txpower, policy;

	/* Don't issue a proto request while the WIFI_MGMT link is not ready
	 * (probe before sync, or teardown). */
	if (!priv->mgmt_ready)
		return -EAGAIN;

	if (type == NL80211_TX_POWER_AUTOMATIC)
		return -EOPNOTSUPP;

	txpower = (uint8_t)(mbm / 100);	/* mBm -> dBm */
	policy  = (type == NL80211_TX_POWER_FIXED)
			  ? priv->chip_caps->txpow_policy_freerun
			  : priv->chip_caps->txpow_policy_safe;

	return (rcp_wifi_set_tx_power(0, txpower, policy) == RCP_WIFI_OK)
		       ? 0 : -EIO;
}

static int rcp_cfg80211_get_tx_power(struct wiphy *wiphy,
				     struct wireless_dev *wdev, int *dbm)
{
	struct rcp_priv *priv = wiphy_priv(wiphy);

	if (!priv->mgmt_ready)
		return -EAGAIN;

	/* Synchronous round-trip: send the GET, then wait for the response
	 * event (RCP_WIFI_TX_POWER_RESP) to fill priv->tx_power. */
	reinit_completion(&priv->tx_power_done);
	priv->tx_power_valid = false;

	if (rcp_wifi_get_tx_power(0) != RCP_WIFI_OK)
		return -EIO;

	if (!wait_for_completion_timeout(&priv->tx_power_done, HZ))
		return -ETIMEDOUT;
	if (!priv->tx_power_valid)
		return -EIO;

	*dbm = priv->tx_power.real_power;
	return 0;
}

/* iw phy <phy> set rts <val|off> — cfg80211 caches the value in
 * wiphy->rts_threshold and notifies us via this callback. QCC730 only does
 * RTS/CTS on/off; the proto carries the full threshold and the QCC730 side
 * degrades it to a boolean. */
static int rcp_cfg80211_set_wiphy_params(struct wiphy *wiphy, u32 changed)
{
	struct rcp_priv *priv = wiphy_priv(wiphy);

	if (!(changed & WIPHY_PARAM_RTS_THRESHOLD))
		return 0;	/* nothing we handle changed */

	/* Don't issue a synchronous send while the WIFI_MGMT link is not ready
	 * (probe before sync, or teardown), or it blocks for the full timeout. */
	if (!priv->mgmt_ready)
		return -EAGAIN;

	return (rcp_wifi_set_rts_threshold(0, wiphy->rts_threshold) == RCP_WIFI_OK)
		       ? 0 : -EIO;
}

/* iw dev <dev> info — report the current operating channel. STA channel is
 * whatever the associated AP uses; query it live from QCC730's iface status.
 * iface_status has no bandwidth field, so report 20 MHz (NL80211_CHAN_NO_HT). */
static int rcp_cfg80211_get_channel(struct wiphy *wiphy,
				    struct wireless_dev *wdev,
				    unsigned int link_id,
				    struct cfg80211_chan_def *chandef)
{
	struct ieee80211_channel *chan;
	enum nl80211_band nl_band;
	int freq;

	if (!g_priv)
		return -ENODEV;

	reinit_completion(&g_priv->iface_status_done);
	rcp_wifi_get_iface_status(0);
	wait_for_completion_timeout(&g_priv->iface_status_done, HZ);

	if (!g_priv->iface_status_valid || g_priv->iface_status.channel == 0)
		return -ENODATA;

	nl_band = rcp_to_nl_band(g_priv->iface_status.band);
	freq = ieee80211_channel_to_frequency((int)g_priv->iface_status.channel,
					      nl_band);
	chan = ieee80211_get_channel(wiphy, freq);
	if (!chan)
		return -ENODATA;

	cfg80211_chandef_create(chandef, chan, NL80211_CHAN_NO_HT);
	return 0;
}

/* iw dev <dev> set power_save on/off — toggle BMPS. `timeout` (ms) is the BMPS
 * idle timeout; -1 = firmware default (set_ps_config clamps negatives). */
static int rcp_cfg80211_set_power_mgmt(struct wiphy *wiphy, struct net_device *dev,
				       bool enabled, int timeout)
{
	struct rcp_priv *priv = wiphy_priv(wiphy);
	unsigned long flags;
	enum rcp_wifi_status st;

	if (!priv->mgmt_ready)
		return -EAGAIN;

	pr_info("rcp: set_power_mgmt enabled=%d timeout=%d\n", enabled, timeout);

	/* Wake the 730 before the PS command: if power save was on, it may be in
	 * BMPS long-sleep and the write would hit an unresponsive QcSPI.
	 * qcc730_wakeup() no-ops when already AWAKE. */
	rcp_bus_wakeup_qcc730();

	st = rcp_wifi_set_ps_config(0, enabled, timeout);
	if (st != RCP_WIFI_OK)
		return -EIO;

	/* Track power state for the next wakeup decision: enable -> ASLEEP
	 * (730 enters BMPS), disable -> AWAKE. */
	spin_lock_irqsave(&priv->pwr_lock, flags);
	priv->pwr_state = enabled ? RCP_PWR_ASLEEP : RCP_PWR_AWAKE;
	spin_unlock_irqrestore(&priv->pwr_lock, flags);

	return 0;
}

struct cfg80211_ops rcp_cfg80211_ops = {
	.scan            = rcp_cfg80211_scan,
	.change_virtual_intf = rcp_cfg80211_change_iface,
	.add_key         = rcp_cfg80211_add_key,
	.del_key         = rcp_cfg80211_del_key,
	.set_default_key = rcp_cfg80211_set_default_key,
	.connect         = rcp_cfg80211_connect,
	.disconnect      = rcp_cfg80211_disconnect,
	.start_ap        = rcp_cfg80211_start_ap,
	.stop_ap         = rcp_cfg80211_stop_ap,
	.del_station     = rcp_cfg80211_del_station,
	.get_station     = rcp_cfg80211_get_station,
	.dump_station    = rcp_cfg80211_dump_station,
	.dump_survey     = rcp_cfg80211_dump_survey,
	.set_tx_power    = rcp_cfg80211_set_tx_power,
	.get_tx_power    = rcp_cfg80211_get_tx_power,
	.set_wiphy_params = rcp_cfg80211_set_wiphy_params,
	.get_channel     = rcp_cfg80211_get_channel,
	.set_power_mgmt  = rcp_cfg80211_set_power_mgmt,
};
