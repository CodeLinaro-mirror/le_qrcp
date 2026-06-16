/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/etherdevice.h>

#include "rcp_wifi_cfg80211.h"
#include <rcp_interface.h>
#include <rcp_interface_impl.h>
#include <rcp_event.h>
#include <buffer_mgmt.h>

/* ndo_start_xmit can run in softirq/atomic context (e.g. TCP delayed-ACK
 * fired from an hrtimer), where get_buffer()/link_send() — which take a
 * sleeping mutex — must not be called. So this only enqueues the skb (the
 * sk_buff_head lock is atomic-safe) and wakes the event loop kthread, which
 * drains the queue in process context via rcp_tx_drain_skb_q(). */
static netdev_tx_t rcp_ndo_start_xmit(struct sk_buff *skb,
				       struct net_device *dev)
{
	struct rcp_priv *priv = g_priv;

	if (!priv) {
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	skb_queue_tail(&priv->tx_skb_q, skb);

	/* Wake the event loop (up() on the RCP sem — atomic-context safe). */
	rcp_events_set(RCP_EVENT_TX_DATA);
	return NETDEV_TX_OK;
}

/* Drain queued TX skbs into RCP buffers. Runs only in the event loop kthread
 * (process context), so the sleeping get_buffer()/link_send() are legal here.
 * On buffer-pool exhaustion, requeue the skb at the head and stop the netdev
 * queue; the event loop re-wakes it after pop_next() frees a buffer. */
void rcp_tx_drain_skb_q(struct rcp_priv *priv)
{
	struct net_device *dev = priv->netdev;
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&priv->tx_skb_q)) != NULL) {
		rcp_buffer_hdl_t *buf = get_buffer(skb->len + 2);

		if (!buf) {
			/* Pool empty: put it back and stop the queue until drained. */
			skb_queue_head(&priv->tx_skb_q, skb);
			if (dev)
				netif_stop_queue(dev);
			return;
		}

		buf->data_ptr[0] = 1U; /* PROXY_WIFI_DATA_HDR_VERSION */
		/* Route to SoftAP iface when in AP mode; STA and AP net_contexts
		 * are separate and the STA iface is down while AP is up. */
		buf->data_ptr[1] = (dev && dev->ieee80211_ptr->iftype == NL80211_IFTYPE_AP) ?
				   2U /* PROXY_WIFI_IFACE_AP */ : 1U /* PROXY_WIFI_IFACE_STA */;
		memcpy(buf->data_ptr + 2, skb->data, skb->len);
		buf->size = skb->len + 2;

		if (link_send(RCP_STREAM_WIFI_DATA, buf) != RCP_LNK_SUCCESS) {
			release_buffer(&buf);
			if (dev)
				dev->stats.tx_dropped++;
		} else if (dev) {
			dev->stats.tx_packets++;
			dev->stats.tx_bytes += skb->len;
		}

		dev_kfree_skb(skb);
	}
}

uint8_t wifi_data_rx_handler(enum rcp_rx_status status,
			     rcp_buffer_hdl_t *payload,
			     rcp_error_info_t *err_info)
{
	struct rcp_priv *priv = g_priv;
	struct sk_buff *skb;
	struct net_device *netdev;
	const uint8_t *frame;
	uint16_t frame_len;

	ARG_UNUSED(status);
	ARG_UNUSED(err_info);

	if (!priv || !priv->netdev || !payload)
		return RCP_RX_ACK;

	/* Strip the 2-byte proxy header added by proxy_wifi_data.c on QCC730:
	 *   byte[0] = PROXY_WIFI_DATA_HDR_VERSION (1)
	 *   byte[1] = iface_index
	 * Without this, eth_type_trans() parses garbage as the Ethernet header. */
	frame     = payload->data_ptr;
	frame_len = payload->size;
	if (frame_len > 2 && frame[0] == 1U /* PROXY_WIFI_DATA_HDR_VERSION */) {
		frame     += 2;
		frame_len -= 2;
	}

	netdev = priv->netdev;

	skb = dev_alloc_skb(frame_len + NET_IP_ALIGN);
	if (!skb) {
		netdev->stats.rx_dropped++;
		return RCP_RX_ACK;
	}

	skb_reserve(skb, NET_IP_ALIGN);
	memcpy(skb_put(skb, frame_len), frame, frame_len);
	skb->protocol = eth_type_trans(skb, netdev);

	netdev->stats.rx_packets++;
	netdev->stats.rx_bytes += frame_len;

	netif_rx(skb);
	return RCP_RX_ACK;
}

static int rcp_ndo_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

static int rcp_ndo_stop(struct net_device *dev)
{
	netif_carrier_off(dev);
	netif_stop_queue(dev);
	/* Drop any TX skbs still queued so they are not leaked on iface down. */
	if (g_priv)
		skb_queue_purge(&g_priv->tx_skb_q);
	return 0;
}

const struct net_device_ops rcp_ndo_ops = {
	.ndo_open       = rcp_ndo_open,
	.ndo_stop       = rcp_ndo_stop,
	.ndo_start_xmit = rcp_ndo_start_xmit,
};
