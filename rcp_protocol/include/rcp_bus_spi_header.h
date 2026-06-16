/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file rcp_bus_spi_header.h
 *
 * @brief SPI framing utilities.
 *
 * Master and slave communicating over SPI use a small header to exchange
 * read and write lengths. "write length" indicates amount of data being send
 * from one peer to another. "accept length" is used to indicate the amount of
 * space that the peer has available for receiving data. This value is also used
 * to confirm successful TX, e.g.:
 * master accept length >= slave write length -> master could accept, TX successful.
 *
 * @{
 * @ingroup rcp_bus
 */
#ifndef __RCP_BUS_SPI_HEADER_H__
#define __RCP_BUS_SPI_HEADER_H__

#include <stdbool.h>
#include <stdint.h>

#define SPI_HEADER_SIZE 5

/**
 * @brief Initialize header values
 * @param header - pointer to the header buffer
 */
void rcp_bus_spi_header_init(uint8_t *header);

/**
 * @brief Set TX data length field in the header
 * @param header - pointer to the header buffer
 * @param len - TX data length
 */
void rcp_bus_spi_header_set_data_length(uint8_t *header, uint16_t len);

/**
 * @brief Set RX accept length field in the header
 * @param header - pointer to the header buffer
 * @param len - RX accept length
 */
void rcp_bus_spi_header_set_accept_length(uint8_t *header, uint16_t len);

/**
 * @brief Get TX data length field from the header
 * @param header - pointer to the header buffer
 * @return TX data length value
 */
uint16_t rcp_bus_spi_header_get_data_length(uint8_t *header);

/**
 * @brief Get RX accept length field from the header
 * @param header - pointer to the header buffer
 * @return RX accept length value
 */
uint16_t rcp_bus_spi_header_get_accept_length(uint8_t *header);

/**
 * @brief Check if header is valid (checks fixed header marker)
 * @param header - pointer to the header buffer
 * @return true if valid, false otherwise
 */
bool rcp_bus_spi_header_is_valid(uint8_t *header);

/** @} */
#endif /* __RCP_BUS_SPI_HEADER_H__ */
