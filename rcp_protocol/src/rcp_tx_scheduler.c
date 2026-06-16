/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "rcp_tx_scheduler.h"

#include "rcp_definitions.h"
#include "rcp_log.h"
#include "rcp_event.h"
#include "rcp_os.h"

#include <stddef.h>

#define MAX_TX_EVENTS RCP_MAX_TX_EVENTS
#define TX_SCHED_WARN_INTERVAL_MS 1000U

static rcp_schedule_evt_t tx_events[MAX_TX_EVENTS];
static rcp_schedule_evt_t tx_queue;
static rcp_schedule_evt_t free_queue;
static rcp_mutex_t tx_queue_mutex;

static void log_tx_scheduler_exhausted(void)
{
	static uint32_t last_log_ms;
	static uint32_t suppressed_logs;
	uint32_t now = rcp_os_time_ms();

	if ((last_log_ms == 0U) || ((now - last_log_ms) >= TX_SCHED_WARN_INTERVAL_MS)) {
		if (suppressed_logs > 0U) {
			RCP_LOG_WRN("TX event queue remained full; suppressed %lu duplicate warning(s)",
				(unsigned long)suppressed_logs);
			suppressed_logs = 0U;
		}

		RCP_LOG_ERR("No avaiable TX event slots, Used=%d", MAX_TX_EVENTS);
		last_log_ms = now;
		return;
	}

	suppressed_logs++;
}

static void remove_event(rcp_schedule_evt_t *handle)
{
	handle->next->prev = handle->prev;
	handle->prev->next = handle->next;
	handle->prev = handle;
	handle->next = handle;
}

static rcp_schedule_evt_t *get_event(rcp_schedule_evt_t *queue)
{
	if (queue->next == queue) {
		return NULL;
	}
	rcp_schedule_evt_t *top_event = queue->next;
	remove_event(top_event);
	return top_event;
}

static void add_event(rcp_schedule_evt_t *queue, rcp_schedule_evt_t *event)
{
	if (event == NULL) {
		return;
	}
	rcp_schedule_evt_t *temp_prev = queue->prev;
	event->next = queue;
	queue->prev = event;
	event->prev = temp_prev;
	event->prev->next = event;
}

void init_tx_queue()
{
	(void)rcp_os_mutex_init(&tx_queue_mutex);
	free_queue.next = &free_queue;
	free_queue.prev = &free_queue;
	for (int i = 0; i < MAX_TX_EVENTS; i++) {
		add_event(&free_queue, &tx_events[i]);
	}
	tx_queue.next = &tx_queue;
	tx_queue.prev = &tx_queue;
}

bool is_tx_empty()
{
	bool empty;
	rcp_os_mutex_lock(&tx_queue_mutex);
	empty = (tx_queue.next == &tx_queue);
	rcp_os_mutex_unlock(&tx_queue_mutex);
	return empty;
}

bool schedule_tx(rcp_buffer_hdl_t *tx_buffer, uint8_t stream, bool inc_tsn, bool ack_msg)
{
	rcp_os_mutex_lock(&tx_queue_mutex);
	rcp_schedule_evt_t *event = get_event(&free_queue);
	if (!event) {
		rcp_os_mutex_unlock(&tx_queue_mutex);
		log_tx_scheduler_exhausted();
		return false;
	}
	event->tsn_inc = inc_tsn;
	event->reset_ack_window = ack_msg;
	event->msg_buffer = tx_buffer;
	event->stream = stream;
	event->num_retries = 0;
	add_event(&tx_queue, event);
	rcp_os_mutex_unlock(&tx_queue_mutex);
	rcp_events_set(RCP_EVENT_TX_DATA);
	return true;
}

bool free_event(rcp_schedule_evt_t *event)
{
	remove_event(event);
	add_event(&free_queue, event);
	return true;
}

rcp_schedule_evt_t *get_tx_event()
{
	rcp_os_mutex_lock(&tx_queue_mutex);
	if (tx_queue.next == &tx_queue) {
		rcp_os_mutex_unlock(&tx_queue_mutex);
		return NULL;
	}
	rcp_schedule_evt_t *top_event = tx_queue.next;
	remove_event(top_event);
	bool empty = (tx_queue.next == &tx_queue);
	rcp_os_mutex_unlock(&tx_queue_mutex);
	if (empty) {
		rcp_events_clear(RCP_EVENT_TX_DATA);
	}
	return top_event;
}

// Look at tx_event data but do not pop from queue
rcp_schedule_evt_t *view_next_tx_event()
{
	rcp_os_mutex_lock(&tx_queue_mutex);
	if (tx_queue.next == &tx_queue) {
		rcp_os_mutex_unlock(&tx_queue_mutex);
		return NULL;
	}
	rcp_schedule_evt_t *evt = tx_queue.next;
	rcp_os_mutex_unlock(&tx_queue_mutex);
	return evt;
}

// Pop event from queue and free it
void pop_next()
{
	rcp_os_mutex_lock(&tx_queue_mutex);
	if (tx_queue.next != &tx_queue) {
		release_buffer(&tx_queue.next->msg_buffer);
		free_event(tx_queue.next);
		if (tx_queue.next == &tx_queue) {
			rcp_events_clear(RCP_EVENT_TX_DATA);
		}
	}
	rcp_os_mutex_unlock(&tx_queue_mutex);
}
