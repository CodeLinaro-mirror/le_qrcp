/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "buffer_mgmt.h"
#include "rcp_cntrl_msgs.h"
#include "rcp_definitions.h"
#include "rcp_mux_dispatcher.h"
#include <rcp_log.h>
#include <rcp_encryption.h>
#include <rcp_config.h>

#include <stdlib.h>
#include <string.h>

#define NACK_THIS_MSG "TEST_NUM_NACK_THIS"

static cntrl_msg_handler_t registered_cntrl_msg_hdlr; // Custom control msg handler
static uint8_t mcu_capabilities;                      // Flags indicating MCU capabilities
static uint8_t mcu_vserial_port_count;                // Number of MCU virtual serial ports

static struct {
	uint8_t stream;
	bool active;
	rcp_rx_event_handler_t saved_handler;
} loopback;

#ifdef CONFIG_INCLUDE_TRANSPORT_TESTS
// Handler for test stream messages
static uint8_t handle_test_stream_msgs(enum rcp_rx_status status, rcp_buffer_hdl_t *buffer_ptr,
				       rcp_error_info_t *err_info_ptr);
#endif // CONFIG_INCLUDE_TRANSPORT_TESTS

#if RCP_ENCRYPTION
static struct {
	key_exchange_done_cb cb;
} key_exchange;
#endif

static uint8_t handle_loopback_data_msg(enum rcp_rx_status status, rcp_buffer_hdl_t *buffer_ptr,
					rcp_error_info_t *err_info_ptr)
{
	// Just send response with received data
	return RCP_RX_RESPONSE;
}

static void reset_loopback_state(void)
{
	loopback.stream = RCP_STREAM_NOT_DEFINED;
	loopback.active = false;
	loopback.saved_handler = NULL;
}

#if RCP_ENCRYPTION
/** Set current peer key and derive shared secret on RCP encryption context and respond with
 *  own public key.
 */
static void handle_key_exchange_req(rcp_msg_ptr_t req)
{
	if (!req->payload_ptr || !req->payload_ptr->data_ptr || req->payload_ptr->size == 0) {
		return;
	}

	RCP_LOG_INF("Received key exchange request from peer");
	/* Generate shared key from received peer key and set current context. */
	if (!rcp_enc_set_peer_key(req->payload_ptr->data_ptr, req->payload_ptr->size)) {
		RCP_LOG_ERR("Failed to generate key pair from received key");
		return;
	}

	/* Respond with current public key. */
	uint8_t *pub_key = NULL;
	uint16_t pub_key_len = rcp_enc_get_pub_key(&pub_key);

	if (!pub_key || pub_key_len == 0) {
		RCP_LOG_INF("Own public key not available - can't respond");
		return;
	}

	RCP_LOG_INF("Sending key exchange response");

	rcp_msg_t cntrl_msg;
	rcp_buffer_hdl_t *buf = get_buffer(RCP_BUF_SIZE);

	memcpy(buf->data_ptr, pub_key, pub_key_len);
	buf->size = pub_key_len;
	cntrl_msg.payload_ptr = buf;
	build_cntrl_rcp_msg(&cntrl_msg, RCP_MSG_TYPE_STATUS_IND, RCP_CNTRL_ENC_KEY_RESP, buf);

	if (!send_rcp_msg(&cntrl_msg)) {
		release_buffer(&buf);
	}
}

/** Accept peer key in response to key exchange request and derive a shared key. */
 static void handle_key_exchange_resp(rcp_msg_ptr_t req)
{
	bool status = true;

	if (!req->payload_ptr || !req->payload_ptr->data_ptr || req->payload_ptr->size == 0) {
		status = false;
		goto end;
	}

	/* Get key from message and set current context. */
	if (!rcp_enc_set_peer_key(req->payload_ptr->data_ptr, req->payload_ptr->size)) {
		RCP_LOG_ERR("Failed to generate key pair from received key");
		status = false;
	}
end:
	if (key_exchange.cb) {
		key_exchange.cb(status);
		/* Single use callback */
		key_exchange.cb = NULL;
	}
}

#endif

bool send_looback_ind(uint8_t stream, uint8_t state)
{
	rcp_buffer_hdl_t *buff_ptr = get_buffer(RCP_MCU_LOOP_SIZE);

	if (buff_ptr) {
		rcp_msg_t cntrl_msg;
		buff_ptr->data_ptr[RCP_MCU_LOOP_STREAM] = stream;
		buff_ptr->data_ptr[RCP_MCU_LOOP_ACTION] = state;
		buff_ptr->size = RCP_MCU_LOOP_SIZE;
		cntrl_msg.payload_ptr = buff_ptr;

		build_cntrl_rcp_msg(&cntrl_msg, RCP_MSG_TYPE_STATUS_IND, RCP_CNTRL_LOOPBACK_IND,
				    buff_ptr);
		return send_rcp_msg(&cntrl_msg);
	}
	return false;
}

void init_cntrl_status()
{
	registered_cntrl_msg_hdlr = NULL;
	mcu_vserial_port_count = 0;
	reset_loopback_state();

#ifdef CONFIG_INCLUDE_TRANSPORT_TESTS
	link_register_rx_handler(RCP_STREAM_TEST_ONLY, handle_test_stream_msgs);
#endif // CONFIG_INCLUDE_TRANSPORT_TESTS
}

void init_mcu_capability(uint8_t capabilities)
{
	mcu_capabilities = capabilities | RCP_MCU_READY_FLAG;
}

void init_mcu_vserial_port_count(uint8_t count)
{
	if (count > 4) {
		count = 4;
	}
	mcu_vserial_port_count = count;
}

bool send_capability_ind(void)
{
	rcp_buffer_hdl_t *buff_ptr = get_buffer(RCP_MCU_CAP_PAYLOAD_SIZE);
	if (buff_ptr) {
		rcp_msg_t cntrl_msg;
		uint8_t payload[RCP_MCU_CAP_PAYLOAD_SIZE] = {
			mcu_capabilities,
			mcu_vserial_port_count,
		};

		if (write_to_buffer(buff_ptr, payload, sizeof(payload))) {
			build_cntrl_rcp_msg(&cntrl_msg, RCP_MSG_TYPE_STATUS_IND, RCP_CNTRL_CAP_IND,
					    buff_ptr);
			return send_rcp_msg(&cntrl_msg);
		}
	}
	return false;
}

/*
 * This will activate loopback specific stream
 * Implicitly removes oopback on another stream
 */
bool send_loopback_request(uint8_t stream)
{
	if ((stream == RCP_STREAM_LNK_MGMT) || (stream >= RCP_STREAM_DELIMITER)) {
		RCP_LOG_WRN("Invalid loopback stream = %d", stream);
		return false;
	}

	rcp_buffer_hdl_t *buff_ptr = get_buffer(RCP_MCU_LOOP_SIZE);
	if (buff_ptr) {
		rcp_msg_t cntrl_msg;
		buff_ptr->data_ptr[RCP_MCU_LOOP_STREAM] = stream;
		buff_ptr->data_ptr[RCP_MCU_LOOP_ACTION] = RCP_MCU_LOOP_ACTIVE;
		buff_ptr->size = RCP_MCU_LOOP_SIZE;
		cntrl_msg.payload_ptr = buff_ptr;

		build_cntrl_rcp_msg(&cntrl_msg, RCP_MSG_TYPE_CONTROL, RCP_CNTRL_LOOPBACK_REQ,
				    buff_ptr);
		return send_rcp_msg(&cntrl_msg);
	}
	return false;
}

/*
 * This remove the loopback on any stream
 */
bool send_loopback_cancel()
{
	rcp_buffer_hdl_t *buff_ptr = get_buffer(RCP_MCU_LOOP_SIZE);
	if (buff_ptr) {
		rcp_msg_t cntrl_msg;

		buff_ptr->data_ptr[RCP_MCU_LOOP_ACTION] = RCP_MCU_LOOP_INACTIVE;
		buff_ptr->data_ptr[RCP_MCU_LOOP_STREAM] = RCP_STREAM_NOT_DEFINED;
		buff_ptr->size = RCP_MCU_LOOP_SIZE;
		cntrl_msg.payload_ptr = buff_ptr;
		build_cntrl_rcp_msg(&cntrl_msg, RCP_MSG_TYPE_CONTROL, RCP_CNTRL_LOOPBACK_REQ,
				    buff_ptr);

		return send_rcp_msg(&cntrl_msg);
	}
	return false;
}

void handle_loopback_req(rcp_msg_ptr_t cntrl_msg)
{
	if ((!cntrl_msg->payload_ptr) || (cntrl_msg->payload_ptr->size > RCP_MCU_LOOP_SIZE)) {
		// Ignore invalid msg
		RCP_LOG_WRN("Ignoring malformed loopback request");
		return;
	}
	uint8_t stream = cntrl_msg->payload_ptr->data_ptr[RCP_MCU_LOOP_STREAM];
	uint8_t req_action = cntrl_msg->payload_ptr->data_ptr[RCP_MCU_LOOP_ACTION];

	if ((stream == loopback.stream) && (loopback.active) &&
	    (req_action == RCP_MCU_LOOP_ACTIVE)) {
		// Loop alreaday active on requested stream
		RCP_LOG_INF("Stream %d already in loopback", loopback.stream);
		send_looback_ind(loopback.stream, RCP_MCU_LOOP_ACTIVE);
	} else if (loopback.active) {
		// Remove existing loop
		link_register_rx_handler(loopback.stream, loopback.saved_handler);
		RCP_LOG_INF("Stream %d loopback cancelled", loopback.stream);
		send_looback_ind(loopback.stream, RCP_MCU_LOOP_INACTIVE);
		reset_loopback_state();
	}

	if (req_action == RCP_MCU_LOOP_ACTIVE) {
		// Activate loopback
		// Save existing handler
		loopback.saved_handler = get_rx_handler(stream);

		// Set loopback
		loopback.active = true;
		loopback.stream = stream;
		link_register_rx_handler(loopback.stream, handle_loopback_data_msg);
		RCP_LOG_INF("Stream %d looped back", loopback.stream);
		send_looback_ind(loopback.stream, RCP_MCU_LOOP_ACTIVE);
	}
}

bool handle_builtin_function(rcp_msg_ptr_t cntrl_msg)
{
	switch (cntrl_msg->msg_type) {
	case RCP_MSG_TYPE_CONTROL:
		switch (cntrl_msg->msg_subtype) {
		case RCP_CNTRL_CAP_REQ: {
			// Automatically return caapabilities
			RCP_LOG_INF("Got Capability Request");
			if (!send_capability_ind()) {
				RCP_LOG_ERR("Failed to send capability reponse");
			}
			return true;
		} break;
		case RCP_CNTRL_LOOPBACK_REQ: {
			handle_loopback_req(cntrl_msg);
			return true;
		} break;
#if RCP_ENCRYPTION
		case RCP_CNTRL_ENC_KEY_REQ: {
			handle_key_exchange_req(cntrl_msg);
			return true;
		} break;
#endif
		}
		break;
	case RCP_MSG_TYPE_STATUS_IND:
		switch (cntrl_msg->msg_subtype) {
#if RCP_ENCRYPTION
		case RCP_CNTRL_ENC_KEY_RESP:
			handle_key_exchange_resp(cntrl_msg);
			return true;
#endif
		}
	}
	return false;
}

void handle_cntrl_msgs(rcp_msg_ptr_t cntrl_msg)
{
	if (handle_builtin_function(cntrl_msg)) {
		return;
	}
	// If not built in function defer to custom handler
	if (registered_cntrl_msg_hdlr == NULL) {
		RCP_LOG_WRN("No handling defined for Cntrl Message Type=%d Subtype=%d",
			    cntrl_msg->msg_type, cntrl_msg->msg_subtype);
	} else {
		// Dispatch to control message handler
		registered_cntrl_msg_hdlr(cntrl_msg->msg_type, cntrl_msg->msg_subtype,
					  cntrl_msg->payload_ptr);
	}
}

void register_cntrl_msg_hdlr(cntrl_msg_handler_t handler)
{
	registered_cntrl_msg_hdlr = handler;
}

bool send_capability_request()
{
	rcp_msg_t cntrl_msg;

	build_cntrl_rcp_msg(&cntrl_msg, RCP_MSG_TYPE_CONTROL, RCP_CNTRL_CAP_REQ, NULL);
	return send_rcp_msg(&cntrl_msg);
}

#ifdef CONFIG_INCLUDE_TRANSPORT_TESTS
static uint8_t handle_test_stream_msgs(enum rcp_rx_status status, rcp_buffer_hdl_t *buffer_ptr,
				       rcp_error_info_t *err_info_ptr)
{
	switch (status) {
	case RCP_RX_STS_INIT_MSG:
		if (buffer_ptr) {
			if (buffer_ptr->data_ptr[0] == TEST_FUNC_RESET) {
				// Acknowledge test with responds - completes reset
				buffer_ptr->data_ptr[0] = TEST_FUNC_RESET_ACK;
				return RCP_RX_RESPONSE;
			}
		}
		// Fall through

	case RCP_RX_STS_DATA_MSG:

		if ((buffer_ptr) && (buffer_ptr->size)) {
			switch (buffer_ptr->data_ptr[0]) {
			case TEST_FUNC_BASIC_HOST:
				buffer_ptr->data_ptr[0] = TEST_FUNC_BASIC_RESP;
				buffer_ptr->size = 1;
				return RCP_RX_RESPONSE;
				break;

			case TEST_NUM_NACK_THIS:
				set_err_msg(NACK_THIS_MSG, strlen(NACK_THIS_MSG), err_info_ptr);
				return RCP_RX_NACK;
			default:
				break;
			}
		}
		break;

	case RCP_RX_STS_ERROR:
		// Return error in NACK message
		return RCP_RX_NACK;
		break;

	case RCP_RX_STS_NACK_MSG:
		break;

	default:
		RCP_LOG_ERR("Unexpected status=%d", status);
		break;
	}
	// Just send response with received data
	return RCP_RX_NO_ACTION;
}

#endif // CONFIG_INCLUDE_TRANSPORT_TESTS

#if RCP_ENCRYPTION
bool send_key_exchange_request(key_exchange_done_cb cb)
{
	rcp_msg_t cntrl_msg;
	rcp_buffer_hdl_t *buf = get_buffer(RCP_BUF_SIZE);

	uint8_t *pub_key = NULL;
	uint16_t pub_key_len = rcp_enc_get_pub_key(&pub_key);

	RCP_LOG_INF("Sending key exchange request: %p, %d", pub_key, pub_key_len);

	memcpy(buf->data_ptr, pub_key, pub_key_len);
	buf->size = pub_key_len;
	cntrl_msg.payload_ptr = buf;

	if (cb) {
		key_exchange.cb = cb;
	}

	build_cntrl_rcp_msg(&cntrl_msg, RCP_MSG_TYPE_CONTROL, RCP_CNTRL_ENC_KEY_REQ, buf);

	if (!send_rcp_msg(&cntrl_msg)) {
		release_buffer(&buf);
		return false;
	}

	return true;
}
#endif
