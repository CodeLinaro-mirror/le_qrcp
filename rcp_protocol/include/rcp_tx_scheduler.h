/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file rcp_tx_scheduler.h
 *
 * @defgroup rcp_tx_scheduler RCP TX scheduler
 * @ingroup rcp_internal
 *
 * TX scheduler maintains a FIFO queue of outgoing TX message buffers.
 * Messages are put into the queue when calling RCP API functions (link_send,
 * link_request) and will be retrieved by the multiplexer loop for sending
 * over the bus.
 * @{
 */
#ifndef __RCP_TX_SCHEDULER_H__
#define __RCP_TX_SCHEDULER_H__

#include "buffer_mgmt.h"

/** TX event handle */
typedef struct rcp_schedule_evt_t evt_handle_t;

/** TX event structure */
typedef struct rcp_schedule_evt_t {
	struct rcp_schedule_evt_t *prev;
	struct rcp_schedule_evt_t *next;
	uint8_t stream;
	uint8_t num_retries;
	bool reset_ack_window; // Sending of this message resets ack window
	bool tsn_inc;          // Indicates TSN need incrementing after send
	rcp_buffer_hdl_t *msg_buffer;
} rcp_schedule_evt_t;

/** Initialize TX scheduler queue. */
void init_tx_queue(void);

/**
 * Put a buffer at the end of the TX queue.
 *
 * @param msg_buffer Buffer to be scheduled
 * @param stream Stream ID associated with the buffer
 * @param inc_tsn true if TSN shall be incremented after sending
 * @param ack_msg true if sending this message resets the ACK window
 *
 * @return True if buffer was scheduled successfully. False in case of failure
 * (e.g. queue full).
 */
bool schedule_tx(rcp_buffer_hdl_t *msg_buffer, uint8_t stream, bool inc_tsn, bool ack_msg);

/**
* Check if TX queue is empty.
*
* @return true if empty, otherwise false.
*/
bool is_tx_empty(void);

/**
 * Get TX event that should be sent next.
 *
 * @return pointer to next TX event or NULL if queue is empty.
 */
rcp_schedule_evt_t *get_tx_event(void);

// Look at tx_event data but do not pop from queue
rcp_schedule_evt_t *view_next_tx_event(void);

// Pop event from queue and free it
void pop_next(void);

/**
 * Release a TX event.
 *
 * Releases the event and returns it back to free events queue.
 *
 * @param event Pointer event to be released.
 *
 * @return true if event was released successfully.
 */
bool free_event(rcp_schedule_evt_t *event);

/** @} end of rcp_tx_scheduler */

#endif
