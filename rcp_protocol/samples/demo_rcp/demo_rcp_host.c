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

#define NUM_WIFI_RESPONSES 5

char *msgs[] = {
	"Initial Message 1",
	"Initial Message 2",
	"Another Message 3",
};

static uint16_t bt_count;
static uint16_t wifi_count;
static uint16_t max_wifi_messages;

uint8_t handle_expected_error(uint8_t event_type, rcp_buffer_hdl_t *buffer_ptr)
{
	printf("Dispatch triggered for WiFi Data Stream on msg=%d,\n", event_type);
	if (event_type & RCP_RX_STS_ERROR) {
		if (event_type & RCP_RX_STS_PAYLOAD) {
			uint8_t error = *buffer_ptr;
			printf("Got error from peer %s\n", display_err_code(error));
		}
	} else {
		printf("Unexpected Result for handler");
	}
	printf("\n*****\n");
}

uint8_t handle_wifi_response(uint8_t event_type, rcp_buffer_hdl_t *buffer_ptr)
{
	printf("Dispatch triggered for WiFi Cntrl Stream on msg=%d,\n", event_type);
	if (event_type & RCP_RX_STS_PAYLOAD) {
		display_buffer("WIFI HOST <- MCU", buffer_ptr, true);

		if (wifi_count < max_wifi_messages) {
			snprintf((char *)buffer_ptr->data_ptr, buffer_ptr->max_size,
				 "%d - WIFI Response msg from HOST", wifi_count++);
			buffer_ptr->size = 32;
			return RCP_RX_RESPONSE;
		} else {
			printf("Reached limit %d now Rejecting Host messages\n", max_wifi_messages);
			set_err_code(buffer_ptr, RCP_ERR_REJECTED);
			printf("\n*****\n");
			return RCP_RX_NACK;
		}
	} else {
		printf("No data in message\n");
	}
	printf("\n*****\n");
}

uint8_t handle_bt_response(uint8_t event_type, rcp_buffer_hdl_t *buffer_ptr)
{
	printf("Dispatch triggered for BT Cntrl Stream events=%d,\n", event_type);
	if (event_type & RCP_RX_STS_ERROR) {
		if (event_type & RCP_RX_STS_PAYLOAD) {
			display_buffer("Nack Data", buffer_ptr, false);
			uint8_t error = get_err_code(buffer_ptr);
			printf("Got error from peer %s\n", display_err_code(error));
		}
		printf("Msg %d Got error from Peer - Stopping responding\n", bt_count++);
		// Stop sending data
		return RCP_RX_NO_ACTION;
	}
	if (event_type & RCP_RX_STS_PAYLOAD) {
		display_buffer("BT HOST <- MCU", buffer_ptr, true);
	}

	return RCP_RX_NO_ACTION;
}

int main(int argc, char *argv[])
{
	if (argc == 2) {
		// Asume number of wifi require wifi responses
		max_wifi_messages = strtol(argv[1], NULL, 10);

	} else {
		// Set default
		max_wifi_messages = NUM_WIFI_RESPONSES;
	}

	printf("WiFi limit=%d\n ", max_wifi_messages);

	/* Initialize bus transport. */
	rcp_bus_socket_init();
	init_rcp();

	bt_count = 0;
	wifi_count = 0;

	rcp_buffer_hdl_t *msg_buffer;

	msg_buffer = get_buffer(100);
	strlcpy((char *)msg_buffer->data_ptr, msgs[0], msg_buffer->max_size);
	msg_buffer->size = 18;
	display_buffer("BT HOST -> MCU", msg_buffer, true);
	link_request(RCP_STREAM_BT, msg_buffer, handle_bt_response);

	msg_buffer = get_buffer(100);
	strlcpy((char *)msg_buffer->data_ptr, msgs[0], msg_buffer->max_size);
	msg_buffer->size = 18;
	display_buffer("BT HOST -> MCU", msg_buffer, true);
	link_request(RCP_STREAM_BT, msg_buffer, handle_bt_response);

	if (max_wifi_messages) {
		msg_buffer = get_buffer(100);
		strlcpy((char *)msg_buffer->data_ptr, msgs[1], msg_buffer->max_size);
		msg_buffer->size = 18;
		display_buffer("WIFI HOST -> MCU", msg_buffer, true);
		link_request(RCP_STREAM_WIFI_MGMT, msg_buffer, handle_wifi_response);

		// Expect no data response - send second
		msg_buffer = get_buffer(100);
		strlcpy((char *)msg_buffer->data_ptr, msgs[2], msg_buffer->max_size);
		msg_buffer->size = 18;
		display_buffer("WIFI HOST -> MCU", msg_buffer, true);
		link_request(RCP_STREAM_WIFI_MGMT, msg_buffer, handle_wifi_response);
	}

	msg_buffer = get_buffer(32);
	msg_buffer->size = random_fill(msg_buffer, 32);
	display_buffer("WIFI DATA HOST -> MCU", msg_buffer, false);
	link_request(RCP_STREAM_WIFI_DATA, msg_buffer, handle_expected_error);

	run_event_loop();

	printf("\nDone\n");
	rcp_bus_socket_close();
	return 0;
}