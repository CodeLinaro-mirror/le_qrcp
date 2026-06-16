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

#include "rcp_error_codes.h"
#include "rcp_interface.h"
#include "rcp_interface_impl.h"
#include "rcp_tx_scheduler.h"
#include "demo_utils.h"
#include "bus_socket.h"

#define NUM_BT_RESPONSES 5

uint16_t bt_count;
uint16_t wifi_count;
uint16_t max_bt_msg;

uint8_t handle_wifi_msgs(uint8_t event_type, rcp_buffer_hdl_t *buffer_ptr)
{
	printf("Dispatch triggered for WiFi Cntrl Stream events=%d,\n", event_type);
	if (event_type & RCP_RX_STS_ERROR) {
		printf("Msg %d Got error from Peer - Stopping responding\n", wifi_count++);
		// Stop sending data
		printf("\n*****\n");
		return RCP_RX_NO_ACTION;
	}
	if (wifi_count++ == 0) {
		printf("Wifi Cntrl Sending Ack");
		printf("\n*****\n");
		return RCP_RX_ACK;
	}
	if (event_type & RCP_RX_STS_PAYLOAD) {

		display_buffer("WIFI MCU <- HOST", buffer_ptr, true);

		snprintf((char *)buffer_ptr->data_ptr, buffer_ptr->max_size,
			 "%d - WiFi Response msg from MCU", wifi_count++);
		buffer_ptr->size = 32;
		printf("\n*****\n");
		return RCP_RX_RESPONSE;
	}
	printf("\n*****\n");
	return RCP_RX_NO_ACTION;
}

uint8_t handle_bt_msgs(uint8_t event_type, rcp_buffer_hdl_t *buffer_ptr)
{
	printf("Dispatch triggered BT Stream events=%d,\n", event_type);
	display_buffer("BT MCU <- HOST", buffer_ptr, true);
	if (bt_count < max_bt_msg) {
		snprintf((char *)buffer_ptr->data_ptr, buffer_ptr->max_size,
			 "%d - BT Response msg from MCU", bt_count++);
		buffer_ptr->size = 32;
		printf("\n*****\n");
		return RCP_RX_RESPONSE;
	} else {
		printf("Reached limit %d - now Rejecting Host messages\n", bt_count++);
		set_err_code(buffer_ptr, RCP_ERR_REJECTED);
		printf("\n*****\n");
		return RCP_RX_NACK;
	}
}

int main(int argc, char *argv[])
{
	if (argc == 2) {
		// Asume number of wifi require wifi responses
		max_bt_msg = strtol(argv[1], NULL, 10);
	} else {
		// Set default
		max_bt_msg = NUM_BT_RESPONSES;
	}
	printf("BT limit=%d\n ", max_bt_msg);

	/* Initialize bus transport. */
	rcp_bus_socket_init();

	// Initialize
	init_rcp();

	bt_count = 0;
	wifi_count = 0;

	register_data_msg_handler(RCP_STREAM_WIFI_MGMT, handle_wifi_msgs);

	register_data_msg_handler(RCP_STREAM_BT, handle_bt_msgs);

	run_event_loop();

	rcp_bus_socket_close();
	printf("\n Done\n");
	return 0;
}