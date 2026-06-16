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

static int msg_num;
static int response_count;
static int max_response = 2;
static char exit_msg[] = {EXIT_MSG};

static link_result_t send_mcu_msg(uint8_t stream, int num, rcp_buffer_hdl_t *msg_buffer)
{
	if (!msg_buffer) {
		return RCP_LINK_ERR_INVALID_BUFFER;
	}
	return send_numbered_msg(stream, "MCU msg: ", num, msg_buffer);
}

uint8_t handle_test_2(uint8_t event_type, rcp_buffer_hdl_t *buffer_ptr)
{
	printf("\n*****\nResponse Handler 2\n");
	if (!buffer_ptr) {
		printf("No Data in Msg\n");
		return RCP_RX_NO_ACTION;
	}

	display_buffer("MCU got msg:", buffer_ptr, true);

	if (strncmp(buffer_ptr->data_ptr, exit_msg, buffer_ptr->size) == 0) {
		printf("Got exit msg from host - exiting\n");
		exit(0);
	}

	if (event_type & RCP_RX_STS_SYNCING) {
		printf("Wifi Mgmt Syncing - Sending Ack\n");
		return RCP_RX_ACK;
	} else {
		// Sending response
		printf("Send response to Host\n");
		snprintf((char *)buffer_ptr->data_ptr, buffer_ptr->max_size, "%s %3d", "MCU Msg",
			 msg_num);
		buffer_ptr->size = 12;
	}
	return RCP_RX_RESPONSE;
}

uint8_t handle_test_1(uint8_t event_type, rcp_buffer_hdl_t *buffer_ptr)
{
	printf("\n*****\nResponse Handler 1\n");
	if (buffer_ptr) {
		display_buffer("MCU got msg:", buffer_ptr, true);
	}
	if (event_type & RCP_RX_STS_SYNCING) {
		printf("Wifi Mgmt Syncing");
		if (response_count == 0) {
			printf(" - Sending Response\n");
		} else {
			printf(" - Ack message\n");
			response_count = 0;
			msg_num = 0;
			register_data_msg_handler(RCP_STREAM_WIFI_MGMT, handle_test_2);
			return RCP_RX_ACK;
		}
	}

	send_mcu_msg(RCP_STREAM_WIFI_MGMT, response_count++, buffer_ptr);

	return RCP_RX_NO_ACTION;
}

int main(int argc, char *argv[])
{
	/* Initialize bus transport. */
	rcp_bus_socket_init();

	// Initialize
	init_rcp();

	register_data_msg_handler(RCP_STREAM_WIFI_MGMT, handle_test_1);

	run_event_loop();

	rcp_bus_socket_close();
	printf("\n Done\n");
	return 0;
}