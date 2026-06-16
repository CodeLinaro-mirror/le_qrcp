/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file rcp_bus_spi_header.c
 *
 */
#include <rcp_bus_spi_header.h>

/** Header pattern byte to check validity of the header. Arbitrarily chosen. */
#define SPI_HEADER_PATTERN_BYTE 0xBA

void rcp_bus_spi_header_init(uint8_t *header)
{
	header[0] = SPI_HEADER_PATTERN_BYTE;
	for (int i = 1; i < SPI_HEADER_SIZE; i++) {
		header[i] = 0;
	}
}

void rcp_bus_spi_header_set_data_length(uint8_t *header, uint16_t len)
{
	header[3] = ((len >> 0) & 0xFF);
	header[4] = ((len >> 8) & 0xFF);
}

void rcp_bus_spi_header_set_accept_length(uint8_t *header, uint16_t len)
{
	header[1] = ((len >> 0) & 0xFF);
    	header[2] = ((len >> 8) & 0xFF);
}

uint16_t rcp_bus_spi_header_get_data_length(uint8_t *header)
{
	return (header[3] + (uint16_t)(header[4] << 8));
}

uint16_t rcp_bus_spi_header_get_accept_length(uint8_t *header)
{
	return (header[1] + (uint16_t)(header[2] << 8));
}

bool rcp_bus_spi_header_is_valid(uint8_t *header)
{
	if (header[0] != SPI_HEADER_PATTERN_BYTE) {
		return false;
	}
	return true;
}
