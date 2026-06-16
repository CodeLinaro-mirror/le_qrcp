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
static int max_response = 3;
static char reset_msg[] = {"Host Reseting Link"};
static char exit_msg[] = {EXIT_MSG};

static link_result_t send_host_msg(uint8_t stream, int num, rcp_buffer_hdl_t *msg_buffer)
{
	if (!msg_buffer) {
		return RCP_LINK_ERR_INVALID_BUFFER;
	}
	return send_numbered_msg(stream, "Host msg: ", num, msg_buffer);
}

uint8_t test_response(enum rcp_rx_status status, event_type, rcp_buffer_hdl_t *buffer_ptr,
		      rcp_error_info_t *err_info_ptr)
{
	printf("\n*****\nResonse Handle\n");

	if (buffer_ptr) {
		display_buffer("Host got msg:", buffer_ptr, true);
	}

	link_state_t current_state = link_get_state(RCP_STREAM_WIFI_MGMT);
	if (current_state != LINK_STATE_ACTIVE) {
		printf("Test 1 failed Link State %s not LINK_STATE_ACTIVE\n",
		       display_state(current_state));
		exit(-1);
	}

	if (response_count++ < max_response) {
		link_result_t result = send_host_msg(RCP_STREAM_WIFI_MGMT, msg_num++, buffer_ptr);
	} else {
		printf("End of Test - No further action from host\n");
		link_reset(RCP_STREAM_WIFI_MGMT, NULL);
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