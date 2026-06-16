/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file rcp_mux_dispatcher.h
 * RCP mux dispatcher API.
 *
 * @defgroup rcp_mux_dispatcher RCP multiplexer dispatcher
 *
 * RCP multiplexer dispatcher module is responsible to route messages received
 * from the bus to appropriate stream handlers provided by the application.
 * The dispatcher will also handle automatic responses (ACKs, NACKs) and manage
 * The state of the link (sequence number, error status etc.).
 *
 * @{
 * @ingroup rcp_internal
 */
#ifndef __RCP_MUX_DISPATCHER_H__
#define __RCP_MUX_DISPATCHER_H__

#include "buffer_mgmt.h"
#include "rcp_interface.h"

/** Initialize dispatcher state. */
void init_dispatcher(void);

/** @{ @ingroup rcp_interface */
/**
 * Register message handler function for a stream.
 *
 * Handler function will be called upon reception of a message (payload, ACK or
 * NACK) for the specified stream.
 *
 * @param stream stream ID
 * @param handler Handler function
 *
 * @return true if handler was registered successfully, otherwise false.
*/
bool register_data_msg_handler(uint8_t stream, rcp_rx_event_handler_t handler);

/** @} */

/** Decode a message and dispatch to the appropriate stream handler.
 *
 * This function should be called from the multiplexer loop with a decoded
 * message to be routed by the dispatcher. This function is not intended to be used
 * by user application.
 *
 * @param message Handle of message to dispatch
 * @param lnk_sts_ptr Current stream context
 */
void dispatch_msg(rcp_msg_ptr_t message, lnk_sts_t *lnk_sts_ptr);

/** Get currently registered handler for a stream.
 * @param stream stream ID
 *
 * @return registered handler function pointer or NULL.
*/
rcp_rx_event_handler_t get_rx_handler(uint8_t stream);

/** @} end of rcp_mux_dispatcher */

#endif
