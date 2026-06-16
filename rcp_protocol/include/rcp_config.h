/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file rcp_config.h
 *
 * @defgroup rcp_config RCP configuration
 *
 * This module provides all possible, configurable parameters for all RCP protocol
 * code modules. Applications using RCP protocol MUST provide a file called
 * rcp_user_config.h that will at very least define the target OS (RCP_OS_ZEPHYR,
 * RCP_OS_LINUX or RCP_OS_FREERTOS). The user configuration can then be used
 * to override default values specified by this module.
 *
 * @ingroup rcp_public
 * @{
 */
#ifndef __RCP_CONFIG_H__
#define __RCP_CONFIG_H__

#include "rcp_user_config.h"

#ifndef RCP_LOG
/**
 * RCP logging
 *
 * - 1 - ON
 * - 0 - OFF
 */
#define RCP_LOG 1
#endif

#ifndef RCP_LOG_LEVEL
/**
 * RCP log level
 *
 * - 0 - None
 * - 1 - ERR
 * - 2 - WRN
 * - 3 - INF
 * - 4 - DBG
 */
#define RCP_LOG_LEVEL 3
#endif

#ifndef RCP_LNK_MGMT_BUS
/** Bus used by the RCP control stream. */
#define RCP_LNK_MGMT_BUS RCP_DEFAULT_BUS
#endif

#ifndef RCP_BT_BUS
/** Bus used by the Bluetooth stream. */
#define RCP_BT_BUS RCP_DEFAULT_BUS
#endif

#ifndef RCP_WIFI_BUS
/** Bus used by the WiFi stream. */
#define RCP_WIFI_BUS RCP_DEFAULT_BUS
#endif

#ifndef RCP_TEST_BUS
/** Bus used by the test stream. */
#define RCP_TEST_BUS RCP_DEFAULT_BUS
#endif

#ifndef RCP_SEQ_NUM_ACK_WINDOW
/**
 * Max Number of transmiited messages allowed to have pending ack.
 * Must be less that RCP_SEQ_NUM_MAX.
 */
#define RCP_SEQ_NUM_ACK_WINDOW 5
#endif

#ifndef RCP_NUM_BUFFERS
/** Number of RCP message buffers available for use. */
#define RCP_NUM_BUFFERS 10
#endif

#ifndef RCP_BUF_SIZE
/** Buffer size of single RCP message buffer in bytes. */
#define RCP_BUF_SIZE 1600
#endif

#ifndef RCP_MAX_TX_EVENTS
/** Maximum number of outgoing TX messages in the queue. */
#define RCP_MAX_TX_EVENTS 10
#endif

#ifndef RCP_WIFI_MAX_CB_CNT
/** Maximum WiFi event callback functions. */
#define RCP_WIFI_MAX_CB_CNT 2
#endif

#ifndef RCP_NUM_EXT_BUFFERS
/** Buffer number of external buffer wrappers supported. */
#define RCP_NUM_EXT_BUFFERS 5
#endif

#ifndef RCP_SDIO_FREQUENCY_HZ
/** SDIO bus frequency (in Hz) - configured by the host, no effect on RCP device. */
#define RCP_SDIO_FREQUENCY_HZ 25000000
#endif

#ifndef RCP_SDIO_BUS_WIDE
/** SDIO wide (4-bit) bus mode - configured by the host, no effect on RCP device. */
#define RCP_SDIO_BUS_WIDE 1
#endif

#ifndef RCP_ENCRYPTION
/** RCP message encryption support. Off by default. */
#define RCP_ENCRYPTION 0
#endif

#ifndef RCP_ENCRYPT_BT
/** Encrypt Bluetooth HCI communication. */
#define RCP_ENCRYPT_BT 0
#endif

#ifndef RCP_ENCRYPT_WIFI_DATA
/** Encrypt WiFi data communication. */
#define RCP_ENCRYPT_WIFI_DATA 0
#endif

#ifndef RCP_ENCRYPT_WIFI_MGMT
/** Encrypt WiFi management communication. */
#define RCP_ENCRYPT_WIFI_MGMT 0
#endif

#ifndef RCP_ENCRYPT_THREAD
/** Encrypt Thread Spinel communication. */
#define RCP_ENCRYPT_THREAD 0
#endif

#if !RCP_ENCRYPTION

#define RCP_ENCRYPT_BT 0
#define RCP_ENCRYPT_WIFI_MGMT 0
#define RCP_ENCRYPT_WIFI_DATA 0
#define RCP_ENCRYPT_THREAD 0

#endif

#ifndef RCP_STREAM_THREAD_USE_VSERIAL
/** Use virtual serial for Thread Spinel stream (default).
 *
 * By default one of the VSerial streams is used instead of a dedicated stream
 * since the MCU side Zephyr Thread integration expects an UART device for
 * Spinel transmission. Keep this set unless you want to implement custom
 * handling on the MCU side using standard (non-vserial) stream.
 *
 * Host drivers can still use RCP_STREAM_THREAD as this will be mapped automatically
 * to the selected VSerial port.
 */
#define RCP_STREAM_THREAD_USE_VSERIAL 1
#endif

#ifndef RCP_STREAM_THREAD_VSERIAL_PORT
/** VSerial port to be used by Thread Spinel.
 *
 * This determines the number of the VSerial port used for Thread
 * In case RCP_STREAM_THREAD_USE_VSERIAL is selected. Otherwise this setting
 * is ignored.
 */
#define RCP_STREAM_THREAD_VSERIAL_PORT 1
#endif

#if !defined(RCP_OS_ZEPHYR) && !defined(RCP_OS_FREERTOS) && !defined(RCP_OS_LINUX)
#error "RCP OS not defined - define RCP_OS_ZEPHYR or RCP_OS_FREERTOS or RCP_OS_LINUX"
#endif

#if (defined(RCP_OS_ZEPHYR) && defined(RCP_OS_FREERTOS)) ||                                        \
	(defined(RCP_OS_ZEPHYR) && defined(RCP_OS_LINUX)) ||                                       \
	(defined(RCP_OS_FREERTOS) && defined(RCP_OS_LINUX))
#error "Only one OS can be defined - RCP_OS_ZEPHYR, RCP_OS_FREERTOS or RCP_OS_LINUX"
#endif

/** @} end of rcp_config */

#endif
