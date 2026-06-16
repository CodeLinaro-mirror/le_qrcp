/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */


/** rcp_interface_impl.h
 *
 * @{
 * @ingroup rcp_interface
 */

#ifndef __RCP_INTERFACE_IMPL_H__
#define __RCP_INTERFACE_IMPL_H__

#include "rcp_interface.h"
#include "rcp_mux_dispatcher.h"
#include "rcp_interface_impl.h"
#include "rcp_bus.h"

/**
 * Run RCP interface TX / RX loop.
 *
 * Event loop processes the RCP TX queue and sends messages over appropriate
 * bus as well and passes RX traffic from bus to appropriate stream handlers.
 */
void run_event_loop(void);

/**
 * Single-iteration event loop step.
 * Same processing as run_event_loop() but returns after one pass.
 * Use from a kthread that checks kthread_should_stop() between calls.
 */
void run_event_loop_once(void);

#ifdef CONFIG_INCLUDE_TRANSPORT_TESTS
link_result_t link_send_seq_error(uint8_t stream, bool tsn_error, bool rsn_error,
				  rcp_buffer_hdl_t *tx_buffer);
#endif // CONFIG_INCLUDE_TRANSPORT_TESTS

/** @} */

#endif
