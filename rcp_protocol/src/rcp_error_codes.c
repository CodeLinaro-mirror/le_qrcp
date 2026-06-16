/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "rcp_error_codes.h"
#include <string.h>

static void set_seq_num_data(uint8_t actual, uint8_t expected, rcp_error_info_t *error_info)
{
	error_info->err_data.out_of_seq.actual = actual;
	error_info->err_data.out_of_seq.expected = expected;
}

void set_err_msg(const char *msg, uint8_t msg_size, rcp_error_info_t *error_info)
{
	if (msg && (msg_size > 0)) {
		error_info->err_code = RCP_ERR_MSG;
		msg_size = msg_size <= MAX_ERROR_MSG_SIZE ? msg_size : MAX_ERROR_MSG_SIZE;
		error_info->err_data.err_msg.msg_size = msg_size;
		memcpy(error_info->err_data.err_msg.msg, msg, msg_size);
	}
}

void set_rx_seq_err(uint8_t actual, uint8_t expected, rcp_error_info_t *error_info)
{
	error_info->err_code = RCP_ERR_TSN_OUT_SEQ;
	set_seq_num_data(actual, expected, error_info);
}

void set_ack_seq_err(uint8_t rx_rsn, uint8_t tsn_sent, rcp_error_info_t *error_info)
{
	error_info->err_code = RCP_ERR_RSN_OUT_SEQ;
	set_seq_num_data(rx_rsn, tsn_sent, error_info);
}

bool marshall_nak_error(rcp_buffer_hdl_t **buffer, rcp_error_info_t *error_info)
{
	// Info goes after err code
	if (!*buffer) {
		*buffer = get_buffer(MAX_ERROR_MSG_SIZE + 1);
	}
	rcp_buffer_hdl_t *nak_buffer = *buffer;
	uint8_t *info_ptr = nak_buffer->data_ptr;
	*info_ptr++ = error_info->err_code;
	nak_buffer->size = 1;

	enum rcp_error_code_t err_code = error_info->err_code;
	switch (err_code) {

	case RCP_ERR_MSG:
		uint8_t data_size = error_info->err_data.err_msg.msg_size;
		*info_ptr++ = data_size;
		memcpy(info_ptr, error_info->err_data.err_msg.msg, data_size);
		nak_buffer->size += data_size + 1;
		break;

	case RCP_ERR_TSN_OUT_SEQ:
	case RCP_ERR_RSN_OUT_SEQ:
		info_ptr[0] = error_info->err_data.out_of_seq.actual;
		info_ptr[1] = error_info->err_data.out_of_seq.expected;
		nak_buffer->size += 2;
		break;

	case RCP_ERR_UNHANDELED_STREAM:
	case RCP_ERR_HW_ERROR:
		// No data
		break;

	default:
		nak_buffer->size = 0;
		return false;
	}
	return true;
}

bool decode_nak_error_info(rcp_error_info_t *error_info, rcp_buffer_hdl_t *buffer)
{
	// Get err code
	if (!buffer) {
		return false;
	}
	// Get error code
	enum rcp_error_code_t err_code = *buffer->data_ptr;
	uint8_t *msg_data_ptr = ++buffer->data_ptr;
	uint16_t info_size = buffer->size - 1;

	error_info->err_code = RCP_ERR_UNDEFINED;
	switch (err_code) {
	case RCP_ERR_MSG:
		uint8_t msg_size = *msg_data_ptr++;
		if (msg_size > sizeof(error_info->err_data.err_msg.msg)) {
			msg_size = sizeof(error_info->err_data.err_msg.msg);
		}
		error_info->err_data.err_msg.msg_size = msg_size;
		memcpy(error_info->err_data.err_msg.msg, msg_data_ptr, msg_size);
		break;

	case RCP_ERR_TSN_OUT_SEQ:
	case RCP_ERR_RSN_OUT_SEQ:
		if (info_size < 2) {
			return false;
		}
		error_info->err_data.out_of_seq.actual = msg_data_ptr[0];
		error_info->err_data.out_of_seq.expected = msg_data_ptr[1];
		break;

	case RCP_ERR_UNHANDELED_STREAM:
	case RCP_ERR_HW_ERROR:
		// No data
		break;

	default:
		return false;
	}
	error_info->err_code = err_code;
	return true;
}
