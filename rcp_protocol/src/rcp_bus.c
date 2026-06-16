/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stddef.h>
#include <stdbool.h>
#include <rcp_bus.h>
#include <rcp_os.h>

static struct {
	const struct rcp_bus_api *instances[RCP_BUS_MAX];
} bus_ctx;

static const char *const status_string[] = {
	[RCP_BUS_OK] = "RCP_BUS_OK",
	[RCP_BUS_ERR_IN_PROGRESS] = "RCP_BUS_ERR_IN_PROGRESS",
	[RCP_BUS_ERR_NO_INST] = "RCP_BUS_ERR_NO_INST",
	[RCP_BUS_ERR_IO] = "RCP_BUS_ERR_IO",
	[RCP_BUS_ERR_INVALID] = "RCP_BUS_ERR_INVALID",
	[RCP_BUS_ERR_CLOSED] = "RCP_BUS_ERR_CLOSED",
	[RCP_BUS_ERR_MEMORY] = "RCP_BUS_ERR_MEMORY",
	[RCP_BUS_ERR_UNKNOWN] = "RCP_BUS_ERR_UNKNOWN",
};

static inline bool is_registered(enum rcp_bus bus)
{
	return (bus_ctx.instances[bus] != NULL);
}

static inline const struct rcp_bus_api *get_inst(enum rcp_bus bus)
{
	return bus_ctx.instances[bus];
}

enum rcp_bus_status rcp_bus_register(enum rcp_bus bus, struct rcp_bus_api *api)
{
	if (bus >= RCP_BUS_MAX || api == NULL) {
		return RCP_BUS_ERR_INVALID;
	}
	if (is_registered(bus)) {
		return RCP_BUS_ERR_MEMORY;
	}

	if (api->open == NULL || api->write == NULL || api->close == NULL || api->read == NULL ||
	    api->get_caps == NULL || api->get_rx_len == NULL) {
		return RCP_BUS_ERR_INVALID;
	}

	bus_ctx.instances[bus] = api;
	return RCP_BUS_OK;
}

enum rcp_bus_status rcp_bus_open(enum rcp_bus bus)
{
	if (!is_registered(bus)) {
		return RCP_BUS_ERR_INVALID;
	}

	return get_inst(bus)->open();
}

enum rcp_bus_status rcp_bus_close(enum rcp_bus bus)
{
	if (!is_registered(bus)) {
		return RCP_BUS_ERR_INVALID;
	}

	return get_inst(bus)->close();
}

enum rcp_bus_status rcp_bus_write(enum rcp_bus bus, uint8_t *tx, uint16_t len)
{
	if (!is_registered(bus) || tx == NULL) {
		return RCP_BUS_ERR_INVALID;
	}

	return get_inst(bus)->write(tx, len);
}

enum rcp_bus_status rcp_bus_read(enum rcp_bus bus, uint8_t *rx, uint16_t len, uint16_t *nread)
{
	if (!is_registered(bus) || rx == NULL) {
		return RCP_BUS_ERR_INVALID;
	}

	return get_inst(bus)->read(rx, len, nread);
}

enum rcp_bus_status rcp_bus_get_caps(enum rcp_bus bus, struct rcp_bus_caps *caps)
{
	if (!is_registered(bus) || caps == NULL) {
		return RCP_BUS_ERR_INVALID;
	}

	return get_inst(bus)->get_caps(caps);
}

uint16_t rcp_bus_get_rx_len(enum rcp_bus bus)
{
	if (!is_registered(bus)) {
		return RCP_BUS_ERR_INVALID;
	}
	return get_inst(bus)->get_rx_len();
}

const char *rcp_bus_status_to_str(enum rcp_bus_status status)
{
	return status_string[status];
}
