/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/bluetooth/hci_raw.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/buf.h>

#include <rcp_interface.h>
#include <rcp_interface_impl.h>

#include "proxy_bt.h"

#define BT_RX_THREAD_STACK_SIZE 1024
#define LINK_MAX_MSG_SIZE 500

LOG_MODULE_REGISTER(proxy_bt, CONFIG_QUALCOMM_RCP_APP_LOG_LEVEL);

static K_FIFO_DEFINE(rx_queue);

static K_THREAD_STACK_DEFINE(bt_rx_thread_stack, BT_RX_THREAD_STACK_SIZE);
static struct k_thread bt_rx_thread_data;
static k_tid_t rx_tid;

/* BT -> Host */
static bool link_send_bt_packet(struct net_buf *buf);
static void bt_rx_thread(void *p1, void *p2, void *p3);
/* Host -> BT */
static uint8_t handle_rcp_msg(enum rcp_rx_status status, rcp_buffer_hdl_t *payload_ptr, rcp_error_info_t *err_info_ptr);

bool proxy_bt_init(void)
{
	int ret;

	register_data_msg_handler(RCP_STREAM_BT, handle_rcp_msg);

	ret = bt_enable_raw(&rx_queue);
	if (ret) {
		LOG_ERR("Failed to enable BT raw mode: %d", ret);
		return false;
	}

	rx_tid = k_thread_create(&bt_rx_thread_data, bt_rx_thread_stack,
		K_THREAD_STACK_SIZEOF(bt_rx_thread_stack),
		bt_rx_thread, NULL, NULL, NULL, K_PRIO_COOP(8),
		0, K_NO_WAIT);

	k_thread_name_set(&bt_rx_thread_data, "bt_proxy_rx");

	return true;
}

static bool link_send_bt_packet(struct net_buf *buf)
{
    	rcp_buffer_hdl_t* msg_buffer;

	switch (bt_buf_get_type(buf)) {
	case BT_BUF_ACL_IN:
		net_buf_push_u8(buf, BT_HCI_H4_ACL);
		break;
	case BT_BUF_EVT:
		net_buf_push_u8(buf, BT_HCI_H4_EVT);
		break;
	default:
		LOG_ERR("Unknown packet from BT ctrl: %u", bt_buf_get_type(buf));
		net_buf_unref(buf);
		return false;
	}

	msg_buffer = get_buffer(LINK_MAX_MSG_SIZE);
	if (msg_buffer == NULL) {
		LOG_WRN("Failed to get buffer for upload");
		return false;
	}
	/* TODO: RCP should allow to prepare msg_buffer without allocation. */
	write_to_buffer(msg_buffer, buf->data, buf->len);

	if (RCP_LNK_SUCCESS != link_send(RCP_STREAM_BT, msg_buffer)) {
		LOG_ERR("Failed to send BT HCI packet over link");
		/* link_send() did not take ownership on failure */
		release_buffer(&msg_buffer);
		net_buf_unref(buf);
		return false;
	}

	net_buf_unref(buf);
	return true;
}

static uint8_t handle_rcp_msg(enum rcp_rx_status status, rcp_buffer_hdl_t *payload_ptr, rcp_error_info_t *err_info_ptr)
{
	struct net_buf *buf = NULL;
	int ret;
	uint8_t h4_type;

	union {
		struct bt_hci_cmd_hdr *cmd_hdr;
		struct bt_hci_acl_hdr *acl_hdr;
	} hci_hdr;

	/* Process message. */
	h4_type = payload_ptr->data_ptr[0];

	/* Get header. */
	hci_hdr.cmd_hdr = (struct bt_hci_cmd_hdr *)&payload_ptr->data_ptr[1];

	switch (h4_type) {
	case BT_HCI_H4_CMD:
		buf = bt_buf_get_tx(BT_BUF_CMD, K_NO_WAIT, hci_hdr.cmd_hdr,
				sizeof(*hci_hdr.cmd_hdr));
		if (buf) {
			net_buf_add_mem(buf, &payload_ptr->data_ptr[4],
					hci_hdr.cmd_hdr->param_len);
		} else {
			LOG_ERR("BT CMD buffers not available");
			return RCP_RX_ACK;
		}
		break;
	case BT_HCI_H4_ACL:
		buf = bt_buf_get_tx(BT_BUF_ACL_OUT, K_NO_WAIT, hci_hdr.acl_hdr,
				sizeof(*hci_hdr.acl_hdr));
		if (buf) {
			net_buf_add_mem(buf, &payload_ptr->data_ptr[5],
					hci_hdr.acl_hdr->len);
		} else {
			LOG_ERR("BT ACL buffers not available");
			return RCP_RX_ACK;
		}
		break;
	default:
		LOG_ERR("Unknown BT HCI packet: %d", h4_type);
		return RCP_RX_ACK;
	}
	ret = bt_send(buf);

	if (ret) {
		LOG_ERR("Unable to send BT packet: %d", ret);
		net_buf_unref(buf);
	}

	return RCP_RX_ACK;
}

static void bt_rx_thread(void *p1, void *p2, void *p3)
{
	struct net_buf *buf;
	while(true) {
		buf = k_fifo_get(&rx_queue, K_FOREVER);
		LOG_HEXDUMP_DBG(buf->data, buf->len, "BT CTRL data");
		link_send_bt_packet(buf);
		k_yield();
	}
}
