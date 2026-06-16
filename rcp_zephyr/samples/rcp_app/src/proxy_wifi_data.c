/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/logging/log.h>

#include <zephyr/kernel.h>

#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_context.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <stdint.h>

#include "rcp_mux_dispatcher.h"
#include <rcp_interface.h>
#include <rcp_interface_impl.h>
#include <buffer_mgmt.h>

#include "proxy_wifi_data.h"

#define RX_THREAD_PRIO       K_PRIO_COOP(15)
#define RX_THREAD_STACK_SIZE 1024
/* Max. Ethernet frame + 2-byte Wi-Fi stream header. */
#define RX_ETH_FRAME_SIZE    1514
#define RX_BUF_SIZE          (RX_ETH_FRAME_SIZE + PROXY_WIFI_DATA_HDR_SIZE)

/* Determines delay between subsequent tries in case of TX failure. */
#define RCP_TX_ERR_DELAY_MS   1
/* Amount of retries in case of RCP TX failure. */
#define RCP_TX_RETRIES        5
/* TX completion wait timeout (ms) for net_context send callback. */
#define WIFI_TX_CB_TIMEOUT_MS 1
/* Amount of retries in case of Wi-Fi TX failure. */
#define WIFI_TX_RETRIES       3

LOG_MODULE_REGISTER(wifi_data_proxy, CONFIG_QUALCOMM_RCP_APP_LOG_LEVEL);

/* ---- host->wifi TX path selector ----
 *   0 = BLOCKING: net_context_sendto + per-frame synchronous completion wait
 *                 (original standard net_context path)
 *   1 = DIRECT  : bypass socket/net_context/L2, call the WiFi driver's .send
 *                 (qwifi_drv_send) directly with a locally-built net_pkt.
 *                 Skips ~182us/frame of net_context/L2 wrapper; reaches the
 *                 ~21 Mbps QcSPI ceiling on clean air.
 */
#define RCP_TX_MODE_BLOCKING  0
#define RCP_TX_MODE_DIRECT    1

#ifndef RCP_TX_MODE
#define RCP_TX_MODE RCP_TX_MODE_DIRECT
#endif

/* ---- wifi->host RX path selector ----
 *   0 = NETSTACK: driver net_recv_data -> ethernet L2 -> AF_PACKET socket ->
 *                 wifi_rx_cb -> rx_fifo -> wifi_rx_thread (original path).
 *   1 = DIRECT  : driver rx cb hands the frame straight to
 *                 proxy_wifi_data_rx_direct(), which builds the RCP frame and
 *                 calls link_send() in the Wi-Fi RX thread context. Skips
 *                 net_pkt alloc (the ~510us pool-starvation stall), the net
 *                 stack (~190us) and one full-frame copy. Mirror of TX DIRECT.
 */
#define RCP_RX_MODE_NETSTACK  0
#define RCP_RX_MODE_DIRECT    1

#ifndef RCP_RX_MODE
#define RCP_RX_MODE RCP_RX_MODE_DIRECT
#endif

static struct net_context *sta_ctx;
static struct net_context *ap_ctx;
static K_FIFO_DEFINE(rx_fifo);
static K_MUTEX_DEFINE(tx_lock);
static K_MUTEX_DEFINE(ctx_lock);
#if (RCP_TX_MODE == RCP_TX_MODE_BLOCKING)
static K_SEM_DEFINE(tx_sem, 0, 1);
static int tx_status = 0;
#endif

static void wifi_rx_thread(void *p1, void *p2, void *p3);
static bool init_ctx(struct net_context **ctx, struct net_if *iface);
static bool ensure_ctx_bound_locked(uint8_t iface_index);
static struct net_if *get_iface_by_index(uint8_t iface_index);
#if (RCP_TX_MODE == RCP_TX_MODE_BLOCKING)
static struct net_context *get_ctx_locked(uint8_t iface_index);
static int get_ifindex_by_iface_index(uint8_t iface_index);
#endif
static uint8_t get_iface_index_by_iface(const struct net_if *iface);
static uint8_t normalize_iface_index(uint8_t iface_index);
static uint8_t handle_data_msg(enum rcp_rx_status status, rcp_buffer_hdl_t *payload_ptr,
			       rcp_error_info_t *err_info_ptr);
static void wifi_rx_cb(struct net_context *ctx, struct net_pkt *pkt, union net_ip_header *ip_hdr,
		       union net_proto_header *proto_hdr, int status, void *user_data);
#if (RCP_TX_MODE == RCP_TX_MODE_BLOCKING)
static void wifi_tx_cb(struct net_context *ctx, int status, void *user_data);
#endif
#if (RCP_RX_MODE == RCP_RX_MODE_DIRECT)
/* Called directly (extern) from the wifi driver's rx callback. */
int proxy_wifi_data_rx_direct(uint8_t iface_idx, const uint8_t *frame, uint16_t len);
#endif

static struct k_thread wifi_rx_thread_data;
static k_tid_t wifi_rx_tid;

/* ---- Per-frame Ethernet trace (ARP/DHCP/ICMP) ----
 * Default OFF. Enable by setting RCP_ETH_TRACE_RX/TX to 1.
 */
#ifndef RCP_ETH_TRACE_SUPPORT
#define RCP_ETH_TRACE_SUPPORT 1
#endif

#ifndef RCP_ETH_TRACE_RX
#define RCP_ETH_TRACE_RX 0
#endif

#ifndef RCP_ETH_TRACE_TX
#define RCP_ETH_TRACE_TX 0
#endif

static struct net_if *get_sta_iface(void)
{
	return net_if_get_wifi_sta();
}

static struct net_if *get_ap_iface(void)
{
	return net_if_get_wifi_sap();
}

static uint8_t normalize_iface_index(uint8_t iface_index)
{
	/* Backward compatibility: iface_index=0 means STA. */
	if (iface_index == 0U) {
		return PROXY_WIFI_IFACE_STA;
	}
	return iface_index;
}

static struct net_if *get_iface_by_index(uint8_t iface_index)
{
	iface_index = normalize_iface_index(iface_index);
	if (iface_index == PROXY_WIFI_IFACE_AP) {
		return get_ap_iface();
	}
	return get_sta_iface();
}

#if (RCP_TX_MODE == RCP_TX_MODE_BLOCKING)
static int get_ifindex_by_iface_index(uint8_t iface_index)
{
	struct net_if *iface = get_iface_by_index(iface_index);

	if (!iface) {
		return -1;
	}

	return net_if_get_by_iface(iface);
}
#endif

static uint8_t get_iface_index_by_iface(const struct net_if *iface)
{
	if (!iface) {
		return PROXY_WIFI_IFACE_STA;
	}
	if (iface == get_ap_iface()) {
		return PROXY_WIFI_IFACE_AP;
	}
	if (iface == get_sta_iface()) {
		return PROXY_WIFI_IFACE_STA;
	}

	LOG_WRN("RX: unknown iface %p, defaulting to STA", iface);
	return PROXY_WIFI_IFACE_STA;
}

#if (RCP_TX_MODE == RCP_TX_MODE_BLOCKING)
static struct net_context *get_ctx_locked(uint8_t iface_index)
{
	iface_index = normalize_iface_index(iface_index);
	if (iface_index == PROXY_WIFI_IFACE_AP) {
		return ap_ctx;
	}
	return sta_ctx;
}
#endif

static bool ensure_ctx_bound_locked(uint8_t iface_index)
{
	struct net_context **ctx = NULL;
	struct net_if *iface = get_iface_by_index(iface_index);

	iface_index = normalize_iface_index(iface_index);
	if (iface_index == PROXY_WIFI_IFACE_AP) {
		ctx = &ap_ctx;
	} else {
		ctx = &sta_ctx;
	}

	if (!iface) {
		LOG_WRN("WiFi iface %u not available", iface_index);
		return false;
	}

	if (*ctx) {
		return true;
	}

	return init_ctx(ctx, iface);
}

#if (RCP_ETH_TRACE_SUPPORT && (RCP_ETH_TRACE_RX || RCP_ETH_TRACE_TX))
static inline uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void mac_to_str(char *out, size_t out_sz, const uint8_t *m)
{
	snprintf(out, out_sz, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void ip4_to_str(char *out, size_t out_sz, const uint8_t *ip)
{
	snprintf(out, out_sz, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

// Prints only ARP / DHCP (UDP 67/68) / ICMP to reduce log size
static void log_eth_brief(const char *tag, const uint8_t *buf, size_t len)
{
	if (!buf || len < 14) {
		return;
	}

	const uint8_t *dst = &buf[0];
	const uint8_t *src = &buf[6];
	uint16_t ethertype = be16(&buf[12]);
	size_t l2off = 14;

	// Optional VLAN (0x8100) support
	if (ethertype == 0x8100 && len >= 18) {
		ethertype = be16(&buf[16]);
		l2off = 18;
	}

	// ARP
	if (ethertype == 0x0806) {
		if (len < l2off + 28) {
			return;
		}
		uint16_t op = be16(&buf[l2off + 6]);
		const uint8_t *sha = &buf[l2off + 8];
		const uint8_t *spa = &buf[l2off + 14];
		const uint8_t *tha = &buf[l2off + 18];
		const uint8_t *tpa = &buf[l2off + 24];

		char s_src[24], s_dst[24], s_sha[24], s_tha[24], s_spa[16], s_tpa[16];
		mac_to_str(s_src, sizeof s_src, src);
		mac_to_str(s_dst, sizeof s_dst, dst);
		mac_to_str(s_sha, sizeof s_sha, sha);
		mac_to_str(s_tha, sizeof s_tha, tha);
		ip4_to_str(s_spa, sizeof s_spa, spa);
		ip4_to_str(s_tpa, sizeof s_tpa, tpa);

		LOG_INF("[ETH] %s ARP op=%u eth %s->%s arp %s/%s -> %s/%s len=%u\n", tag, op, s_src,
			s_dst, s_sha, s_spa, s_tha, s_tpa, (unsigned)len);
		return;
	}

	// IPv4
	if (ethertype == 0x0800) {
		if (len < l2off + 20) {
			return;
		}
		uint8_t ihl = (buf[l2off] & 0x0F) * 4;
		if (ihl < 20 || len < l2off + ihl) {
			return;
		}

		uint8_t proto = buf[l2off + 9];
		const uint8_t *sip = &buf[l2off + 12];
		const uint8_t *dip = &buf[l2off + 16];

		// UDP header starts at l2off+ihl
		if (proto == 17 && len >= l2off + ihl + 8) {
			uint16_t sport = be16(&buf[l2off + ihl + 0]);
			uint16_t dport = be16(&buf[l2off + ihl + 2]);

			// DHCP traffic (67/68) is of interest
			if (!((sport == 67 || sport == 68) || (dport == 67 || dport == 68))) {
				return;
			}

			char s_src[24], s_dst[24], s_sip[16], s_dip[16];
			mac_to_str(s_src, sizeof s_src, src);
			mac_to_str(s_dst, sizeof s_dst, dst);
			ip4_to_str(s_sip, sizeof s_sip, sip);
			ip4_to_str(s_dip, sizeof s_dip, dip);

			LOG_INF("[ETH] %s UDP %s:%u -> %s:%u eth %s->%s len=%u\n", tag, s_sip,
				sport, s_dip, dport, s_src, s_dst, (unsigned)len);
			return;
		}

		// ICMP traffic (ping) is of interest
		if (proto == 1 && len >= l2off + ihl + 4) {
			uint8_t icmp_type = buf[l2off + ihl + 0];
			uint8_t icmp_code = buf[l2off + ihl + 1];

			char s_src[24], s_dst[24], s_sip[16], s_dip[16];
			mac_to_str(s_src, sizeof s_src, src);
			mac_to_str(s_dst, sizeof s_dst, dst);
			ip4_to_str(s_sip, sizeof s_sip, sip);
			ip4_to_str(s_dip, sizeof s_dip, dip);

			LOG_INF("[ETH] %s ICMP type=%u code=%u %s->%s eth %s->%s len=%u\n", tag,
				icmp_type, icmp_code, s_sip, s_dip, s_src, s_dst, (unsigned)len);
			return;
		}
	}
}
#endif /* RCP_ETH_TRACE_RX || RCP_ETH_TRACE_TX */

#if (RCP_ETH_TRACE_SUPPORT && RCP_ETH_TRACE_RX)
#define RCP_ETH_LOG_RX(tag, buf, len) log_eth_brief((tag), (buf), (len))
#else
#define RCP_ETH_LOG_RX(tag, buf, len)                                                              \
	do {                                                                                       \
	} while (0)
#endif

#if (RCP_ETH_TRACE_SUPPORT && RCP_ETH_TRACE_TX)
#define RCP_ETH_LOG_TX(tag, buf, len) log_eth_brief((tag), (buf), (len))
#else
#define RCP_ETH_LOG_TX(tag, buf, len)                                                              \
	do {                                                                                       \
	} while (0)
#endif

bool proxy_wifi_data_init(void)
{
	k_thread_stack_t *wifi_rx_stack;

	register_data_msg_handler(RCP_STREAM_WIFI_DATA, handle_data_msg);

#if (RCP_RX_MODE == RCP_RX_MODE_DIRECT)
	/* DIRECT: the wifi driver rx cb calls proxy_wifi_data_rx_direct() directly
	 * (extern); no AF_PACKET net_context, no rx_fifo, no wifi_rx_thread. */
	return true;
#else
	k_mutex_lock(&ctx_lock, K_FOREVER);
	(void)ensure_ctx_bound_locked(PROXY_WIFI_IFACE_STA);
	(void)ensure_ctx_bound_locked(PROXY_WIFI_IFACE_AP);
	k_mutex_unlock(&ctx_lock);

	if (wifi_rx_tid != NULL) {
		return true;
	}

	wifi_rx_stack = k_thread_stack_alloc(RX_THREAD_STACK_SIZE, 0);
	if (wifi_rx_stack == NULL) {
		LOG_ERR("Failed to allocate WiFi RX thread stack");
		return false;
	}

	wifi_rx_tid = k_thread_create(&wifi_rx_thread_data, wifi_rx_stack,
				      RX_THREAD_STACK_SIZE, wifi_rx_thread,
				      NULL, NULL, NULL, RX_THREAD_PRIO, 0, K_NO_WAIT);
	if (wifi_rx_tid == NULL) {
		LOG_ERR("Failed to create WiFi RX thread");
		return false;
	}

	k_thread_name_set(wifi_rx_tid, "wifi_rx");

	return true;
#endif /* RCP_RX_MODE */
}

static bool init_ctx(struct net_context **ctx, struct net_if *iface)
{
	int ret = 0;
	struct sockaddr_ll addr = {0};
	struct net_context *local = NULL;

	if (!iface) {
		LOG_ERR("Wi-Fi interface not found");
		return false;
	}

	ret = net_context_get(AF_PACKET, SOCK_RAW, ETH_P_ALL, &local);
	if (ret < 0) {
		LOG_ERR("Failed to get net_context: %d", ret);
		return false;
	}

	addr.sll_ifindex = net_if_get_by_iface(iface);
	addr.sll_family = AF_PACKET;
	addr.sll_protocol = htons(ETH_P_ALL);

	ret = net_context_bind(local, (const struct sockaddr *)&addr, sizeof(struct sockaddr_ll));
	if (ret < 0) {
		LOG_ERR("Failed to bind net_context: %d", ret);
		(void)net_context_put(local);
		return false;
	}

	ret = net_context_recv(local, wifi_rx_cb, K_NO_WAIT, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to register RX callback: %d", ret);
		(void)net_context_put(local);
		return false;
	}

	*ctx = local;
	LOG_INF("WiFi data net_context bound ifindex=%d", addr.sll_ifindex);
	return true;
}

#if (RCP_TX_MODE == RCP_TX_MODE_DIRECT)
/* Experiment B: hand the frame straight to the WiFi driver's .send, bypassing
 * the socket / net_context / ethernet-L2 dispatch path. The driver .send is the
 * first member (ethernet_api at offset 0) of the device API.
 *
 * qwifi_drv_send() copies the frame into its own DPM TX buffer and does NOT
 * take ownership of the net_pkt (in the normal path the ethernet L2 unrefs it
 * after .send returns). Since we bypass L2, WE must unref the pkt ourselves —
 * on every path, success or failure — or the 16-entry TX pool leaks dry after
 * a few frames (net_pkt_alloc then returns -ENOMEM). */
static int wifi_tx_direct(struct net_if *iface, const uint8_t *frame, uint16_t len)
{
	const struct device *dev = net_if_get_device(iface);
	const struct ethernet_api *eth = dev ? dev->api : NULL;
	struct net_pkt *pkt;
	int ret;

	if (!eth || !eth->send) {
		return -ENOTSUP;
	}

	pkt = net_pkt_alloc_with_buffer(iface, len, AF_UNSPEC, 0, K_NO_WAIT);
	if (!pkt) {
		return -ENOMEM;
	}

	if (net_pkt_write(pkt, frame, len) < 0) {
		net_pkt_unref(pkt);
		return -EIO;
	}

	ret = eth->send(dev, pkt);
	net_pkt_unref(pkt);
	return ret;
}
#endif

/* Called from the RCP dispatcher context. */
static uint8_t handle_data_msg(enum rcp_rx_status status, rcp_buffer_hdl_t *payload_ptr,
			       rcp_error_info_t *)
{
	ARG_UNUSED(status);

	int ret = 0;
	uint8_t retries = WIFI_TX_RETRIES;
	uint8_t iface_index = PROXY_WIFI_IFACE_STA;
	const uint8_t *frame_ptr;
	uint16_t frame_len;
	int ifindex;
	struct net_context *ctx = NULL;

	if (!payload_ptr) {
		return RCP_RX_NO_ACTION;
	}

	frame_ptr = payload_ptr->data_ptr;
	frame_len = payload_ptr->size;

	if ((payload_ptr->size >= PROXY_WIFI_DATA_HDR_SIZE) &&
	    (payload_ptr->data_ptr[0] == PROXY_WIFI_DATA_HDR_VERSION)) {
		iface_index = normalize_iface_index(payload_ptr->data_ptr[1]);
		frame_ptr = &payload_ptr->data_ptr[PROXY_WIFI_DATA_HDR_SIZE];
		frame_len = payload_ptr->size - PROXY_WIFI_DATA_HDR_SIZE;
	}

	if ((iface_index != PROXY_WIFI_IFACE_STA) && (iface_index != PROXY_WIFI_IFACE_AP)) {
		LOG_WRN("TX: unsupported iface_index=%u", iface_index);
		return RCP_RX_NO_ACTION;
	}

	if (frame_len == 0U) {
		LOG_WRN("TX: empty Ethernet frame");
		return RCP_RX_NO_ACTION;
	}

	RCP_ETH_LOG_TX("MCU TX(host->wifi)", frame_ptr, frame_len);

	k_mutex_lock(&tx_lock, K_FOREVER);

#if (RCP_TX_MODE == RCP_TX_MODE_DIRECT)
	/* DIRECT: skip socket/net_context/L2 — call the driver .send. */
	ARG_UNUSED(retries);
	ARG_UNUSED(ctx);
	ARG_UNUSED(ifindex);
	{
		struct net_if *iface = get_iface_by_index(iface_index);

		if (!iface) {
			LOG_ERR("TX: iface %u not available", iface_index);
			k_mutex_unlock(&tx_lock);
			return RCP_RX_NO_ACTION;
		}
		ret = wifi_tx_direct(iface, frame_ptr, frame_len);
		if (ret < 0) {
			/* -EAGAIN here is the WiFi HW TX queue being transiently
			 * full (common when TCP ACKs stream back during high-rate
			 * RX). Harmless — TCP's cumulative ACK recovers — so log at
			 * debug level rather than error to avoid noise. */
			LOG_DBG("wifi_tx_direct failed: %d", ret);
		}
	}
#else /* RCP_TX_MODE_BLOCKING */
	k_mutex_lock(&ctx_lock, K_FOREVER);
	(void)ensure_ctx_bound_locked(iface_index);
	ctx = get_ctx_locked(iface_index);
	k_mutex_unlock(&ctx_lock);

	if (!ctx) {
		LOG_ERR("TX: net_context for iface %u not ready", iface_index);
		k_mutex_unlock(&tx_lock);
		return RCP_RX_NO_ACTION;
	}

	ifindex = get_ifindex_by_iface_index(iface_index);
	if (ifindex < 0) {
		LOG_ERR("TX: invalid ifindex for iface %u", iface_index);
		k_mutex_unlock(&tx_lock);
		return RCP_RX_NO_ACTION;
	}

	LOG_DBG("WiFi TX over ifindex=%d iface=%u len=%u", ifindex, iface_index, frame_len);

	struct sockaddr_ll dst = {
		.sll_ifindex = ifindex,
		.sll_family = AF_PACKET,
		.sll_protocol = htons(ETH_P_ALL),
	};

	do {
		k_sem_reset(&tx_sem);
		tx_status = 0;

		ret = net_context_sendto(ctx, frame_ptr, frame_len, (const struct sockaddr *)&dst,
					 sizeof(struct sockaddr_ll), wifi_tx_cb, K_NO_WAIT, NULL);
		if (ret < 0) {
			LOG_ERR("net_context_sendto failed: %d", ret);
			k_sleep(K_MSEC(RCP_TX_ERR_DELAY_MS));
			continue;
		}

		ret = k_sem_take(&tx_sem, K_MSEC(WIFI_TX_CB_TIMEOUT_MS));
		if (ret != 0) {
			LOG_ERR("TX callback timeout");
			/* Wait for completion to avoid overlap with next send. */
			(void)k_sem_take(&tx_sem, K_FOREVER);
		}

		if (tx_status >= 0) {
			break;
		}

		/* Delay a bit and retry TX to the driver. */
		LOG_DBG("Wi-Fi TX failed: %d", tx_status);
		k_sleep(K_MSEC(RCP_TX_ERR_DELAY_MS));
	} while (retries--);
#endif /* RCP_TX_MODE_DIRECT */

	k_mutex_unlock(&tx_lock);

	return RCP_RX_NO_ACTION;
}

static bool link_send_with_retries(rcp_buffer_hdl_t *msg_buffer)
{
	uint8_t retries = RCP_TX_RETRIES;
	link_result_t result = RCP_LNK_ERROR;

	do {
		result = link_send(RCP_STREAM_WIFI_DATA, msg_buffer);
		if (result != RCP_LNK_SUCCESS) {
			LOG_DBG("Failed to send WiFi data over link, retrying...");
			k_sleep(K_MSEC(RCP_TX_ERR_DELAY_MS));
		}
	} while (retries-- && (result != RCP_LNK_SUCCESS));

	return (result == RCP_LNK_SUCCESS);
}

#if (RCP_RX_MODE == RCP_RX_MODE_DIRECT)
/* RX DIRECT: called by the WiFi driver's rx callback (nt_wlan thread context)
 * with the raw Ethernet frame. Build the RCP WiFi-data frame in place and hand
 * it to link_send. The HAL frees `frame` right after we return, so we must copy
 * it out here (single copy, replacing net_pkt alloc + net_pkt_write +
 * net_pkt_read of the net-stack path). */
int proxy_wifi_data_rx_direct(uint8_t iface_idx, const uint8_t *frame, uint16_t len)
{
	rcp_buffer_hdl_t *msg_buffer;

	if ((len == 0U) || ((size_t)len + PROXY_WIFI_DATA_HDR_SIZE > RX_BUF_SIZE)) {
		return -1;
	}

	msg_buffer = get_buffer(RX_BUF_SIZE);
	if (!msg_buffer) {
		return -1;
	}

	msg_buffer->data_ptr[0] = PROXY_WIFI_DATA_HDR_VERSION;
	msg_buffer->data_ptr[1] = normalize_iface_index(iface_idx);
	memcpy(msg_buffer->data_ptr + PROXY_WIFI_DATA_HDR_SIZE, frame, len);
	msg_buffer->size = (uint16_t)(len + PROXY_WIFI_DATA_HDR_SIZE);

	RCP_ETH_LOG_RX("MCU RX(wifi->host)", msg_buffer->data_ptr + PROXY_WIFI_DATA_HDR_SIZE, len);

	if (!link_send_with_retries(msg_buffer)) {
		release_buffer(&msg_buffer);
		return -1;
	}
	return 0;
}
#endif /* RCP_RX_MODE_DIRECT */

/* Dedicated thread for WiFi -> host path. */
__attribute__((unused))
static void wifi_rx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("RX thread started");

	while (true) {
		struct net_pkt *pkt = NULL;
		rcp_buffer_hdl_t *msg_buffer = NULL;
		size_t received = 0;
		struct net_if *iface = NULL;
		uint8_t iface_index = PROXY_WIFI_IFACE_STA;

		pkt = k_fifo_get(&rx_fifo, K_FOREVER);
		if (!pkt) {
			continue;
		}

		received = net_pkt_get_len(pkt);
		msg_buffer = get_buffer(RX_BUF_SIZE);
		if (!msg_buffer) {
			k_sleep(K_MSEC(RCP_TX_ERR_DELAY_MS));
			LOG_WRN("No buffer available for RX");
			net_pkt_unref(pkt);
			continue;
		}
		if ((received + PROXY_WIFI_DATA_HDR_SIZE) > msg_buffer->max_size) {
			LOG_ERR("RX packet too large: %zu", received);
			net_pkt_unref(pkt);
			release_buffer(&msg_buffer);
			continue;
		}

		iface = net_pkt_iface(pkt);
		iface_index = normalize_iface_index(get_iface_index_by_iface(iface));

		net_pkt_cursor_init(pkt);
		if (net_pkt_read(pkt, msg_buffer->data_ptr + PROXY_WIFI_DATA_HDR_SIZE, received) <
		    0) {
			LOG_ERR("Failed to read RX packet");
			net_pkt_unref(pkt);
			release_buffer(&msg_buffer);
			continue;
		}
		net_pkt_unref(pkt);

		msg_buffer->data_ptr[0] = PROXY_WIFI_DATA_HDR_VERSION;
		msg_buffer->data_ptr[1] = iface_index;
		msg_buffer->size = (uint16_t)(received + PROXY_WIFI_DATA_HDR_SIZE);

		RCP_ETH_LOG_RX("MCU RX(wifi->host)",
			       msg_buffer->data_ptr + PROXY_WIFI_DATA_HDR_SIZE, received);

		/* If the link is congested, link_send_with_retries will stall here,
		 * preventing the next recv() call and naturally backing up the WiFi
		 * driver's socket buffer - triggering WiFi-level flow control.
		 */
		if (!link_send_with_retries(msg_buffer)) {
			LOG_ERR("Failed to send WiFi data over link, dropping");
			release_buffer(&msg_buffer);
		}
	}
}

static void wifi_rx_cb(struct net_context *ctx, struct net_pkt *pkt, union net_ip_header *ip_hdr,
		       union net_proto_header *proto_hdr, int status, void *user_data)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(ip_hdr);
	ARG_UNUSED(proto_hdr);
	ARG_UNUSED(status);
	ARG_UNUSED(user_data);

	if (!pkt) {
		return;
	}

	k_fifo_put(&rx_fifo, pkt);
}

#if (RCP_TX_MODE == RCP_TX_MODE_BLOCKING)
static void wifi_tx_cb(struct net_context *ctx, int status, void *user_data)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(user_data);

	tx_status = status;
	k_sem_give(&tx_sem);
}
#endif
