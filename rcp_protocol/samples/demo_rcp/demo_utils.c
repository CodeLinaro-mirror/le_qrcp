/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "demo_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ERR_CODE 5
static char *rcp_error_txt[MAX_ERR_CODE] = {
	"RCP_ERR_NONE",       "RCP_ERR_OUT_SEQ_MSG", "RCP_ERR_UNHANDELED_STREAM",
	"RCP_ERR_NOT_SYNCED", "RCP_ERR_REJECTED",
};

int get_rnd_number(int max_bound)
{
	return rand() % (max_bound + 1);
}

uint16_t random_fill(rcp_buffer_hdl_t *buffer_ptr, uint16_t max_size)
{
	if (buffer_ptr == NULL) {
		printf("Rx Empty Buffer");
		return 0;
	}
	uint16_t size = get_rnd_number(max_size - 1) + 1;
	uint8_t *dest_ptr = buffer_ptr->data_ptr;
	for (int i = 0; i < size; ++i) {
		*dest_ptr++ = (uint8_t)(((double)rand() / (RAND_MAX)) * 32 + 1);
	}
	buffer_ptr->size = size;
	return size;
}

void display_buffer(char *title, rcp_buffer_hdl_t *buffer_ptr, bool data_as_text)
{
	char str_data[65];
	int string_size;

	if (buffer_ptr == NULL) {
		printf("Rx Empty Buffer");
		return;
	}

	printf("\n%s  Size:%d\n", title, buffer_ptr->size);

	if (data_as_text) {
		// truncate data
		string_size = buffer_ptr->size < 64 ? buffer_ptr->size : 64;
		strlcpy(str_data, (char *)buffer_ptr->data_ptr, string_size + 1);
		printf("%s", str_data);
	} else {
		for (int i = 0; i < buffer_ptr->size; ++i) {
			printf("%02x ", buffer_ptr->data_ptr[i]);
		}
	}
	printf("\n-----\n");
}

char *display_err_code(uint8_t error)
{
	if (error < MAX_ERR_CODE) {
		return rcp_error_txt[error];
	} else {
		return "UNKNOWN";
	}
}

char *display_error(link_result_t error)
{
	switch (error) {
	case RCP_LNK_SUCCESS:
		return "RCP_LNK_SUCCESS";
	case RCP_LNK_NO_RX_DATA:
		return "RCP_LNK_NO_RX_DATA";
	case RCP_LNK_TIMEOUT:
		return "RCP_LNK_TIMEOUT";
	case RCP_LNK_ERR_TX_NO_RESOURCES:
		return "RCP_LNK_ERR_TX_NO_RESOURCES";
	case RCP_LNK_ERR_UNSUPPORTED_STREAM:
		return "RCP_LNK_ERR_UNSUPPORTED_STREAM";
	case RCP_LINK_ERR_INVALID_BUFFER:
		return "RCP_LINK_ERR_INVALID_BUFFER";
	case RCP_LNK_ERROR:
		return "  RCP_LNK_ERROR";
	default:
		return "Error code unknown";
	}
}

char *display_state(link_state_t state)
{
	switch (state) {
	case LINK_STATE_IDLE:
		return "IDLE";
	case LINK_STATE_ACTIVE:
		return "ACTIVE";
	case LINK_STATE_FAULT:
		return "FAULT";
	case LINK_STATE_SYNCING:
		return "SYNCING";
	default:
		return "Unknown state";
	}
}

link_result_t send_numbered_msg(uint8_t stream, const char *base_msg, int message_num,
				rcp_buffer_hdl_t *msg_buffer)
{
	snprintf((char *)msg_buffer->data_ptr, msg_buffer->max_size, "%s %3d", base_msg, message_num);
	msg_buffer->size = strlen(base_msg) + 5;
	return link_send(stream, msg_buffer);
}