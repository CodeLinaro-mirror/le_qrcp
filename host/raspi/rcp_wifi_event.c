/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <linux/module.h>
#include <linux/ieee80211.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <net/cfg80211.h>

#include "rcp_wifi_cfg80211.h"
#include <wifi/rcp_wifi_mgmt.h>

/* Grace window for the firmware fail-then-success connect quirk. The observed
 * gap between the spurious CONN_FAIL and the real CONN_SUCCESS is ~5 s; 8 s
 * leaves margin without leaving a genuine failure pending too long. */
#define RCP_CONNECT_FAIL_GRACE_MS   8000

/* Report a successful connect to cfg80211 and consume the BSS reference.
 * Security (4-way/SAE) is offloaded to QCC730, so signal port-authorized too,
 * else wpa_supplicant times out and disconnects. */
static void rcp_connect_done(struct net_device *netdev, struct wiphy *wiphy,
			     struct cfg80211_bss *bss)
{
	u8 bssid[ETH_ALEN];

	/* cfg80211_connect_bss() consumes our bss ref, so copy the BSSID out
	 * first — bss must not be dereferenced or put afterwards. */
	ether_addr_copy(bssid, bss->bssid);
	cfg80211_connect_bss(netdev, bssid, bss, NULL, 0, NULL, 0,
			     WLAN_STATUS_SUCCESS, GFP_KERNEL,
			     NL80211_TIMEOUT_UNSPECIFIED);
	cfg80211_port_authorized(netdev, bssid, NULL, 0, GFP_KERNEL);
	netif_carrier_on(netdev);
}

/* Deferred connect-failure report. Runs RCP_CONNECT_FAIL_GRACE_MS after a
 * firmware CONN_FAIL if no CONN_SUCCESS cancelled it. Reports only if the
 * attempt is still pending AND still the same generation this work was armed
 * for — a success/new-connect/disconnect bumps connect_gen or clears
 * connect_pending, invalidating a stale (possibly already-running) work item. */
void rcp_connect_fail_work(struct work_struct *work)
{
	struct rcp_priv *priv = container_of(to_delayed_work(work),
					     struct rcp_priv, connect_fail_work);
	unsigned long flags;
	bool report;

	spin_lock_irqsave(&priv->lock, flags);
	report = priv->connect_pending && priv->fail_gen == priv->connect_gen;
	if (report)
		priv->connect_pending = false;
	spin_unlock_irqrestore(&priv->lock, flags);

	if (report)
		cfg80211_connect_result(priv->netdev, NULL, NULL, 0, NULL, 0,
					WLAN_STATUS_UNSPECIFIED_FAILURE,
					GFP_KERNEL);
}

void rcp_wifi_event_to_cfg80211(struct rcp_priv *priv, int evt, void *data)
{
	struct net_device *netdev = priv->netdev;
	struct wiphy      *wiphy  = priv->wiphy;

	switch ((enum rcp_wifi_evt)evt) {
	case RCP_WIFI_CONNECTED: {
		const struct rcp_wifi_evt_conn *conn = data;
		struct cfg80211_bss *bss;
		unsigned long flags;
		bool success = conn && conn->status == 0;

		/* Firmware fail-then-success quirk: defer a CONN_FAIL while a connect
		 * is pending — reporting it clears cfg80211's ssid_len and breaks a
		 * later success. A success within the grace window cancels it. */
		if (!success) {
			bool pending;

			spin_lock_irqsave(&priv->lock, flags);
			pending = priv->connect_pending;
			if (pending)
				priv->fail_gen = priv->connect_gen;
			spin_unlock_irqrestore(&priv->lock, flags);

			if (pending)
				mod_delayed_work(system_wq, &priv->connect_fail_work,
					msecs_to_jiffies(RCP_CONNECT_FAIL_GRACE_MS));
			else
				cfg80211_connect_result(netdev, NULL, NULL, 0,
							NULL, 0,
							WLAN_STATUS_UNSPECIFIED_FAILURE,
							GFP_KERNEL);
			break;
		}

		/* Success: settle the connect. Bump the generation and clear the
		 * pending guard to invalidate any deferred failure, then cancel it. */
		spin_lock_irqsave(&priv->lock, flags);
		priv->connect_pending = false;
		priv->connect_gen++;
		spin_unlock_irqrestore(&priv->lock, flags);
		cancel_delayed_work(&priv->connect_fail_work);

		/* Retrieve and consume the BSS reference stored in .connect().
		 * If BSS was NULL (no prior scan in this session), try one more
		 * lookup now — it may have been added by a concurrent scan. */
		spin_lock_irqsave(&priv->lock, flags);
		bss = priv->connect_bss;
		priv->connect_bss = NULL;
		spin_unlock_irqrestore(&priv->lock, flags);

		if (!bss && priv->connect_ssid_len) {
			bss = cfg80211_get_bss(wiphy, NULL, NULL,
					       priv->connect_ssid,
					       priv->connect_ssid_len,
					       IEEE80211_BSS_TYPE_ESS,
					       IEEE80211_PRIVACY_ANY);
		}

		if (bss) {
			/* BSS available — complete connect result immediately */
			rcp_connect_done(netdev, wiphy, bss);
		} else {
			/* No BSS in cache. Query iface_status asynchronously for
			 * the BSSID; complete connect result in IFACE_STATUS_RESP. */
			priv->pending_connect_result = true;
			rcp_wifi_get_iface_status(0);
		}
		break;
	}
	case RCP_WIFI_DISCONNECTED: {
		const struct rcp_wifi_evt_conn *conn = data;
		u16 reason = conn ? (u16)conn->status : WLAN_REASON_UNSPECIFIED;

		cfg80211_disconnected(netdev, reason, NULL, 0, true, GFP_KERNEL);
		netif_carrier_off(netdev);
		break;
	}
	case RCP_WIFI_SCAN_RESULT: {
		const struct rcp_wifi_evt_scan_result *r = data;
		struct cfg80211_bss *bss;
		struct ieee80211_channel *chan;
		/* IE buffer: SSID IE (2+32) + RSN IE (2+22 max) */
		u8 ie[2 + WIFI_MAX_SSID_SIZE + 24];
		u16 ie_len;
		int freq;

		if (!r)
			break;

		/* Determine frequency: channels 1-14 are 2.4 GHz, rest 5 GHz */
		if (r->channel > 14)
			freq = ieee80211_channel_to_frequency(r->channel,
							      NL80211_BAND_5GHZ);
		else
			freq = ieee80211_channel_to_frequency(r->channel,
							      NL80211_BAND_2GHZ);
		chan = ieee80211_get_channel(wiphy, freq);
		if (!chan)
			break;

		/* Build SSID IE */
		ie[0] = WLAN_EID_SSID;
		ie[1] = (u8)min_t(u32, r->ssid.size, WIFI_MAX_SSID_SIZE);
		memcpy(&ie[2], r->ssid.bytes, ie[1]);
		ie_len = 2 + ie[1];

		/* Append RSN IE so wpa_supplicant can match security policy.
		 * Without this, all BSS entries look open and wpa_supplicant
		 * will never issue a WPA2/WPA3 connect for them. */
		switch (r->sec_type) {
		case RCP_WIFI_SEC_TYPE_PSK:       /* WPA2-PSK */
		case RCP_WIFI_SEC_TYPE_WPA_PSK:   /* WPA-PSK */
		case RCP_WIFI_SEC_TYPE_WPA_AUTO_PERSONAL: {
			/* Minimal RSN IE: WPA2-PSK, CCMP group+pairwise */
			static const u8 rsn_wpa2_psk[] = {
				0x30, 0x14,             /* RSN IE, len=20 */
				0x01, 0x00,             /* version 1 */
				0x00, 0x0f, 0xac, 0x04, /* group: CCMP */
				0x01, 0x00,             /* 1 pairwise */
				0x00, 0x0f, 0xac, 0x04, /* pairwise: CCMP */
				0x01, 0x00,             /* 1 AKM */
				0x00, 0x0f, 0xac, 0x02, /* AKM: PSK */
				0x00, 0x00,             /* RSN caps */
			};
			memcpy(&ie[ie_len], rsn_wpa2_psk, sizeof(rsn_wpa2_psk));
			ie_len += sizeof(rsn_wpa2_psk);
			break;
		}
		case RCP_WIFI_SEC_TYPE_SAE:       /* WPA3-SAE */
		case RCP_WIFI_SEC_TYPE_SAE_H2E:
		case RCP_WIFI_SEC_TYPE_SAE_AUTO: {
			/* Minimal RSN IE: WPA3-SAE, CCMP, MFP required */
			static const u8 rsn_wpa3_sae[] = {
				0x30, 0x14,
				0x01, 0x00,
				0x00, 0x0f, 0xac, 0x04,
				0x01, 0x00,
				0x00, 0x0f, 0xac, 0x04,
				0x01, 0x00,
				0x00, 0x0f, 0xac, 0x08, /* AKM: SAE */
				0xc0, 0x00,             /* MFP required (bit6+7) */
			};
			memcpy(&ie[ie_len], rsn_wpa3_sae, sizeof(rsn_wpa3_sae));
			ie_len += sizeof(rsn_wpa3_sae);
			break;
		}
		default:
			break;
		}

		/* kernel 6.12: wiphy_lock must be held for cfg80211_inform_bss,
		 * else the BSS list races with disconnect cleanup and crashes. */
		{
			u16 cap = WLAN_CAPABILITY_ESS;

			if (r->sec_type != RCP_WIFI_SEC_TYPE_NONE)
				cap |= WLAN_CAPABILITY_PRIVACY;

			wiphy_lock(wiphy);
			bss = cfg80211_inform_bss(wiphy, chan,
						  CFG80211_BSS_FTYPE_UNKNOWN,
						  r->bssid, 0,
						  cap, 100,
						  ie, ie_len,
						  (s32)r->rssi * 100, GFP_KERNEL);
			if (bss)
				cfg80211_put_bss(wiphy, bss);
			wiphy_unlock(wiphy);
		}
		break;
	}
	case RCP_WIFI_SCAN_DONE: {
		struct cfg80211_scan_info info = { .aborted = false };
		struct cfg80211_scan_request *req;
		unsigned long flags;

		spin_lock_irqsave(&priv->lock, flags);
		req = priv->scan_request;
		priv->scan_request = NULL;
		spin_unlock_irqrestore(&priv->lock, flags);

		if (req) {
			/* cfg80211_scan_done() must hold wiphy_lock (kernel 6.1+)
			 * to synchronize with other wiphy operations. */
			wiphy_lock(wiphy);
			cfg80211_scan_done(req, &info);
			wiphy_unlock(wiphy);
		}
		break;
	}
	case RCP_WIFI_AP_ENABLED:
		pr_info("rcp: AP enabled\n");
		/* SoftAP is up: firmware has now assigned the SAP MAC (still all-zero
		 * at MGMT bring-up). Re-query L2_INTERFACES; the RESP handler aligns
		 * the netdev MAC to it. */
		rcp_wifi_get_l2_interfaces(RCP_NET_IF_TYPE_WIFI, 0);
		break;
	case RCP_WIFI_AP_DISABLED:
		pr_info("rcp: AP disabled\n");
		break;
	case RCP_WIFI_AP_STA_CONNECTED: {
		const struct rcp_wifi_evt_ap_sta *sta = data;
		struct station_info sinfo = { 0 };
		/* The firmware AP STA-connected event carries no (Re)Assoc IEs, so
		 * WPA2 hostapd sees "No WPA/RSN IE from STA" and kicks it. Report a
		 * matching RSN IE via assoc_req_ies so the STA is accepted. */
		static const u8 rsn_wpa2_psk[] = {
			0x30, 0x14,             /* RSN IE, len=20 */
			0x01, 0x00,             /* version 1 */
			0x00, 0x0f, 0xac, 0x04, /* group: CCMP */
			0x01, 0x00,             /* 1 pairwise */
			0x00, 0x0f, 0xac, 0x04, /* pairwise: CCMP */
			0x01, 0x00,             /* 1 AKM */
			0x00, 0x0f, 0xac, 0x02, /* AKM: PSK */
			0x00, 0x00,             /* RSN caps */
		};

		if (!sta)
			break;

		/* Only WPA2-PSK needs the synthesized IE; an Open AP works
		 * without one (and must not advertise RSN). */
		if (g_priv && g_priv->ap_sec_type == RCP_WIFI_SEC_TYPE_PSK) {
			sinfo.assoc_req_ies     = rsn_wpa2_psk;
			sinfo.assoc_req_ies_len = sizeof(rsn_wpa2_psk);
		}

		/* sinfo must be non-NULL: nl80211_send_station() dereferences
		 * it unconditionally on kernel 6.12. */
		cfg80211_new_sta(netdev, sta->sta_mac_address, &sinfo,
				 GFP_KERNEL);

		/* Firmware is the AP authenticator: the STA's port is already open by
		 * AP_STA_CONNECTED, but hostapd would leave it at [AUTH][ASSOC]. Emit
		 * PORT_AUTHORIZED (STA MAC in AP mode) so hostapd marks it [AUTHORIZED]. */
		cfg80211_port_authorized(netdev, sta->sta_mac_address, NULL, 0,
					 GFP_KERNEL);
		break;
	}
	case RCP_WIFI_AP_STA_DISCONNECTED: {
		const struct rcp_wifi_evt_ap_sta *sta = data;

		if (sta)
			cfg80211_del_sta(netdev, sta->sta_mac_address, GFP_KERNEL);
		break;
	}
	case RCP_WIFI_IFACE_STATUS_RESP: {
		const struct rcp_wifi_evt_iface_status *s = data;

		if (s && g_priv) {
			g_priv->iface_status = s->status;
			g_priv->iface_status_valid = true;
			complete(&g_priv->iface_status_done);
		}

		/* Complete deferred connect result if pending (triggered when
		 * RCP_WIFI_CONNECTED arrived but BSS was not in cfg80211 cache).
		 * We now have the BSSID and channel from QCC730's iface_status. */
		if (!priv->pending_connect_result || !s)
			break;
		priv->pending_connect_result = false;

		{
			struct cfg80211_bss *bss = NULL;
			const uint8_t *bssid = s->status.bssid;
			struct ieee80211_channel *chan = NULL;
			enum nl80211_band nl_band;
			int freq;

			nl_band = rcp_to_nl_band(s->status.band);
			freq = ieee80211_channel_to_frequency(
					(int)s->status.channel, nl_band);
			chan = ieee80211_get_channel(wiphy, freq);

			/* wiphy_lock for get_bss/inform_bss (see scan-result path).
			 * cfg80211_connect_bss() below locks itself, so keep it
			 * OUTSIDE this region. */
			if (chan) {
				wiphy_lock(wiphy);

				/* Try BSS lookup by BSSID first (may exist from
				 * a QCC730-internal scan triggered by connect). */
				bss = cfg80211_get_bss(wiphy, chan, bssid,
						       NULL, 0,
						       IEEE80211_BSS_TYPE_ESS,
						       IEEE80211_PRIVACY_ANY);

				if (!bss) {
					/* Inject minimal BSS: SSID IE so that
					 * __cfg80211_connect_result completes. */
					u8 ie[2 + WIFI_MAX_SSID_SIZE];
					u8 ssid_len = (u8)min_t(size_t,
							priv->connect_ssid_len,
							WIFI_MAX_SSID_SIZE);

					ie[0] = WLAN_EID_SSID;
					ie[1] = ssid_len;
					memcpy(&ie[2], priv->connect_ssid, ssid_len);

					bss = cfg80211_inform_bss(
						wiphy, chan,
						CFG80211_BSS_FTYPE_UNKNOWN,
						bssid, 0,
						WLAN_CAPABILITY_ESS, 100,
						ie, 2 + ssid_len,
						(s32)s->status.rssi * 100,
						GFP_KERNEL);
				}

				wiphy_unlock(wiphy);
			}

			if (bss) {
				rcp_connect_done(netdev, wiphy, bss);
			} else {
				/* Last resort: pass BSSID without BSS.
				 * chan was NULL (unregistered channel?). */
				pr_warn("rcp: pending_connect: chan NULL (ch=%u band=%d), bssid-only fallback\n",
					(unsigned)s->status.channel,
					(int)s->status.band);
				cfg80211_connect_result(netdev, bssid,
							NULL, 0, NULL, 0,
							WLAN_STATUS_SUCCESS,
							GFP_KERNEL);
				cfg80211_port_authorized(netdev, bssid,
							 NULL, 0, GFP_KERNEL);
				netif_carrier_on(netdev);
			}
		}
		break;
	}
	case RCP_WIFI_TX_POWER_RESP: {
		const struct rcp_wifi_evt_tx_power *tp = data;

		if (tp && g_priv) {
			g_priv->tx_power = *tp;
			g_priv->tx_power_valid = true;
			complete(&g_priv->tx_power_done);
		}
		break;
	}
	case RCP_WIFI_STATUS_RESP:
		break;
	case RCP_WIFI_L2_INTERFACES_RESP: {
		const struct rcp_wifi_l2_interfaces *ifs = data;
		uint32_t i;

		if (!ifs || !ifs->count)
			break;

		/* QCC730 reports the STA (index 1) and SAP (index 2) MACs. Align the
		 * netdev MAC to the egress iface (SAP in AP mode, STA otherwise), else
		 * the downlink source MAC is wrong and clients drop the replies. */
		for (i = 0; i < ifs->count; i++) {
			if (ifs->net_ifs[i].net_if_type != RCP_NET_IF_TYPE_WIFI)
				continue;
			if (ifs->net_ifs[i].index == 2) {
				memcpy(priv->ap_mac, ifs->net_ifs[i].mac_address,
				       ETH_ALEN);
				priv->ap_mac_valid = true;
				pr_info("rcp: L2_INTERFACES: AP MAC %pM\n",
					ifs->net_ifs[i].mac_address);
				if (netdev->ieee80211_ptr->iftype == NL80211_IFTYPE_AP &&
				    !is_zero_ether_addr(priv->ap_mac))
					dev_addr_set(netdev, priv->ap_mac);
			} else {
				memcpy(priv->sta_mac, ifs->net_ifs[i].mac_address,
				       ETH_ALEN);
				pr_info("rcp: L2_INTERFACES: STA MAC %pM\n",
					ifs->net_ifs[i].mac_address);
				if (netdev->ieee80211_ptr->iftype != NL80211_IFTYPE_AP)
					dev_addr_set(netdev, ifs->net_ifs[i].mac_address);
			}
		}
		/* WIFI_MGMT stream is now synced — vendor commands are usable. */
		priv->mgmt_ready = true;
		break;
	}
	case RCP_WIFI_VERSION:
	case RCP_WIFI_REG_DOMAIN:
	case RCP_TEST_RESULT:
	default:
		break;
	}
}
