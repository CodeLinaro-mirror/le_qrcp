/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file rcp_bus.h
 *
 * @defgroup rcp_bus RCP bus layer
 *
 * RCP bus layer provides common abstraction over different bus implementations
 * (SDIO, SPI, UART etc.) for transferring RCP messages between two peers.
 * Each available bus shall implement the RCP bus API, which then can be associated
 * with appropriate data streams at RCP level.
 * @ingroup rcp_internal
 * @{
 */
#ifndef __RCP_BUS_H__
#define __RCP_BUS_H__

#include <stdint.h>

/** Bus transaction status code. */
enum rcp_bus_status {
	RCP_BUS_OK,              /**< Transaction complete. */
	RCP_BUS_ERR_IN_PROGRESS, /**< Transaction ongoing. */
	RCP_BUS_ERR_NO_INST,     /**< Instance not registered. */
	RCP_BUS_ERR_IO,          /**< Hardware error. */
	RCP_BUS_ERR_INVALID,     /**< Invalid usage, e.g. NULL pointer. */
	RCP_BUS_ERR_EMPTY,       /**< No data. */
	RCP_BUS_ERR_CLOSED,      /**< Channel closed. */
	RCP_BUS_ERR_MEMORY,      /**< No buffer space left. */
	RCP_BUS_ERR_PEER_MEMORY, /**< Peer could not accept data. */
	RCP_BUS_ERR_TIMEOUT,     /**< Operation timeout. */
	RCP_BUS_ERR_UNKNOWN,     /**< Enum delimeter, do not use. */
};

/** Bus instance. */
enum rcp_bus {
	RCP_BUS_SPI = 0,
	RCP_BUS_SDIO,
	RCP_BUS_UART,
	RCP_BUS_MAX, /* Maximum number of available buses. */
	RCP_BUS_INACTIVE = 0xFF, /* Indicates bus inactive */
};

/* Bus instance-specific capabilities. */
struct rcp_bus_caps {
	/** Bus-specific MTU. */
	uint16_t mtu;
};

/** Bus instance API. */
struct rcp_bus_api {
	enum rcp_bus_status (*open)(void);
	enum rcp_bus_status (*close)(void);
	enum rcp_bus_status (*write)(uint8_t *tx, uint16_t len);
	enum rcp_bus_status (*read)(uint8_t *rx, uint16_t len, uint16_t *nread);
	uint16_t (*get_rx_len)(void);
	enum rcp_bus_status (*get_caps)(struct rcp_bus_caps *caps);
};

/**
 * Register an API instance for the specific bus.
 * Each bus can be registered only once.
 *
 * @param bus Bus type.
 * @param api Pointer to API implementation.
 */
enum rcp_bus_status rcp_bus_register(enum rcp_bus bus, struct rcp_bus_api *api);

/**
 * Open bus instance.
 * @param bus Bus to open.
 */
enum rcp_bus_status rcp_bus_open(enum rcp_bus bus);

/**
 * Close bus instance.
 * @param bus Bus to close.
 */
enum rcp_bus_status rcp_bus_close(enum rcp_bus bus);

/**
 * Write using bus instance - blocking.
 *
 * @param bus Bus to use.
 * @param tx Tx data buffer.
 * @param len Amount of tx data.
 *
 * @return RCP_BUS_OK or error code.
 */
enum rcp_bus_status rcp_bus_write(enum rcp_bus bus, uint8_t *tx, uint16_t len);

/**
 * Read using bus instance - blocking.
 *
 * @param bus Bus to use.
 * @param rx Data buffer to store rx data.
 * @param len Size of rx buffer.
 * @param nread Actual amount of data read.
 *
 * @return RCP_BUS_OK or error code.
 */
enum rcp_bus_status rcp_bus_read(enum rcp_bus bus, uint8_t *rx, uint16_t len, uint16_t *nread);

/**
 * Get capabilities of the underlying bus.
 *
 * @param bus Bus to use.
 * @param caps Returned capabilities structure.
 *
 * @return RCP_BUS_OK or error code.
 */
enum rcp_bus_status rcp_bus_get_caps(enum rcp_bus bus, struct rcp_bus_caps *caps);

/**
 * Get amount of RX data waiting to be read.
 *
 * @param bus Bus intance to get RX data len.
 *
 * @return Amount of bytes available for reading.
 */
uint16_t rcp_bus_get_rx_len(enum rcp_bus bus);

/** Utility function to convert status code to human-readable string.
 *
 * @param status bus status code
 *
 * @return status code converted to a string
 */
const char *rcp_bus_status_to_str(enum rcp_bus_status status);

/** @} end of rcp_bus */

#endif
