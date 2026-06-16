/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file rcp_cntrl_msgs.h
 *
 * @defgroup rcp_cntrl_msgs RCP control messages
 *
 * RCP control messages provide a side channel for RCP protocol control, testing
 * and sending other control messages to the peer.
 *
 * @ingroup rcp_public
 * @{
 */
#ifndef __RCP_CNTRL_MSGS_H__
#define __RCP_CNTRL_MSGS_H__

#include "buffer_mgmt.h"
#include "rcp_interface.h"

/** @name Capabilities definitions */
/** @{ */

/** Flag bit posistions */
#define RCP_MCU_READY_FLAG         0x01
#define RCP_WIFI_CAPABILITY_FLAG   0x02
#define RCP_BT_CAPABILITY_FLAG     0x04
#define RCP_THREAD_CAPABILITY_FLAG 0x08

/** CAP_IND payload positions */
#define RCP_MCU_CAP_FLAGS_POS         0x00
#define RCP_MCU_CAP_VSERIAL_COUNT_POS 0x01
#define RCP_MCU_CAP_PAYLOAD_SIZE      0x02

/** @} */

/** @name Loopback definitions */
/** @{ */

/** Payload Byte posistions */
#define RCP_MCU_LOOP_SIZE 0x02

#define RCP_MCU_LOOP_ACTION 0x00
#define RCP_MCU_LOOP_STREAM 0x01

/** LOOP STATE Values */
#define RCP_MCU_LOOP_ACTIVE 0x01 /**< Loopback stream/Indicates stream looped back */
#define RCP_MCU_LOOP_INACTIVE                                                                      \
	0x00 /**< Stop stream loop back/Indicate stream now normal operation */

/** @} */

/** Definition of control message handler */
typedef void (*cntrl_msg_handler_t)(uint8_t msg_type, uint8_t subtype,
				    rcp_buffer_hdl_t *payload_ptr);

/** Callback to be called after encryption key exchange is finished */
typedef void (*key_exchange_done_cb)(bool success);

/** Initialize control link. */
void init_cntrl_status();

/** Handle messages received over control stream.
 *
 * Function shall only be used by message dispatcher upon reception of a message
 * over control stream.
 *
 * @param cntrl_msg Pointer to the control message
 */
void handle_cntrl_msgs(rcp_msg_ptr_t cntrl_msg);

/** Register a handler for received control messages.
 *
 * @param handler Handler function.
 */
void register_cntrl_msg_hdlr(cntrl_msg_handler_t handler);

/** Set capabilities available on the MCU.
 *
 * This function shall only be used by RCP MCU to initialize indicated capabilities.
 * The control message module will return capabilities set using this function
 * after host issues a capability request.
 *
 * @param capbilities Capabilities mask.
 */
void init_mcu_capability(uint8_t capbilities);

/** Set number of MCU virtual serial ports (0..4).
 *
 * CAP_IND payload format:
 * Byte 0: capability flags (RCP_MCU_*_CAPABILITY_FLAG).
 * Byte 1: number of virtual serial ports supported by MCU (0..4).
 *
 * @param count Virtual serial port count.
 */
void init_mcu_vserial_port_count(uint8_t count);

/** Send capabilities request over control channel.
 *
 * Capability request is normally send by host to MCU to read it's network
 * capabilities. The MCU shall initialize capabilities first with init_mcu_capability.
 *
 * @return true on success, otherwise false
 */
bool send_capability_request();

/**  Request a loopback on a specific stream.
 *
 * A loopback allows to test bidirectional communication over a specified stream.
 * Can be for example used for bandwidth testing. After requesting a loopback
 * the control module will use a special handler that will echo any incoming
 * messages on a stream.
 *
 * @param stream Stream ID
 * @return true on success, otherwise false
 */
bool send_loopback_request(uint8_t stream);

/** Cancel last loopback request.
 *
 * @return true on success, otherwise false
 */
bool send_loopback_cancel();

/** Send encryption key exchange request.
 *
 * The key exchange request will pull current public key from RCP encryption module
 * and send it to the peer. The peer will respond with it's own public key which
 * will be passed to encryption module to derive a shared secret.
 *
 * A function callback will be called after key exchange is done (sender receives a response)
 * with status of the operation.
 *
 * @param cb Key exchange done callback function
 *
 * @return true if request was successful, false otherwise
 */
bool send_key_exchange_request(key_exchange_done_cb cb);

/** @} end of rcp_cntrl_msgs */

/**
 * Test Message indicate RCP test/indication
 */
#define TEST_NUM_POS 0 /**< Byte posistion of TEST NUM */

/**
 * Test Number Value
 */
#define TEST_FUNC_BASIC_HOST 0x55 /**< Basic Test Request*/
#define TEST_FUNC_BASIC_RESP 0xAA /**< Valid Response from MCU */
#define TEST_NUM_NACK_THIS   0x00 /**< Host expects Nack in response */
#define TEST_FUNC_RESET      0x01 /**< Host reset link test */
#define TEST_FUNC_RESET_ACK  0x02 /**< MCU reset link acknowledge */

#endif // __RCP_CNTRL_MSGS_H__
