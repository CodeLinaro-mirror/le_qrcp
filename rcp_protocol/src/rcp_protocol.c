/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */


#include "buffer_mgmt.h"
#include "rcp_definitions.h"
#include "rcp_log.h"
#include "rcp_protocol.h"
#include "rcp_tx_scheduler.h"
#include "rcp_encryption.h"
#include "rcp_config.h"

#include <stdio.h>
#include <stdlib.h>

/* Update data message buffers with upto date sequence numbers */
static void upd_msg_buff_seq_nums(lnk_sts_t *lnk_sts_ptr, rcp_buffer_hdl_t *buffer)
{
	if (buffer && (buffer->size >= RCP_PAYLOAD_LENGTH_SIZE)) {
		uint8_t *msg_ptr = buffer->data_ptr + RPC_POS_MSG_INFO;
		if (msg_type2fmt(*msg_ptr) == RPC_MSG_FORMAT_DATA ||
				msg_type2fmt(*msg_ptr) == RPC_MSG_FORMAT_DATA_ENC) {
			msg_ptr += RCP_POS_SEQ_NUMS;
			/* Clear seq fields first — retries reuse the same buffer, so
			 * OR-ing stale bits would corrupt TSN/RSN on the second call. */
			*msg_ptr &= ~(RCP_MASK_TSN | RCP_MASK_RSN | RCP_MASK_ACK);
			*msg_ptr |= (lnk_sts_ptr->seq_nums.tsn << RCP_BIT_SHFT_TSN) & RCP_MASK_TSN;
			*msg_ptr |= (lnk_sts_ptr->seq_nums.rsn << RCP_BIT_SHFT_RSN) & RCP_MASK_RSN;
			*msg_ptr |= (lnk_sts_ptr->seq_nums.ack_required << RCP_BIT_SHFT_ACK) &
				    RCP_MASK_ACK;
		}
	}
}

uint8_t msg_type2fmt(const uint8_t msg_type)
{
	return (msg_type & RCP_MASK_FORMAT) >> RCP_BIT_SHFT_FORMAT;
}

bool is_msg_encrypted(rcp_msg_ptr_t msg_ptr)
{
	return (msg_type2fmt(msg_ptr->msg_type) == RPC_MSG_FORMAT_DATA_ENC);
}

uint16_t marshal_rcp_msg(rcp_msg_ptr_t msg_ptr, rcp_buffer_hdl_t *tx_buffer)
{
	uint8_t *dest_ptr = tx_buffer->data_ptr;

	*dest_ptr++ = ((msg_ptr->stream << RCP_BIT_SHFT_STREAM) & RCP_MASK_STREAM) |
		      (msg_ptr->msg_type & RCP_MASK_MSG_TYPE);

	if (msg_type2fmt(msg_ptr->msg_type) == RPC_MSG_FORMAT_DATA ||
		msg_type2fmt(msg_ptr->msg_type) == RPC_MSG_FORMAT_DATA_ENC) {
		// Data message sequence number will be updated on bus send
		*dest_ptr++ = 0;
	} else {
		// Control Msg subtype
		*dest_ptr++ = msg_ptr->msg_subtype;
	}

	uint16_t payload_size = 0;

	if (is_msg_encrypted(msg_ptr) && msg_ptr->payload_ptr) {
#if RCP_ENCRYPTION
		uint8_t *payload = msg_ptr->payload_ptr->data_ptr;
		uint16_t size = (uint16_t)msg_ptr->payload_ptr->size;
		/* Start by putting the encrypted payload after the header.  */
		uint16_t out_len = rcp_encrypt_buf(
			payload, size, &tx_buffer->data_ptr[RCP_HDR_SIZE], tx_buffer->max_size
		);
		if (out_len == 0) {
			RCP_LOG_ERR("Failed to encrypt RCP message");
		}
		payload_size = out_len;
#else
		RCP_LOG_ERR("Encryption not enabled to support encrypted messages");
#endif
		dest_ptr = put_payld_size_in_buff(dest_ptr, payload_size);
	} else {
		if(msg_ptr->payload_ptr != NULL) {
			payload_size = msg_ptr->payload_ptr->size;
		}
		dest_ptr = put_payld_size_in_buff(dest_ptr, payload_size);

		if ((msg_ptr->payload_ptr != NULL) || (payload_size == 0)) {
			read_from_buffer(dest_ptr, msg_ptr->payload_ptr, payload_size);
		}
	}

	return payload_size + RCP_HDR_SIZE;
}

bool decode_rcp_msg(rcp_msg_ptr_t msg_ptr, rcp_buffer_hdl_t *rx_buffer)
{
	if (rx_buffer->data_ptr == NULL) {
		RCP_LOG_ERR("Recieve buffer is NULL");
		return false;
	}


	uint8_t *scan_ptr = rx_buffer->data_ptr;
	msg_ptr->stream = (*scan_ptr & RCP_MASK_STREAM) >> RCP_BIT_SHFT_STREAM;
	msg_ptr->msg_type = (*scan_ptr++ & RCP_MASK_MSG_TYPE);
	if (msg_type2fmt(msg_ptr->msg_type) == RPC_MSG_FORMAT_DATA ||
			msg_type2fmt(msg_ptr->msg_type) == RPC_MSG_FORMAT_DATA_ENC) {
		msg_ptr->seq_nums.tsn = (*scan_ptr & RCP_MASK_TSN) >> RCP_BIT_SHFT_TSN;
		msg_ptr->seq_nums.rsn = (*scan_ptr & RCP_MASK_RSN) >> RCP_BIT_SHFT_RSN;
		msg_ptr->seq_nums.ack_required = (*scan_ptr++ & RCP_MASK_ACK) >> RCP_BIT_SHFT_ACK;
	} else {
		msg_ptr->msg_subtype = *scan_ptr++;
	}

	// Get payload length
	uint16_t payload_size = get_payld_size_from_buff(scan_ptr);

	// Sanity check message length should be msg header + payload
	if (rx_buffer->size < RCP_HDR_SIZE + payload_size) {
		RCP_LOG_ERR("Invalid RX message: msg type=%d msg size %d < %d", msg_ptr->msg_type,
			    rx_buffer->size, RCP_HDR_SIZE + payload_size);
		return false;
	}

	msg_ptr->payload_ptr = NULL;
	if (payload_size > 0) {
		scan_ptr += 2;

		if (is_msg_encrypted(msg_ptr)) {
			/* Encrypted: decrypt into a fresh buffer (cannot decrypt in
			 * place), then release the raw rx_buffer. */
			msg_ptr->payload_ptr = get_buffer(payload_size);
			if (msg_ptr->payload_ptr) {
				RCP_LOG_DBG("Received encrypted message");
#if RCP_ENCRYPTION
				uint8_t *buf = msg_ptr->payload_ptr->data_ptr;
				uint16_t buf_len = msg_ptr->payload_ptr->max_size;
				uint16_t decrypted = rcp_decrypt_buf(scan_ptr, payload_size, buf, buf_len);
				msg_ptr->payload_ptr->size = decrypted;
#else
				RCP_LOG_ERR("Encryption not supported to decrypt message");
#endif
			}
			release_buffer(&rx_buffer);
		} else {
			/* Zero-copy: hand the rx_buffer itself up as the payload,
			 * advancing data_ptr past the RCP header instead of allocating a
			 * second buffer and memcpy'ing ~1.5 KB (the dominant per-frame
			 * cost on this CPU). release_internal_buffer() restores data_ptr
			 * to the backing store when the payload buffer is freed. */
			rx_buffer->data_ptr = scan_ptr;
			rx_buffer->size = payload_size;
			/* Shrink max_size so data_ptr + max_size stays within the
			 * backing store (data_ptr advanced by RCP_HDR_SIZE). Restored
			 * on release. */
			rx_buffer->max_size = payload_size;
			msg_ptr->payload_ptr = rx_buffer;
		}
	} else {
		/* No payload: nothing to hand up, free the raw buffer. */
		release_buffer(&rx_buffer);
	}

	return true;
}

uint16_t get_payld_size_from_buff(uint8_t *buffer_ptr)
{
	uint16_t length = 0;
	length = *buffer_ptr++;
	length += *buffer_ptr++ << 8;
	return length;
}

uint8_t *put_payld_size_in_buff(uint8_t *buffer_ptr, uint16_t payload_length)
{
	*buffer_ptr++ = (uint8_t)(payload_length & 0x00FF);
	*buffer_ptr++ = (uint8_t)((payload_length & 0xFF00) >> 8);
	return buffer_ptr;
}

void build_rcp_msg(rcp_msg_ptr_t msg_ptr, uint8_t stream, bool encrypted, rcp_seq_num_t *seq_nums,
		   rcp_buffer_hdl_t *payload_ptr)
{
	msg_ptr->stream = stream;
	msg_ptr->msg_type = encrypted ? RCP_MSG_TYPE_DATA_XFER_ENC : RCP_MSG_TYPE_DATA_XFER;
	msg_ptr->inc_tsn_after_send = true;
	msg_ptr->seq_nums = *seq_nums;
	msg_ptr->payload_ptr = payload_ptr;
}

void build_link_sync_message(rcp_msg_ptr_t msg_ptr, uint8_t stream, bool encrypted, bool inc_tsn_after_send,
			     rcp_seq_num_t *seq_nums, rcp_buffer_hdl_t *payload_ptr)
{
	msg_ptr->stream = stream;
	msg_ptr->msg_type = encrypted ? RCP_MSG_TYPE_INIT_DATA_ENC : RCP_MSG_TYPE_INIT_DATA;
	msg_ptr->inc_tsn_after_send = inc_tsn_after_send;
	msg_ptr->seq_nums = *seq_nums;
	msg_ptr->payload_ptr = payload_ptr;
}

void build_ack_to_rxmsg(rcp_msg_ptr_t msg_ptr, rcp_seq_num_t *seq_nums)
{
	msg_ptr->msg_type = RCP_MSG_TYPE_ACK;
	msg_ptr->seq_nums.ack_required = 0;
	msg_ptr->inc_tsn_after_send = false;
	// Ack should have rx message
	msg_ptr->seq_nums = *seq_nums;
	msg_ptr->payload_ptr = NULL;
}

void build_nack_to_rxmsg(rcp_msg_ptr_t msg_ptr, rcp_seq_num_t *seq_nums,
			 rcp_buffer_hdl_t *payload_ptr)
{
	msg_ptr->msg_type = RCP_MSG_TYPE_NACK;
	msg_ptr->inc_tsn_after_send = false;
	msg_ptr->seq_nums.ack_required = 0;
	// Ack should have rx message
	msg_ptr->seq_nums = *seq_nums;
	msg_ptr->payload_ptr = payload_ptr;
}

void build_cntrl_rcp_msg(rcp_msg_ptr_t msg_ptr, uint8_t msg_type, uint8_t msg_subtype,
			 rcp_buffer_hdl_t *payload_ptr)
{
	// Control messages go over link management stream
	msg_ptr->stream = RCP_STREAM_LNK_MGMT;
	msg_ptr->msg_type = msg_type;
	msg_ptr->msg_subtype = msg_subtype;
	msg_ptr->payload_ptr = payload_ptr;
}

void build_data_stream_msg(rcp_msg_ptr_t send_msg, lnk_sts_t *lnk_sts_ptr, rcp_buffer_hdl_t *buffer)
{
	switch (lnk_sts_ptr->state) {
	case LINK_STATE_ACTIVE: {
		// No action required
		build_rcp_msg(send_msg, lnk_sts_ptr->stream, lnk_sts_ptr->encrypted,
			      &lnk_sts_ptr->seq_nums, buffer);
		break;
	}
	case LINK_STATE_SYNC_PENDING:
		set_rcp_link_active(lnk_sts_ptr);
		build_link_sync_message(send_msg, lnk_sts_ptr->stream, lnk_sts_ptr->encrypted,
			true, &lnk_sts_ptr->seq_nums, buffer);
		break;

	default:
		RCP_LOG_INF("Sending Sync Message");
		build_link_sync_message(send_msg, lnk_sts_ptr->stream, lnk_sts_ptr->encrypted,
			false, &lnk_sts_ptr->seq_nums, buffer);
		lnk_sts_ptr->state = LINK_STATE_SYNCING;
	}
}

bool send_rcp_msg(rcp_msg_ptr_t send_msg)
{
	rcp_buffer_hdl_t *tx_buffer = get_buffer(RCP_BUF_SIZE);
	if (tx_buffer == NULL) {
		return false;
	}

	bool is_ack = false;
	if ((send_msg->msg_type == RCP_MSG_TYPE_DATA_XFER) ||
	    (send_msg->msg_type == RCP_MSG_TYPE_DATA_XFER_ENC) ||
	    (send_msg->msg_type == RCP_MSG_TYPE_ACK)) {
		is_ack = true;
	}

	tx_buffer->size = marshal_rcp_msg(send_msg, tx_buffer);
	RCP_LOG_DBG("Sending Msg=%d", send_msg->msg_type);

	if (!schedule_tx(tx_buffer, send_msg->stream, send_msg->inc_tsn_after_send, is_ack)) {
		/* Only drop the marshalled version. Let user decide what to do
		 * with provided message in case of failure. */
		release_buffer(&tx_buffer);
		return false;
	}
	release_buffer(&send_msg->payload_ptr);
	return true;
}

bool tx_rcp_msg(lnk_sts_t *lnk_sts_ptr, rcp_buffer_hdl_t *tx_buffer)
{
	// Update Seq nums before send
	upd_msg_buff_seq_nums(lnk_sts_ptr, tx_buffer);

	if (RCP_BUS_OK != rcp_bus_write(lnk_sts_ptr->bus, tx_buffer->data_ptr, tx_buffer->size)) {
		return false;
	}
	return true;
}

uint8_t inc_seq_num(uint8_t current_value)
{
	return (current_value < RCP_SEQ_NUM_MAX ? current_value + 1 : 0) & 0x07;
}

uint8_t diff_seq_nums(uint8_t tsn_sent, uint8_t last_rsn)
{
	// RSN ahead of TSN
	uint8_t diff;
	if (tsn_sent >= last_rsn) {
		diff = tsn_sent - last_rsn;
	} else {
		// RSN behind TSN
		diff = (RCP_SEQ_NUM_MAX - last_rsn) + tsn_sent + 1;
	}
	return diff;
}

void set_rcp_link_active(lnk_sts_t *lnk_sts_ptr)
{
	lnk_sts_ptr->state = LINK_STATE_ACTIVE;
	lnk_sts_ptr->seq_nums.ack_window = 0;
	// Peer expects incremented TSN
	RCP_LOG_INF("stream %d ACTIVE tsn=%d rsn=%d Ack Wdw=%d", lnk_sts_ptr->stream,
		    lnk_sts_ptr->seq_nums.tsn, lnk_sts_ptr->seq_nums.rsn, RCP_SEQ_NUM_ACK_WINDOW);
}
