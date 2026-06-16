/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "rcp_bus.h"
#include "rcp_definitions.h"
#include "rcp_interface_impl.h"
#include "rcp_cntrl_msgs.h"
#include "rcp_protocol.h"
#include "rcp_mux_dispatcher.h"
#include "rcp_tx_scheduler.h"
#include "rcp_os.h"
#include "rcp_log.h"
#include "rcp_event.h"
#include "rcp_config.h"
#include "rcp_encryption.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/select.h>
#include <stdlib.h>

#define BUS_TX_RETRIES        3
#define BUS_TX_RETRY_DELAY_MS 100

static lnk_sts_t stream_status[RCP_STREAM_DELIMITER];
static bool rcp_link_initialized;

/* Indicate which buses are activate and usable */
static bool active_bus[RCP_BUS_MAX];

/* Forward declaration: helpers below need to access current stream state. */
static lnk_sts_t *get_stream_status(uint8_t stream);

static bool get_rx_data(rcp_msg_ptr_t msg_ptr, enum rcp_bus bus)
{
	uint16_t rx_len = rcp_bus_get_rx_len(bus);
	if (rx_len == 0) {
		return false;
	}

	rcp_buffer_hdl_t *rx_buffer = get_buffer(RCP_BUF_SIZE);

	if (!rx_buffer) {
		return false;
	}
	/* Amount of data that was actually received. */
	ssize_t rx_data_size = 0;

	enum rcp_bus_status ret =
		rcp_bus_read(bus, rx_buffer->data_ptr, rx_len, (uint16_t *)&rx_data_size);

	if (ret != RCP_BUS_OK) {
		if (ret != RCP_BUS_ERR_EMPTY) {
			RCP_LOG_ERR("RX bus failure err=%d", ret);
		}
	} else if (rx_data_size == 0) {
		RCP_LOG_WRN("No RX data");
	} else if (rx_data_size < RCP_HDR_SIZE) {
		RCP_LOG_ERR("RX Invalid Msg size %d<%d", ret, RCP_HDR_SIZE);
	} else {
		rx_buffer->size = rx_data_size;
		if (decode_rcp_msg(msg_ptr, rx_buffer)) {
			return true;
		}
		RCP_LOG_ERR("Rx Decode failed");
	}

	release_buffer(&rx_buffer);
	return false;
}

static void init_lnk_stat(uint8_t link_type, bool encrypt, enum rcp_bus bus, uint8_t tsn, lnk_sts_t *lnk_sts_ptr)
{
	lnk_sts_ptr->stream = link_type;
	lnk_sts_ptr->encrypted = encrypt;
	lnk_sts_ptr->state = LINK_STATE_IDLE;
	lnk_sts_ptr->error = RCP_LNK_SUCCESS;
	lnk_sts_ptr->bus = bus;
	lnk_sts_ptr->rx_status = 0;
	lnk_sts_ptr->seq_nums.ack_required = false;
	lnk_sts_ptr->seq_nums.rsn = 0;
	lnk_sts_ptr->seq_nums.tsn = tsn & 0x07;
	lnk_sts_ptr->seq_nums.ack_window = 0;
}

/* Set bus as active if not already set. */
static void activate_bus(enum rcp_bus bus)
{
	if(!active_bus[bus] && bus != RCP_BUS_INACTIVE) {
		if (rcp_bus_open(bus) != RCP_BUS_OK) {
			RCP_LOG_ERR("Unable to open bus %d\n", bus);
			return;
		} else {
			active_bus[bus] = true;
		}
	}
}

static void init_links(uint8_t tsn)
{
	for (int i = 0; i < RCP_STREAM_DELIMITER; i++) {
		// Baseline link status
		stream_status[i].stream = RCP_STREAM_NOT_DEFINED;
	}
	init_lnk_stat(RCP_STREAM_LNK_MGMT, false, RCP_LNK_MGMT_BUS, tsn,
		      &stream_status[RCP_STREAM_LNK_MGMT]);
	init_lnk_stat(RCP_STREAM_WIFI_MGMT, RCP_ENCRYPT_WIFI_MGMT, RCP_WIFI_BUS, tsn,
		      &stream_status[RCP_STREAM_WIFI_MGMT]);
	init_lnk_stat(RCP_STREAM_WIFI_DATA, RCP_ENCRYPT_WIFI_DATA, RCP_WIFI_BUS, tsn,
		      &stream_status[RCP_STREAM_WIFI_DATA]);
	init_lnk_stat(RCP_STREAM_BT, RCP_ENCRYPT_BT,
		RCP_BT_BUS, tsn, &stream_status[RCP_STREAM_BT]);
	init_lnk_stat(RCP_STREAM_THREAD, RCP_ENCRYPT_THREAD,
		RCP_DEFAULT_BUS, tsn, &stream_status[RCP_STREAM_THREAD]);
	init_lnk_stat(RCP_STREAM_AT_COMMANDS, false, RCP_DEFAULT_BUS, tsn,
		      &stream_status[RCP_STREAM_AT_COMMANDS]);
	init_lnk_stat(RCP_STREAM_TEST_ONLY, false, RCP_TEST_BUS, tsn,
		      &stream_status[RCP_STREAM_TEST_ONLY]);
	init_lnk_stat(RCP_STREAM_VSERIAL0, false, RCP_DEFAULT_BUS, tsn,
		      &stream_status[RCP_STREAM_VSERIAL0]);
	init_lnk_stat(RCP_STREAM_VSERIAL1, false, RCP_DEFAULT_BUS, tsn,
		      &stream_status[RCP_STREAM_VSERIAL1]);
	init_lnk_stat(RCP_STREAM_VSERIAL2, false, RCP_DEFAULT_BUS, tsn,
		      &stream_status[RCP_STREAM_VSERIAL2]);
	init_lnk_stat(RCP_STREAM_VSERIAL3, false, RCP_DEFAULT_BUS, tsn,
		      &stream_status[RCP_STREAM_VSERIAL3]);

	for (int i = 0; i < RCP_STREAM_DELIMITER; i++) {
		if (stream_status[i].stream != RCP_STREAM_NOT_DEFINED) {
			activate_bus(stream_status[i].bus);
		}
	}
}

static lnk_sts_t *get_stream_status(uint8_t stream)
{
	if ((stream < RCP_STREAM_DELIMITER) && (stream != RCP_STREAM_NOT_DEFINED)) {
		if (stream_status[stream].stream == stream) {
			return &stream_status[stream];
		} else {
			RCP_LOG_ERR("Status error stream Id %d<>%d", stream_status[stream].stream,
				    stream);
		}
	} else {
		RCP_LOG_ERR("Invalid Request status stream=%d", stream);
	}
	return NULL;
}

link_result_t init_rcp()
{
	rcp_link_initialized = false;
	srand(rcp_os_time_ms());
	int start_tsn = rand() % (7);
	RCP_LOG_INF("setting tsn %d", start_tsn);
	rcp_events_init();
	init_links(start_tsn);
	create_buffers();
	init_tx_queue();
	init_cntrl_status();
	rcp_link_initialized = true;

#if RCP_ENCRYPTION
	rcp_encryption_init();
#endif
	// Error handling TBA
	return RCP_LNK_SUCCESS;
}

bool link_is_initialized(void)
{
	return rcp_link_initialized;
}

link_result_t link_request(uint8_t stream, rcp_buffer_hdl_t *tx_buffer,
			   rcp_rx_event_handler_t response_cb)
{
	rcp_msg_t send_msg;
	lnk_sts_t *lnk_sts_ptr = get_stream_status(stream);

	build_data_stream_msg(&send_msg, lnk_sts_ptr, tx_buffer);

	if (!send_rcp_msg(&send_msg)) {
		return RCP_LNK_ERR_TX_NO_RESOURCES;
	}
	if (response_cb) {
		register_data_msg_handler(stream, response_cb);
	}
	return RCP_LNK_SUCCESS;
}

link_result_t link_send(uint8_t stream, rcp_buffer_hdl_t *tx_buffer)
{
	return link_request(stream, tx_buffer, NULL);
}

link_result_t link_register_rx_handler(uint8_t stream, rcp_rx_event_handler_t response_cb)
{
	if (!register_data_msg_handler(stream, response_cb)) {
		return RCP_LNK_ERROR;
	}
	return RCP_LNK_SUCCESS;
}

link_result_t link_reset(uint8_t stream, rcp_buffer_hdl_t *tx_buffer)
{
	lnk_sts_t *lnk_sts_ptr = get_stream_status(stream);
	lnk_sts_ptr->state = LINK_STATE_IDLE;
	return link_request(stream, tx_buffer, NULL);
}

void run_event_loop()
{
	while (true) {
		run_event_loop_once();
	}
}

link_state_t link_get_state(uint8_t stream)
{
	lnk_sts_t *lnk_sts_ptr = get_stream_status(stream);
	if (lnk_sts_ptr) {
		return lnk_sts_ptr->state;
	} else {
		return LINK_STATE_FAULT;
	}
}

/* Single-iteration event loop step.  Call from a kthread loop that checks
 * kthread_should_stop() between iterations.  Same logic as run_event_loop()
 * but returns after one pass instead of looping forever. */
void run_event_loop_once(void)
{
	rcp_msg_t rx_msg;
	uint8_t events = rcp_events_wait_for_any(
		RCP_EVENT_TX_DATA | RCP_EVENT_RX_DATA, 1000);

	if ((events & RCP_EVENT_TX_DATA) && !is_tx_empty()) {
		events = rcp_events_get();
		if (events & RCP_EVENT_TX_RDY) {
			rcp_schedule_evt_t *event = view_next_tx_event();
			if (event) {
				lnk_sts_t *lnk_status = get_stream_status(event->stream);
				if (lnk_status) {
					if (tx_rcp_msg(lnk_status, event->msg_buffer)) {
						if (event->tsn_inc) {
							lnk_status->seq_nums.tsn =
								inc_seq_num(lnk_status->seq_nums.tsn);
						}
						if (event->reset_ack_window) {
							lnk_status->seq_nums.ack_window = 0;
						}
						pop_next();
					} else if (event->num_retries++ >= BUS_TX_RETRIES) {
						RCP_LOG_DBG(
							"Discarding message on stream=%x after %d reties",
							event->stream, event->num_retries);
						pop_next();
					}
					/* On transient failure: don't pop, fall through to RX. */
				}
			}
		} else {
			(void)rcp_events_wait_for_any(RCP_EVENT_TX_RDY | RCP_EVENT_RX_DATA, 10);
		}
	}

	events = rcp_events_get();
	if (events & RCP_EVENT_RX_DATA) {
		enum rcp_bus bus = get_stream_status(1)->bus;
		if (get_rx_data(&rx_msg, bus)) {
			lnk_sts_t *lnk_sts_ptr = get_stream_status(rx_msg.stream);
			if (lnk_sts_ptr) {
				dispatch_msg(&rx_msg, lnk_sts_ptr);
			} else {
				RCP_LOG_WRN("Discarding message on invalid stream=%x",
						rx_msg.stream);
				release_buffer(&rx_msg.payload_ptr);
			}
		}
	}
}

#ifdef CONFIG_INCLUDE_TRANSPORT_TESTS

link_result_t link_send_seq_error(uint8_t stream, bool tsn_error, bool rsn_error,
				  rcp_buffer_hdl_t *tx_buffer)
{
	lnk_sts_t *lnk_sts_ptr = get_stream_status(stream);
	if (tsn_error) {
		lnk_sts_ptr->seq_nums.tsn = inc_seq_num(lnk_sts_ptr->seq_nums.tsn);
	}
	// Create error where RSN Ack >=  RCP_SEQ_NUM_ACK_WINDOW
	if (rsn_error) {
		uint8_t rsn;
		// Create error where RSN Ack >  RCP_SEQ_NUM_ACK_WINDOW
		if (lnk_sts_ptr->seq_nums.rsn - RCP_SEQ_NUM_ACK_WINDOW >= 0) {
			rsn = lnk_sts_ptr->seq_nums.rsn - RCP_SEQ_NUM_ACK_WINDOW;
		} else {
			// Caculate wrapped number
			rsn = RCP_SEQ_NUM_MAX +
			      (lnk_sts_ptr->seq_nums.rsn - RCP_SEQ_NUM_ACK_WINDOW) + 1;
		}
		RCP_LOG_INF("Chaning RSN %d->%d", lnk_sts_ptr->seq_nums.rsn, rsn);
		lnk_sts_ptr->seq_nums.rsn = rsn;
	}
	return link_send(stream, tx_buffer);
}

#endif // RCONFIG_INCLUDE_TRANSPORT_TESTS
