/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file rcp_protocol.h
 *
 * @defgroup rcp_protocol RCP messaging protocol core
 *
 * RCP protocol core provides functions to format and decode RCP messages.
 *
 * @{
 * @ingroup rcp_internal
 */
#ifndef __RCP_PROTOCOL_H__
#define __RCP_PROTOCOL_H__

#include "buffer_mgmt.h"
#include "rcp_definitions.h"
#include "rcp_bus.h"
#include "rcp_error_codes.h"

#include <stdbool.h>

typedef enum {
	RCP_LNK_SUCCESS = 0,
	RCP_LNK_NO_RX_DATA,
	RCP_LNK_TIMEOUT,
	RCP_LNK_ERR_TX_NO_RESOURCES,
	RCP_LNK_ERR_UNSUPPORTED_STREAM,
	RCP_LNK_ERR_INVALID_STREAM,
	RCP_LINK_ERR_INVALID_BUFFER,
	RCP_LINK_SYNCING,
	RCP_LNK_ERROR,
} link_result_t;

typedef enum {
	LINK_STATE_IDLE = 0,     /**< Link not synced */
	LINK_STATE_SYNCING,      /**< Waiting for SYNC response */
	LINK_STATE_SYNC_PENDING, /**< Got sync need to send */
	LINK_STATE_ACTIVE,       /**< Link insync for data transfer */
	LINK_STATE_FAULT,        /**< Link error can not do data transfer */
} link_state_t;

/** @name RX message status */
/** @{ */

enum rcp_rx_status {
	RCP_RX_STS_DATA_MSG, /**< Recieved a normal data message */
	RCP_RX_STS_INIT_MSG, /**< Indicates link init/reset */
	RCP_RX_STS_NACK_MSG, /**< Recieved Nack msg, error data provided */
	RCP_RX_STS_ERROR,    /**< Error detected, error data provided */
};
/** @} */

typedef struct {
	bool ack_required;
	uint8_t tsn;
	uint8_t rsn;
	uint8_t ack_window; /**< Number of defered acks i.e. not sent */
} rcp_seq_num_t;

typedef struct {
	uint8_t stream;
	uint8_t msg_type;
	bool inc_tsn_after_send;
	rcp_seq_num_t seq_nums;
	uint8_t msg_subtype;
	rcp_buffer_hdl_t *payload_ptr;
} rcp_msg_t;

typedef struct {
	uint8_t stream;
	link_state_t state;
	rcp_seq_num_t seq_nums;
	link_result_t error;
	enum rcp_rx_status rx_status;
	struct rcp_error_info_t err_info;
	enum rcp_bus bus;
	bool encrypted; /*< Encrypt communication over the link. */
} lnk_sts_t;

typedef rcp_msg_t *rcp_msg_ptr_t;

/** Encode and marshal a data message into raw RCP data
 *
 * @param msg_ptr Message structure to be encoded
 * @param dest_buffer Destination buffer for raw, encoded data
 *
 * @return Total size of the encoded data
*/
uint16_t marshal_rcp_msg(rcp_msg_ptr_t msg_ptr, rcp_buffer_hdl_t *dest_buffer);

/** Decode raw RCP message from a buffer
 *
 * This function will allocate the target data buffer inside msg_ptr structure
 * which then will be handed over to the caller.
 *
 * @param msg_ptr Target message structure
 * @param rx_buffer Buffer with raw data to be decoded
 *
 * @return true if message was successfuly decoded, otherwise false.
 */
bool decode_rcp_msg(rcp_msg_ptr_t msg_ptr, rcp_buffer_hdl_t *rx_buffer);

/** Encode RCP payload size
 *
 * @param buffer_ptr target buffer
 * @param payload_length length to be encoded
 *
 * @return Pointer shifted by the encoded length
*/
uint8_t *put_payld_size_in_buff(uint8_t *buffer_ptr, uint16_t payload_length);

/** Decode payload size from a buffer
 *
 * @param buffer_ptr pointer to encoded length data
 *
 * @return decoded length field
 */
uint16_t get_payld_size_from_buff(uint8_t *buffer_ptr);

/** Format message type
 *
 * @param msg_type type to format
 * @return formatted type
 */
uint8_t msg_type2fmt(const uint8_t msg_type);

/** Populate RCP data message structure
 *
 * @param msg_ptr Destination message structure
 * @param stream stream ID
 * @param encrypted true if message payload shall be encrypted
 * @param seq_nums Pointer to current sequence number context
 * @param payload_ptr Pointer to message payload buffer
 */
void build_rcp_msg(rcp_msg_ptr_t msg_ptr, uint8_t stream, bool encrypted,
		   rcp_seq_num_t *seq_nums, rcp_buffer_hdl_t *payload_ptr);

void build_link_sync_message(rcp_msg_ptr_t msg_ptr, uint8_t stream, bool encrypted, bool inc_tsn_after_send,
			     rcp_seq_num_t *seq_nums, rcp_buffer_hdl_t *payload_ptr);

/** Populate RCP control message structure
 *
 * @param msg_ptr Destination message structure
 * @param msg_type Type of control message
 * @param msg_subtype Substype of control message
 * @param payload_ptr Pointer to message payload buffer
 */
void build_cntrl_rcp_msg(rcp_msg_ptr_t msg_ptr, uint8_t msg_type, uint8_t msg_subtype,
			 rcp_buffer_hdl_t *payload_ptr);

/** Populate RCP ACK message structure
 *
 * @param msg_ptr Destination message structure
 * @param seq_nums Pointer to current sequence number context
 */
void build_ack_to_rxmsg(rcp_msg_ptr_t msg_ptr, rcp_seq_num_t *seq_nums);

/** Populate RCP NACK message structure
 *
 * @param msg_ptr Destination message structure
 * @param seq_nums Pointer to current sequence number context
 * @param payload_ptr Pointer to message payload buffer
 */
void build_nack_to_rxmsg(rcp_msg_ptr_t msg_ptr, rcp_seq_num_t *seq_nums,
			 rcp_buffer_hdl_t *payload_ptr);

void build_data_stream_msg(rcp_msg_ptr_t send_msg, lnk_sts_t *lnk_sts_ptr,
			   rcp_buffer_hdl_t *buffer);

/**
 * Encode and publish a message to TX queue.
 *
 * @param send_msg Message to publish.
 *
 * @return
 * - true When the message is successfully marshalled and published to the TX queue.
 * The payload buffer associated with send_msg will be freed automatically.
 * - false In case of a failure (e.g. TX queue full). The function will NOT free the payload
 * buffer, letting user decide what to do with the data.
 */
bool send_rcp_msg(rcp_msg_ptr_t send_msg);

/**
 * TX message over chosen bus.
 *
 * Transmits a raw data buffer over bus using common bus API.
 *
 * @param lnk_sts_ptr link to send state/info
 * @param tx_buffer data to transfer.
 *
 * @return true if tx was successful.
 * @return false in case the bus reported an error.
 */
bool tx_rcp_msg(lnk_sts_t *lnk_sts_ptr, rcp_buffer_hdl_t *tx_buffer);

/** Increment a sequence number.
 *
 * Increments a number applying RCP sequence number constraints.
 *
 * @param current_value Sequence number to be incremented
 * @return Next sequence number
 */
uint8_t inc_seq_num(uint8_t current_value);

/** Calculate a difference between two sequence numbers.
 *
 * @param tsn_sent send sequence number
 * @param last_rsn receive sequence number
 * @return difference between tsn_sent and last_rsn
*/
uint8_t diff_seq_nums(uint8_t tsn_sent, uint8_t last_rsn);

/* Update RCP link status to active */
void set_rcp_link_active(lnk_sts_t *link_status);

/* Check if message has encrypted payload. */
bool is_msg_encrypted(rcp_msg_ptr_t msg_ptr);

/** @} end of rcp_protocol */

#endif
