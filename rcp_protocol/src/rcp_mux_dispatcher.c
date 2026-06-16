/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "buffer_mgmt.h"
#include "rcp_error_codes.h"
#include "rcp_log.h"
#include "rcp_mux_dispatcher.h"
#include "rcp_cntrl_msgs.h"
#include "rcp_protocol.h"

#include <stddef.h>

static rcp_rx_event_handler_t data_dispatch_handlers[RCP_STREAM_DELIMITER];

static bool is_rx_seq_num_valid(lnk_sts_t *lnk_sts_ptr, rcp_msg_ptr_t data_msg)
{
	// Either peer RSN confirms receiving sent TSN or is within ack window
	if (data_msg->seq_nums.rsn != lnk_sts_ptr->seq_nums.tsn) {
		if (diff_seq_nums(lnk_sts_ptr->seq_nums.tsn, data_msg->seq_nums.rsn) >
		    RCP_SEQ_NUM_ACK_WINDOW) {
			RCP_LOG_ERR("Exceeded Ack window %d, Sent TSN=%d previous RSN=%d",
				    RCP_SEQ_NUM_ACK_WINDOW, lnk_sts_ptr->seq_nums.tsn,
				    data_msg->seq_nums.rsn);
			return false;
		}
	}
	return true;
}

void send_response_msg(rcp_msg_ptr_t data_msg, lnk_sts_t *lnk_sts_ptr, rcp_buffer_hdl_t *buffer)
{
	build_data_stream_msg(data_msg, lnk_sts_ptr, buffer);
	send_rcp_msg(data_msg);
}

static void send_response_ack(rcp_msg_ptr_t data_msg, lnk_sts_t *lnk_sts_ptr)
{
	// Ack message do not have payload
	release_buffer(&data_msg->payload_ptr);
	if (lnk_sts_ptr->state == LINK_STATE_ACTIVE) {
		if (lnk_sts_ptr->seq_nums.ack_window++ < RCP_SEQ_NUM_ACK_WINDOW) {
			// Defer sending Ack until window size reached
			return;
		}
		RCP_LOG_DBG("Sending ack on Window Size=%d", lnk_sts_ptr->seq_nums.ack_window);
		build_ack_to_rxmsg(data_msg, &lnk_sts_ptr->seq_nums);
		send_rcp_msg(data_msg);
	} else {
		RCP_LOG_INF("Sending ack as response");
		send_response_msg(data_msg, lnk_sts_ptr, NULL);
	}
}

static void send_response_nack(rcp_msg_ptr_t data_msg, lnk_sts_t *lnk_sts_ptr,
			       rcp_buffer_hdl_t *buffer)
{
	if (marshall_nak_error(&buffer, &lnk_sts_ptr->err_info)) {
		build_nack_to_rxmsg(data_msg, &lnk_sts_ptr->seq_nums, buffer);
		send_rcp_msg(data_msg);
	} else {
		RCP_LOG_WRN("Unable to marshall error code=%d", lnk_sts_ptr->err_info.err_code);
		release_buffer(&buffer);
	}
}

static void default_data_msg_handler(rcp_msg_ptr_t data_msg, lnk_sts_t *lnk_sts_ptr)
{
	if ((data_msg->stream >= RCP_STREAM_VSERIAL0) &&
	    (data_msg->stream <= RCP_STREAM_VSERIAL3)) {
		/*
		 * Optional VSerial streams are best-effort. If the host has not enabled
		 * a handler for them, drop silently without generating protocol traffic
		 * back to the peer. This prevents accidental log/test writes on a disabled
		 * VSerial port from starting an ACK/NACK/sync loop.
		 */
		RCP_LOG_DBG("No handler for VSerial stream=%d, dropping without response",
			    data_msg->stream);
		return;
	}

	RCP_LOG_ERR("Using default dispatcher for stream=%d", data_msg->stream);
	if (data_msg->msg_type != RCP_MSG_TYPE_NACK) {
		// Only respond to non Nack messages -stops Nack loop
		RCP_LOG_DBG("Sending Nack response on stream=%x\n", data_msg->stream);
		lnk_sts_ptr->err_info.err_code = RCP_ERR_UNHANDELED_STREAM;
		send_response_nack(data_msg, lnk_sts_ptr, data_msg->payload_ptr);
	} else {
		release_buffer(&data_msg->payload_ptr);
	}
}

static bool manage_data_state(rcp_msg_ptr_t data_msg, lnk_sts_t *lnk_sts_ptr)
{
	RCP_LOG_DBG("Recieved message type=%d for Stream=%d", data_msg->msg_type, data_msg->stream);

	lnk_sts_ptr->err_info.err_code = RCP_ERR_UNDEFINED;

	switch (data_msg->msg_type) {
	case RCP_MSG_TYPE_DATA_XFER:  /* Fall-through intended.*/
	case RCP_MSG_TYPE_DATA_XFER_ENC: {
		lnk_sts_ptr->rx_status = RCP_RX_STS_DATA_MSG;
		uint8_t valid_tsn_seq = inc_seq_num(lnk_sts_ptr->seq_nums.rsn);
		if (data_msg->seq_nums.tsn != valid_tsn_seq) {
			RCP_LOG_ERR("Seq NUM mismatch Msg TSN %d <> Expected %d",
				    data_msg->seq_nums.tsn, valid_tsn_seq);
			lnk_sts_ptr->rx_status = RCP_RX_STS_ERROR;

			set_rx_seq_err(data_msg->seq_nums.tsn, valid_tsn_seq,
				       &lnk_sts_ptr->err_info);

			lnk_sts_ptr->seq_nums.rsn = data_msg->seq_nums.tsn;
			RCP_LOG_WRN("Resetting RSN to %d", data_msg->seq_nums.tsn);
			lnk_sts_ptr->seq_nums.rsn = data_msg->seq_nums.tsn;
		} else if (!is_rx_seq_num_valid(lnk_sts_ptr, data_msg)) {
			RCP_LOG_ERR("Out of sync RSN=%d from peer", data_msg->seq_nums.rsn);
			set_ack_seq_err(data_msg->seq_nums.rsn, lnk_sts_ptr->seq_nums.tsn,
					&lnk_sts_ptr->err_info);
			lnk_sts_ptr->rx_status = RCP_RX_STS_ERROR;
		} else {
			// Update RSN
			lnk_sts_ptr->seq_nums.rsn = valid_tsn_seq;
		}
		break;
	}
	case RCP_MSG_TYPE_INIT_DATA: /* Fall-through intended.*/
	case RCP_MSG_TYPE_INIT_DATA_ENC: {
		lnk_sts_ptr->rx_status = RCP_RX_STS_INIT_MSG;
		lnk_sts_ptr->seq_nums.rsn = data_msg->seq_nums.tsn;

		if (lnk_sts_ptr->state == LINK_STATE_SYNCING) {
			lnk_sts_ptr->seq_nums.tsn = inc_seq_num(lnk_sts_ptr->seq_nums.tsn);
			set_rcp_link_active(lnk_sts_ptr);
		} else {
			RCP_LOG_INF("Stream=%d SYNCING", data_msg->stream);
			lnk_sts_ptr->rx_status = RCP_RX_STS_INIT_MSG;
			lnk_sts_ptr->state = LINK_STATE_SYNC_PENDING;
		}
		break;
	}
	case RCP_MSG_TYPE_ACK: {
		// Update acked TSN
		if (!is_rx_seq_num_valid(lnk_sts_ptr, data_msg)) {
			RCP_LOG_ERR("RSN Ack from peer indicates message sequence error");
			lnk_sts_ptr->rx_status = RCP_RX_STS_ERROR;
			return true;
		}
		return false;
		break;
	}
	case RCP_MSG_TYPE_NACK:
		lnk_sts_ptr->rx_status = RCP_RX_STS_NACK_MSG;
		if (!decode_nak_error_info(&lnk_sts_ptr->err_info, data_msg->payload_ptr)) {
			RCP_LOG_ERR("Failed to Decode Nack Message");
		}
		break;

	default:
		RCP_LOG_WRN("Rx Invalid Msg=%02x", data_msg->msg_type);
	}
	return true;
}

static void dispatch_data_msg(rcp_msg_ptr_t data_msg, lnk_sts_t *lnk_sts_ptr)
{
	if (!manage_data_state(data_msg, lnk_sts_ptr)) {
		return;
	}

	rcp_rx_event_handler_t dispatch_fun = data_dispatch_handlers[data_msg->stream];

	enum rcp_dsptch_result result = RCP_RX_NO_ACTION;
	if (dispatch_fun) {
		result = dispatch_fun(lnk_sts_ptr->rx_status, data_msg->payload_ptr,
				      &lnk_sts_ptr->err_info);

		switch (result) {
		case RCP_RX_RESPONSE:
			send_response_msg(data_msg, lnk_sts_ptr, data_msg->payload_ptr);
			break;
		case RCP_RX_ACK:
			send_response_ack(data_msg, lnk_sts_ptr);
			break;
		case RCP_RX_NACK:
			send_response_nack(data_msg, lnk_sts_ptr, data_msg->payload_ptr);
		default:
			RCP_LOG_DBG("Taking no action on result=%d", result);
			release_buffer(&data_msg->payload_ptr);
			break;
		}
	} else {
		default_data_msg_handler(data_msg, lnk_sts_ptr);
	}
	release_buffer(&data_msg->payload_ptr);
}

static void dispatch_cntrl_msg(rcp_msg_ptr_t cntrl_msg)
{
	RCP_LOG_DBG("Rx Cntrl Msgs Type %d  Subtype %d", cntrl_msg->msg_type,
		    cntrl_msg->msg_subtype);
	handle_cntrl_msgs(cntrl_msg);
	release_buffer(&cntrl_msg->payload_ptr);
}

void init_dispatcher()
{
	for (int i = 0; i < RCP_STREAM_DELIMITER; ++i) {
		data_dispatch_handlers[i] = NULL;
	}
}

bool register_data_msg_handler(const uint8_t stream, rcp_rx_event_handler_t handler)
{
	if ((stream != RCP_STREAM_LNK_MGMT) && stream < RCP_STREAM_DELIMITER) {
		data_dispatch_handlers[stream] = handler;
		return true;
	}
	RCP_LOG_ERR("Stream Id: %d Not supported", stream);
	return false;
}

void dispatch_msg(rcp_msg_ptr_t message, lnk_sts_t *link_sts_ptr)
{
	if (message->stream < RCP_STREAM_DELIMITER) {
		if (msg_type2fmt(message->msg_type) == RPC_MSG_FORMAT_DATA
			|| msg_type2fmt(message->msg_type) == RPC_MSG_FORMAT_DATA_ENC) {
			dispatch_data_msg(message, link_sts_ptr);

		} else if (msg_type2fmt(message->msg_type) == RPC_MSG_FORMAT_CONTROL) {
			if (message->stream != RCP_STREAM_LNK_MGMT) {
				RCP_LOG_WRN("Control Message wrong stream Expected %d Got %d",
					    RCP_STREAM_LNK_MGMT, message->stream);
				release_buffer(&message->payload_ptr);
			}
			dispatch_cntrl_msg(message);
		} else {
			RCP_LOG_ERR("Message Format: %d NOT supported", message->msg_type);
		}
	} else {
		RCP_LOG_ERR("Stream: %d Not supported", message->stream);
	}
}

rcp_rx_event_handler_t get_rx_handler(uint8_t stream)
{
	if ((stream == RCP_STREAM_LNK_MGMT) || (stream >= RCP_STREAM_DELIMITER)) {
		return NULL;
	}
	return data_dispatch_handlers[stream];
}
