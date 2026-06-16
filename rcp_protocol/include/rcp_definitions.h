/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file rcp_definitions.h
 * RCP protocol definitions.
 *
 * @defgroup rcp_definitions RCP protocol definitions
 *
 * Common predefined values used in RCP protocol
 *
 * @{
 * @ingroup rcp_internal
 */
#ifndef _RCP_DEFINITIONS_H_
#define _RCP_DEFINITIONS_H_

#include <stdint.h>
#include <rcp_config.h>

#ifdef RCP_OS_ZEPHYR
#include <zephyr/sys/util.h>
#else
#define CONCAT(a,b) (a##b)
#endif

/** Data sizes */
#define RCP_HDR_SIZE            4
#define RCP_PAYLOAD_LENGTH_SIZE 2
#define RCP_ACK_MSG_SIZE        1
#define RCP_NACK_MSG_SIZE       3

/** Max Sequence Number value/roll over */
#define RCP_SEQ_NUM_MAX 0x07

/** Byte positions from LSB */
#define RPC_POS_MSG_INFO     0x00
#define RCP_POS_SEQ_NUMS     0x01
#define RCP_PAD_BYTE         0x01
#define RPC_POS_PAYLOAD_SIZE 0x02
#define RCP_PAYLOAD_POS      0x04

/** Common Header Mask */
#define RCP_MASK_STREAM   0XF0
#define RCP_MASK_MSG_TYPE 0x0F
#define RCP_MASK_FORMAT   0X0C
#define RCP_MASK_MSG_ID   0x03

/** Bit shift */
#define RCP_BIT_SHFT_STREAM 0x04
#define RCP_BIT_SHFT_FORMAT 0x02
#define RCP_BIT_SHFT_MSG_ID 0x00

/** Common Header Mask */
#define RCP_MASK_TSN 0XE0
#define RCP_MASK_RSN 0X1C
#define RCP_MASK_ACK 0x02

/** Bit shifts */
#define RCP_BIT_SHFT_TSN 0x05
#define RCP_BIT_SHFT_RSN 0x02
#define RCP_BIT_SHFT_ACK 0x01

/** @name Stream Types */
/** @{ */
#define RCP_STREAM_LNK_MGMT			0x00
#define RCP_STREAM_WIFI_MGMT		0x01
#define RCP_STREAM_WIFI_DATA		0x02
#define RCP_STREAM_BT				0x03
#ifndef RCP_STREAM_THREAD_USE_VSERIAL
#define RCP_STREAM_THREAD			0x04
#else /* RCP_STREAM_THREAD_USE_VSERIAL */
/* In case Thread uses VSerial, get appropriate vserial stream for the configured
 * port. For example, if RCP_STREAM_THREAD_VSERIAL_PORT == 1,
 * RCP_STREAM_THREAD will be the same as RCP_STREAM_VSERIAL1.
 */
#define GET_VSERIAL_STREAM(n) CONCAT(RCP_STREAM_VSERIAL, n)
#define RCP_STREAM_THREAD GET_VSERIAL_STREAM(RCP_STREAM_THREAD_VSERIAL_PORT)
#endif
#define RCP_STREAM_AT_COMMANDS		0x05
#define RCP_STREAM_TEST_ONLY		0x06 /**< Used for RCP Testing/Debug */
#define RCP_STREAM_VSERIAL0		0x07
#define RCP_STREAM_VSERIAL1		0x08
#define RCP_STREAM_VSERIAL2		0x09
#define RCP_STREAM_VSERIAL3		0x0A
#define RCP_STREAM_DELIMITER		0x0B /**< Valid streams: 0x00..0x0A */
#define RCP_STREAM_NOT_DEFINED		0xFF
/** @} */

/** Msg Formats */
#define RPC_MSG_FORMAT_DATA     0x00
#define RPC_MSG_FORMAT_CONTROL  0x01
#define RPC_MSG_FORMAT_DATA_ENC 0x02 /**< Encrypted data messages */

/** Msg Ids */
/** Data Messages */
#define RPC_MSG_ID_INIT_DATA 	 0x00
#define RPC_MSG_ID_DATA_XFR  	 0x01
#define RPC_MSG_ID_ACK       	 0x02
#define RPC_MSG_ID_NACK      	 0x03

/** Control */
#define RPC_MSG_ID_CONTROL    0x00
#define RPC_MSG_ID_STATUS_IND 0x01
#define RPC_MSG_ID_DEBUG_REQ  0x02
#define RPC_MSG_ID_DEBUG_RESP 0x03

#define RPC_TYPE(FMT, MSG_ID)                                                                      \
	(((FMT) << RCP_BIT_SHFT_FORMAT) | (((MSG_ID) << (RCP_BIT_SHFT_MSG_ID)) & RCP_MASK_MSG_TYPE))

/** Message Type - combines format with Id */

/** Data */
#define RCP_MSG_TYPE_INIT_DATA RPC_TYPE(RPC_MSG_FORMAT_DATA, RPC_MSG_ID_INIT_DATA)
#define RCP_MSG_TYPE_DATA_XFER RPC_TYPE(RPC_MSG_FORMAT_DATA, RPC_MSG_ID_DATA_XFR)
#define RCP_MSG_TYPE_ACK       RPC_TYPE(RPC_MSG_FORMAT_DATA, RPC_MSG_ID_ACK)
#define RCP_MSG_TYPE_NACK      RPC_TYPE(RPC_MSG_FORMAT_DATA, RPC_MSG_ID_NACK)

/* Encrypted data */
#define RCP_MSG_TYPE_INIT_DATA_ENC RPC_TYPE(RPC_MSG_FORMAT_DATA_ENC, RPC_MSG_ID_INIT_DATA)
#define RCP_MSG_TYPE_DATA_XFER_ENC RPC_TYPE(RPC_MSG_FORMAT_DATA_ENC, RPC_MSG_ID_DATA_XFR)

/** Control */
#define RCP_MSG_TYPE_CONTROL    RPC_TYPE(RPC_MSG_FORMAT_CONTROL, RPC_MSG_ID_CONTROL)
#define RCP_MSG_TYPE_STATUS_IND RPC_TYPE(RPC_MSG_FORMAT_CONTROL, RPC_MSG_ID_STATUS_IND)

/** Control Subtypes */
#define RCP_CNTRL_CAP_REQ       0x00 /**< Request MCU Capabilities */
#define RCP_CNTRL_LOOPBACK_REQ  0x01 /**< Request RCP stream loopback */
#define RCP_CNTRL_ENC_KEY_REQ   0x02 /**< Perform encryption key exchange request. */

/** Status Indication Subtypes */
#define RCP_CNTRL_CAP_IND       0x00 /**< Indicates device capabilities */
#define RCP_CNTRL_LOOPBACK_IND  0x01 /**< Indicates stream loopback state change */
#define RCP_CNTRL_ENC_KEY_RESP  0x02 /**< Encryption key exchange response. */

/** Debug */
#define RCP_MSG_TYPE_DEBUG_REQ  RPC_TYPE(RPC_MSG_FORMAT_CONTROL, RPC_MSG_ID_DEBUG_REQ)
#define RCP_MSG_TYPE_DEBUG_RESP RPC_TYPE(RPC_MSG_FORMAT_CONTROL, RPC_MSG_ID_DEBUG_RESP)

/** @} */

#endif
