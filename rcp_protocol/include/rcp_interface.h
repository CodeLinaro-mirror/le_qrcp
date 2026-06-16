/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file rcp_interface.h
 *
 * @defgroup rcp_public RCP public API
 * @defgroup rcp_internal RCP internal API
 *
 * @defgroup rcp_interface RCP protocol interface
 *
 * RCP protocol public interface provides function to send data over RCP link
 * and register handlers for data received from peer. The data is sent over
 * virtual "streams" that allow to separate different technology streams (WiFi,
 * Thread, Bluetooth etc.) as well as RCP control channels. The buffers for
 * sending / receiving data are handled with buffer management API - @ref rcp_buffer_mgmt
 *
 * @ingroup rcp_public
 * @{
 *
 */
#ifndef __RCP_INTERFACE_H__
#define __RCP_INTERFACE_H__

#include "rcp_protocol.h"
#include <rcp_cntrl_msgs.h>
#include <rcp_error_codes.h>

#include <stdbool.h>
#include <stdint.h>

/**
 * @enum rcp_dsptch_result
 * Dispatch response results
 */
enum rcp_dsptch_result {
	RCP_RX_NO_ACTION, /**< Take no action */
	RCP_RX_RESPONSE,  /**< Send data message in response */
	RCP_RX_ACK,       /**< Send Ack - No data */
	RCP_RX_NACK,      /**< Send Nack with error data */
};

/**
 * Handler for RCP received events
 *
 * @param rx_status see @ref rcp_rx_status
 * @param payload_ptr Pointer to message payload buffer
 * @param err_info_ptr Pointer to error info or null if none
 * @return How RCP should respond to request (see @ref rcp_dsptch_result)
 */
typedef uint8_t (*rcp_rx_event_handler_t)(enum rcp_rx_status status, rcp_buffer_hdl_t *payload_ptr,
					  rcp_error_info_t *err_info_ptr);

/**
 * Initializes RCP link
 *
 * @return On success RCP_LNK_SUCCESS, otherwise error code
 */
link_result_t init_rcp(void);

/**
 * Check whether the RCP link core has completed its initialization.
 *
 * This becomes true after init_rcp() has created buffers, scheduler state and
 * stream state used by link_get_state()/link_send().
 */
bool link_is_initialized(void);

/**
 * Register handler for data message received over link stream
 *
 * @param stream Stream for RX messages
 * @param rx_message_cb Callback to handle received messages
 * @return On success RCP_LNK_SUCCESS, otherwise error code
 */
link_result_t link_register_rx_handler(uint8_t stream, rcp_rx_event_handler_t rx_message_cb);

/**
 * Send request over link.
 *
 * TX message will be formatted into RCP frame and put into TX queue.
 *
 * @param stream Stream request should be transmitted over.
 * See RCP_STREAM_* in rcp_definitions.h.
 * @param tx_buffer Pointer to buffer holding data to send
 * @return On success RCP_LNK_SUCCESS, otherwise error code
 */
link_result_t link_send(uint8_t stream, rcp_buffer_hdl_t *tx_buffer);

/**
 * Send request and register handler for response
 *
 * TX message will be formatted into RCP frame and put into TX queue.
 *
 * @param stream Stream request should be transmitted over
 * See RCP_STREAM_* in rcp_definitions.h..
 * @param tx_buffer Pointer to buffer holding data to send
 * @param response_cb Handler to deal with response
 * @return On success RCP_LNK_SUCCESS, otherwise error code
 */
link_result_t link_request(uint8_t stream, rcp_buffer_hdl_t *tx_buffer,
			   rcp_rx_event_handler_t response_cb);

/**
 * Reset the link
 *
 * @param stream Link to reset
 * See RCP_STREAM_* in rcp_definitions.h.
 * @param tx_buffer Optional buffer for reset data
 * @return RCP_LNK_SUCCESS if reset successful, otherwise link error
 */
link_result_t link_reset(uint8_t stream, rcp_buffer_hdl_t *tx_buffer);

/**
 * Get current link state
 */
link_state_t link_get_state(uint8_t stream);

/** @} end of rcp_interface */

#endif // __RCP_INTERFACE_H__
