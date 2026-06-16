/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * RPi Linux cfg80211 driver for QCC730 RCP WiFi: module init, SPI probe/remove,
 * wiphy setup.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/wireless.h>
#include <linux/ieee80211.h>
#include <linux/kthread.h>
#include <linux/gpio/consumer.h>
#include <linux/of_irq.h>
#include <linux/of_device.h>
#include <linux/rtnetlink.h>
#include <net/cfg80211.h>

#include "rcp_user_config.h"
#include "rcp_wifi_cfg80211.h"
#include "rcp_bus_spi_master_linux_qcc730.h"
#include "rcp_vendor_cmds.h"
#include "rcp_bench.h"

#include <rcp_interface_impl.h>
#include <rcp_cntrl_msgs.h>
#include <rcp_event.h>
#include <wifi/rcp_wifi_mgmt.h>

/*
 * Benchmark mode — selected at insmod time via the bench_mode module param
 * (see rcp_bench.h). bench_mode=0 is the normal WiFi path.
 */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Qualcomm Technologies, Inc.");
MODULE_DESCRIPTION("QCC730 RCP WiFi cfg80211 driver");

const uint32_t rcp_cipher_suites[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
	/* BIP group-mgmt cipher: signals MFP/PMF support to wpa_supplicant.
	 * Without it, supplicant rejects MFP-required WPA3-SAE APs with
	 * "no mgmt frame protection enabled but AP requires it". */
	WLAN_CIPHER_SUITE_AES_CMAC,
};
int rcp_cipher_suites_count = ARRAY_SIZE(rcp_cipher_suites);

/* AKM suites advertised to wpa_supplicant. Without SAE here, supplicant's
 * get_capability(key_mgmt) omits SAE, so it never selects a WPA3 BSS and
 * loops in SCANNING (never calls .connect). */
static const u32 rcp_akm_suites[] = {
	WLAN_AKM_SUITE_PSK,
	WLAN_AKM_SUITE_SAE,
};

/* AP mode supports WPA2-PSK only (SAE deferred); advertise PSK on the AP iftype
 * so hostapd accepts the 4-way-handshake PSK offload. */
static const u32 rcp_ap_akm_suites[] = {
	WLAN_AKM_SUITE_PSK,
};

static const struct wiphy_iftype_akm_suites rcp_iftype_akm[] = {
	{
		.iftypes_mask = BIT(NL80211_IFTYPE_STATION),
		.akm_suites   = rcp_akm_suites,
		.n_akm_suites = ARRAY_SIZE(rcp_akm_suites),
	},
	{
		.iftypes_mask = BIT(NL80211_IFTYPE_AP),
		.akm_suites   = rcp_ap_akm_suites,
		.n_akm_suites = ARRAY_SIZE(rcp_ap_akm_suites),
	},
};

struct rcp_priv *g_priv;

/* RCP WiFi event callback - invoked from rcp_protocol event loop */
static void rcp_wifi_evt_cb(enum rcp_wifi_evt evt, const void *data)
{
	if (g_priv)
		rcp_wifi_event_to_cfg80211(g_priv, evt, (void *)data);
}

static int rcp_event_loop_thread(void *arg)
{
	struct rcp_priv *priv = arg;

	/* Elevate to SCHED_FIFO to minimise wake-up latency after the GPIO IRQ. */
	sched_set_fifo(current);
	while (!kthread_should_stop()) {
		run_event_loop_once();
		/* Drain skbs enqueued by ndo_start_xmit (which cannot do the
		 * sleeping get_buffer()/link_send() in its atomic context). */
		rcp_tx_drain_skb_q(priv);
		/* Wake TX queue if it was stopped due to buffer pool exhaustion.
		 * run_event_loop_once() calls pop_next() which frees TX buffers. */
		if (priv->netdev && netif_queue_stopped(priv->netdev) && has_free_buffer())
			netif_wake_queue(priv->netdev);
	}
	return 0;
}

/* Assemble a writable supported_band from the chip caps. cfg80211 mutates
 * channel flags (reg/DFS) at runtime, so channels must be a writable kmemdup,
 * not the const caps template. Returns 0 or -ENOMEM. */
static int rcp_build_band(struct rcp_priv *priv)
{
	const struct rcp_chip_caps *c = priv->chip_caps;

	priv->chans_2g = kmemdup(c->chans_2g,
				 c->n_chans_2g * sizeof(*c->chans_2g),
				 GFP_KERNEL);
	priv->chans_5g = kmemdup(c->chans_5g,
				 c->n_chans_5g * sizeof(*c->chans_5g),
				 GFP_KERNEL);
	priv->rates_bg = kmemdup(c->rates_bg,
				 c->n_rates_bg * sizeof(*c->rates_bg),
				 GFP_KERNEL);
	priv->rates_ofdm = kmemdup(c->rates_ofdm,
				   c->n_rates_ofdm * sizeof(*c->rates_ofdm),
				   GFP_KERNEL);
	if (!priv->chans_2g || !priv->chans_5g ||
	    !priv->rates_bg || !priv->rates_ofdm)
		return -ENOMEM;

	priv->band_2g.band       = NL80211_BAND_2GHZ;
	priv->band_2g.channels   = priv->chans_2g;
	priv->band_2g.n_channels = c->n_chans_2g;
	priv->band_2g.bitrates   = priv->rates_bg;
	priv->band_2g.n_bitrates = c->n_rates_bg;

	priv->band_5g.band       = NL80211_BAND_5GHZ;
	priv->band_5g.channels   = priv->chans_5g;
	priv->band_5g.n_channels = c->n_chans_5g;
	priv->band_5g.bitrates   = priv->rates_ofdm;
	priv->band_5g.n_bitrates = c->n_rates_ofdm;

	if (c->ht_supported) {
		struct ieee80211_sta_ht_cap ht = {0};

		ht.ht_supported  = true;
		ht.cap           = c->ht_cap_flags;
		ht.ampdu_factor  = c->ht_ampdu_factor;
		ht.ampdu_density = c->ht_ampdu_density;
		ht.mcs.rx_mask[0] = c->ht_mcs_rx_mask0;
		ht.mcs.tx_params  = IEEE80211_HT_MCS_TX_DEFINED;
		priv->band_2g.ht_cap = ht;
		priv->band_5g.ht_cap = ht;
	}
	return 0;
}

static int rcp_wifi_probe(struct spi_device *spi)
{
	struct wiphy       *wiphy;
	struct net_device  *netdev;
	struct rcp_priv    *priv;
	int irq, ret;

	wiphy = wiphy_new(&rcp_cfg80211_ops, sizeof(struct rcp_priv));
	if (!wiphy)
		return -ENOMEM;

	priv = wiphy_priv(wiphy);
	priv->wiphy = wiphy;
	priv->spi   = spi;

	priv->chip_caps = of_device_get_match_data(&spi->dev);
	if (!priv->chip_caps) {
		dev_err(&spi->dev, "No chip caps (of_match data missing)\n");
		ret = -ENODEV;
		goto err_wiphy;
	}

	spin_lock_init(&priv->lock);
	init_waitqueue_head(&priv->wait_queue);
	skb_queue_head_init(&priv->tx_skb_q);
	atomic_set(&priv->rx_events, 0);
	init_completion(&priv->iface_status_done);
	init_completion(&priv->tx_power_done);
	INIT_DELAYED_WORK(&priv->connect_fail_work, rcp_connect_fail_work);
	g_priv = priv;

	/* Get IRQ number from device tree "interrupts" property */
	irq = of_irq_get(spi->dev.of_node, 0);
	if (irq < 0) {
		dev_err(&spi->dev, "No IRQ configured (err %d)\n", irq);
		ret = irq;
		goto err_wiphy;
	}

	/* Initialize RCP bus + protocol layer */
	ret = rcp_spi_master_probe(spi, NULL, irq);
	if (ret < 0)
		goto err_bus;

	ret = init_rcp();
	if (ret < 0)
		goto err_bus;

	if (rcp_bench_mode()) {
		/* Bench mode: skip WiFi / cfg80211, start benchmark kthread */
		rcp_bench_start(priv);
		spi_set_drvdata(spi, priv);
		dev_info(&spi->dev, "QCC730 bench mode %d started\n",
			 rcp_bench_mode());
		return 0;
	}

	/* Register WiFi MGMT RX handler (handle_rcp_msg in rcp_wifi_mgmt.c).
	 * rcp_wifi_init() resets num_cb to 0, so register the event callback
	 * AFTER rcp_wifi_init() to survive the reset. */
	rcp_wifi_init();
	rcp_wifi_register_callback(rcp_wifi_evt_cb);

	/* Register WiFi DATA RX handler so uplink Ethernet frames are
	 * delivered to the Linux network stack instead of being dropped. */
	link_register_rx_handler(RCP_STREAM_WIFI_DATA, wifi_data_rx_handler);

	/* Set up wiphy */
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION) |
				 BIT(NL80211_IFTYPE_AP);
	/* QCC730 firmware is the AP SME/authenticator (runs the 4-way handshake,
	 * owns PTK/GTK, builds the RSN beacon). Advertising HAVE_AP_SME makes
	 * hostapd run in full-offload mode and pass the PSK via start_ap's
	 * crypto.psk instead of running the software authenticator. */
	wiphy->flags |= WIPHY_FLAG_HAVE_AP_SME;
	ret = rcp_build_band(priv);
	if (ret)
		goto err_bus;
	wiphy->bands[NL80211_BAND_2GHZ] = &priv->band_2g;
	wiphy->bands[NL80211_BAND_5GHZ] = &priv->band_5g;
	wiphy->available_antennas_tx = priv->chip_caps->antenna_tx;
	wiphy->available_antennas_rx = priv->chip_caps->antenna_rx;
	wiphy->max_scan_ssids           = 1;
	/* Max length of vendor/extra IEs the supplicant may append to scan
	 * requests. Defaults to 0, which makes the kernel reject any scan
	 * carrying an IE (-EINVAL); wpa_supplicant 2.11 appends an Extended
	 * Capabilities IE, so this must be non-zero for scans to succeed. */
	wiphy->max_scan_ie_len          = 256;
	wiphy->cipher_suites            = rcp_cipher_suites;
	wiphy->n_cipher_suites          = rcp_cipher_suites_count;
	wiphy->signal_type              = CFG80211_SIGNAL_TYPE_MBM;
	/* QCC730 firmware handles WPA2-PSK 4-way handshake internally.
	 * This tells wpa_supplicant not to run EAPOL exchange in userspace. */
	wiphy_ext_feature_set(wiphy, NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK);
	wiphy_ext_feature_set(wiphy, NL80211_EXT_FEATURE_SAE_OFFLOAD);
	/* AP mode: firmware is the WPA2-PSK authenticator. hostapd passes the
	 * precomputed 32-byte PMK via cfg80211_ap_settings.crypto.psk. */
	wiphy_ext_feature_set(wiphy, NL80211_EXT_FEATURE_4WAY_HANDSHAKE_AP_PSK);

	/* Advertise supported AKMs (incl. SAE) so supplicant selects WPA3 BSS.
	 * Set BOTH the global akm_suites (parsed into drv->capa.key_mgmt, used at
	 * association time) and the per-iftype list (used by get_capability).
	 * Without the global list, association fails with
	 * "available key_mgmt 0x0 / Failed to select authenticated key mgmt". */
	wiphy->akm_suites            = rcp_akm_suites;
	wiphy->n_akm_suites          = ARRAY_SIZE(rcp_akm_suites);
	wiphy->iftype_akm_suites     = rcp_iftype_akm;
	wiphy->num_iftype_akm_suites = ARRAY_SIZE(rcp_iftype_akm);

	wiphy->reg_notifier      = rcp_reg_notifier;

	/* Register nl80211 vendor commands (SET/GET TX power, PHY mode, MAC address) */
	wiphy->vendor_commands   = rcp_vendor_commands;
	wiphy->n_vendor_commands = rcp_vendor_commands_count;

	ret = wiphy_register(wiphy);
	if (ret < 0)
		goto err_bus;

	/* Allocate net_device with wlan* naming and link to wdev. */
	netdev = alloc_netdev(0, "wlan%d", NET_NAME_ENUM, ether_setup);
	if (!netdev) {
		ret = -ENOMEM;
		goto err_wiphy_reg;
	}
	priv->netdev = netdev;
	netdev->netdev_ops = &rcp_ndo_ops;
	SET_NETDEV_DEV(netdev, wiphy_dev(wiphy));
	eth_hw_addr_random(netdev);

	/* Wire up wdev so the interface appears as wlan* and cfg80211 commands work */
	priv->wdev.wiphy  = wiphy;
	priv->wdev.iftype = NL80211_IFTYPE_STATION;
	priv->wdev.netdev = netdev;
	netdev->ieee80211_ptr = &priv->wdev;

	rtnl_lock();
	ret = cfg80211_register_netdevice(netdev);
	rtnl_unlock();
	if (ret < 0)
		goto err_netdev;

	/* Start event loop kthread */
	priv->event_thread = kthread_run(rcp_event_loop_thread, priv,
					 "rcp_wifi");
	if (IS_ERR(priv->event_thread)) {
		ret = PTR_ERR(priv->event_thread);
		goto err_netdev_reg;
	}

	spi_set_drvdata(spi, priv);
	dev_info(&spi->dev, "QCC730 RCP WiFi driver probed");

	/* Trigger initial WIFI_MGMT sync by sending get_l2_interfaces.
	 * This queues a message in the TX queue; the event loop kthread will
	 * pick it up and send it, initiating the WIFI_MGMT stream handshake. */
	rcp_wifi_get_l2_interfaces(RCP_NET_IF_TYPE_WIFI, 0);

	return 0;

err_netdev_reg:
	rtnl_lock();
	cfg80211_unregister_netdevice(netdev);
	rtnl_unlock();
err_netdev:
	free_netdev(netdev);
err_wiphy_reg:
	wiphy_unregister(wiphy);
err_bus:
	rcp_spi_master_remove();
err_wiphy:
	/* kfree(NULL) is safe if band was never built */
	kfree(priv->chans_2g);
	kfree(priv->chans_5g);
	kfree(priv->rates_bg);
	kfree(priv->rates_ofdm);
	wiphy_free(wiphy);
	g_priv = NULL;
	return ret;
}

static void rcp_wifi_remove(struct spi_device *spi)
{
	struct rcp_priv *priv = spi_get_drvdata(spi);
	struct wiphy *wiphy;
	struct cfg80211_scan_info scan_info = { .aborted = true };

	if (!priv)
		return;

	if (rcp_bench_mode()) {
		/* Bench mode: stop bench kthread, release bus and wiphy */
		if (priv->event_thread && !IS_ERR(priv->event_thread))
			kthread_stop(priv->event_thread);
		rcp_spi_master_remove();
		wiphy_free(priv->wiphy);
		g_priv = NULL;
		return;
	}

	/* Block any further synchronous vendor commands: the event kthread is
	 * about to stop, so a vendor cmd issued during teardown (e.g. cfg80211
	 * reading tx power on disconnect) would never get a reply and block for
	 * the full timeout. */
	priv->mgmt_ready = false;

	/* Stop the event loop kthread before touching cfg80211 state so no
	 * new events (scan results, connect, disconnect) arrive during teardown. */
	kthread_stop(priv->event_thread);

	/* Cancel any pending deferred connect-failure report so it cannot run
	 * against a half-torn-down wdev after this point. */
	priv->connect_pending = false;
	cancel_delayed_work_sync(&priv->connect_fail_work);

	wiphy = priv->wiphy;

	/* Release cached BSS reference if a connect was in progress */
	if (priv->connect_bss) {
		cfg80211_put_bss(wiphy, priv->connect_bss);
		priv->connect_bss = NULL;
	}

	/* Clean up pending cfg80211 state before unregistering the wdev.
	 * wiphy_lock() serializes with cfg80211's internal work queue and
	 * prevents WARN_ON in _cfg80211_unregister_wdev when the wdev is
	 * still connected or has an active scan at rmmod time. */
	wiphy_lock(wiphy);

	/* Notify cfg80211 of disconnect if still associated */
	if (priv->wdev.connected)
		cfg80211_disconnected(priv->netdev, WLAN_REASON_DEAUTH_LEAVING,
				      NULL, 0, true, GFP_KERNEL);

	/* Abort any in-progress scan */
	if (priv->scan_request) {
		cfg80211_scan_done(priv->scan_request, &scan_info);
		priv->scan_request = NULL;
	}

	wiphy_unlock(wiphy);

	rtnl_lock();
	cfg80211_unregister_netdevice(priv->netdev);
	rtnl_unlock();   /* netdev_run_todo() sets reg_state=NETREG_UNREGISTERED */
	free_netdev(priv->netdev);
	wiphy_unregister(wiphy);
	rcp_spi_master_remove();
	kfree(priv->chans_2g);
	kfree(priv->chans_5g);
	kfree(priv->rates_bg);
	kfree(priv->rates_ofdm);
	wiphy_free(wiphy);
	g_priv = NULL;
}

static const struct spi_device_id rcp_spi_id[] = {
	{ "qcc730-rcp", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, rcp_spi_id);

static const struct of_device_id rcp_of_match[] = {
	{ .compatible = "qcom,qcc730-rcp", .data = &rcp_caps_qcc730 },
	{ },
};
MODULE_DEVICE_TABLE(of, rcp_of_match);

static struct spi_driver rcp_wifi_driver = {
	.driver = {
		.name           = RCP_WIFI_DRIVER_NAME,
		.of_match_table = rcp_of_match,
	},
	.id_table = rcp_spi_id,
	.probe    = rcp_wifi_probe,
	.remove   = rcp_wifi_remove,
};

module_spi_driver(rcp_wifi_driver);
