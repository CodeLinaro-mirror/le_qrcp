/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "rcp_interface.h"
#include "rcp_interface_impl.h"
#include "rcp_tx_scheduler.h"
#include "demo_utils.h"
#include "bus_socket.h"

/************************************************************
 *
 * Test initization and reset
 *
 ************************************************************/

static int msg_num;
static int response_count;
static int max_response = 25;
static char reset_msg[] = {"Host Reseting Link"};
static char exit_msg[] = {EXIT_MSG};
static char resp_msg[] = {RESP_TEST_MSG};

static link_result_t send_host_msg(uint8_t stream, int num, rcp_buffer_hdl_t *msg_buffer)
{
	if (!msg_buffer) {
		return RCP_LINK_ERR_INVALID_BUFFER;
	}
	return send_numbered_msg(stream, "Host data msg: ", num, msg_buffer,
				 rcp_error_info_t * err_info_ptr);
}

uint8_t test_response(enum rcp_rx_status status, rcp_buffer_hdl_t *buffer_ptr, )
{
	printf("\n*****\nResponse Handler\n");

	if (buffer_ptr) {
		display_buffer("Host got msg:", buffer_ptr, true);
	}

	if (!buffer_ptr) {
		// Get buffer if none provided
		buffer_ptr = get_buffer(32);
	}

	if (event_type & RCP_RX_STS_PEER_ERROR) {
		printf("Got Peer Error\n");
		printf("Sending Exit Msg\n");
		snprintf((char *)buffer_ptr->data_ptr, buffer_ptr->max_size, "%s", exit_msg);
		buffer_ptr->size = strlen(exit_msg) + 1;
		link_send(RCP_STREAM_WIFI_MGMT, buffer_ptr);
		return RCP_RX_NO_ACTION;
	}

	link_result_t result;
	if (response_count++ == 0) {
		printf("Succesfully Synced Link\n");

		result = send_host_msg(RCP_STREAM_WIFI_MGMT, msg_num++, buffer_ptr);
	} else if (response_count < max_response) {
		result = send_host_msg(RCP_STREAM_WIFI_MGMT, msg_num++, buffer_ptr);
	} else {
		for (int i = 0; i < 7; i++) {
			printf("Sending Response Msg %d\n", i);
			snprintf((char *)buffer_ptr->data_ptr, buffer_ptr->max_size, "%s", resp_msg);
			buffer_ptr->size = strlen(resp_msg) + 1;
			link_send(RCP_STREAM_WIFI_MGMT, buffer_ptr);
		}
	}

	return RCP_RX_NO_ACTION;
}

int main(int argc, char *argv[])
{
	/* Initialize bus transport. */
	rcp_bus_socket_init();

	link_result_t result;

	result = init_rcp();

	if (result != RCP_LNK_SUCCESS) {
		printf("Init rcp failed with error: %s\n", display_error(result));
		exit(-1);
	}

	msg_num = 0;
	response_count = 0;

	printf("In State: %s\n", display_state(link_get_state(RCP_STREAM_WIFI_MGMT)));

	link_register_rx_handler(RCP_STREAM_WIFI_MGMT, test_response);

	rcp_buffer_hdl_t *msg_buffer = get_buffer(32);
	result = send_host_msg(RCP_STREAM_WIFI_MGMT, msg_num++, msg_buffer);

	if (result != RCP_LNK_SUCCESS) {
		printf("Failed to send first message: %s\n", display_error(result));
		exit(-1);
	}

	run_event_loop();

	printf("\nDone\n");
	rcp_bus_socket_close();
	return 0;
}