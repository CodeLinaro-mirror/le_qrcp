/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "buffer_mgmt.h"
#include "rcp_log.h"
#include "rcp_config.h"
#include "rcp_os.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define RCP_WIFI_RAM_SECTION __attribute__((section(".wifi_ram")))

#define BUF_SIZE_ALIGNED ((RCP_BUF_SIZE + 31) & ~31)
static uint8_t buffer_mem[RCP_NUM_BUFFERS][BUF_SIZE_ALIGNED] __aligned((32));

// RCP buffers
static rcp_buffer_hdl_t buff_hdl[RCP_NUM_BUFFERS] RCP_WIFI_RAM_SECTION;

// External buffers
static rcp_buffer_hdl_t ext_buff_hdl[RCP_NUM_EXT_BUFFERS] RCP_WIFI_RAM_SECTION;

static uint8_t buffers_available RCP_WIFI_RAM_SECTION;

static uint8_t start_idx RCP_WIFI_RAM_SECTION;
static rcp_mutex_t buffer_mtx RCP_WIFI_RAM_SECTION;

static void release_internal_buffer(rcp_buffer_hdl_t *buffer)
{
	if (buffer->in_use) {
		buffer->in_use = false;
		buffer->size = 0;
		/* Restore data_ptr to the buffer's own backing store. decode_rcp_msg
		 * may advance data_ptr past the RCP header for zero-copy payload
		 * delivery; without this restore the handle would be permanently
		 * offset and corrupt the next get_buffer() user. */
		size_t idx = (size_t)(buffer - buff_hdl);
		if (idx < RCP_NUM_BUFFERS) {
			buffer->data_ptr = buffer_mem[idx];
			buffer->max_size = RCP_BUF_SIZE;
		}
		buffers_available++;
		RCP_LOG_DBG("Release buffer; Free=%d\n", buffers_available);
	}
}

static void create_ext_buf_hdls(void)
{
	for (int idx = 0; idx < RCP_NUM_EXT_BUFFERS; idx++) {
		ext_buff_hdl[idx].type = 0;
		ext_buff_hdl[idx].in_use = false;
		ext_buff_hdl[idx].data_ptr = NULL;
	}
}

static void create_rcp_buffers(void)
{
	buffers_available = 0;
	start_idx = RCP_NUM_BUFFERS - 1;
	for (int idx = 0; idx < RCP_NUM_BUFFERS; idx++) {
		buff_hdl[idx].type = RCP_INT_BUFFER_TYPE;
		buff_hdl[idx].in_use = false;
		buff_hdl[idx].max_size = RCP_BUF_SIZE;
		buff_hdl[idx].size = 0;
		buff_hdl[idx].data_ptr = buffer_mem[idx];
		buffers_available++;
	}
	RCP_LOG_DBG("buff_hdl Free=%d\n", buffers_available);
}

void release_ext_buffer(rcp_buffer_hdl_t *ext_buffer)
{
	switch (ext_buffer->type) {
	case RCP_EXT_BUFF_TEST: {
		ext_buffer->type = 0;
		ext_buffer->in_use = false;
		free(ext_buffer->data_ptr);
		break;
	}

	default:
		RCP_LOG_ERR("Invalid release of buffer type %d\n", ext_buffer->type);
	}
}

void create_buffers()
{
	rcp_os_mutex_init(&buffer_mtx);
	create_rcp_buffers();
	create_ext_buf_hdls();
}

rcp_buffer_hdl_t *get_buffer(uint16_t size)
{
	rcp_os_mutex_lock(&buffer_mtx);
	if (buffers_available < 1) {
		RCP_LOG_DBG("No Free buff_hdl. All assigned =%d\n", RCP_NUM_BUFFERS);
		rcp_os_mutex_unlock(&buffer_mtx);
		return NULL;
	}

	if (size > RCP_BUF_SIZE) {
		RCP_LOG_ERR("Invalid buffer size requested %d > %d\n", size, RCP_BUF_SIZE);
		rcp_os_mutex_unlock(&buffer_mtx);
		return NULL;
	}
	// Find unused - try to rotate through buffers
	uint8_t search_idx = start_idx < (RCP_NUM_BUFFERS - 1) ? start_idx + 1 : 0;
	uint8_t search_count = 0;
	while (search_count++ <= RCP_NUM_BUFFERS) {
		if (buff_hdl[search_idx].in_use == false) {
			buff_hdl[search_idx].in_use = true;
			buffers_available--;
			RCP_LOG_DBG("Get buffer; Free=%d", buffers_available);
			start_idx = search_idx;
			rcp_os_mutex_unlock(&buffer_mtx);
			return &buff_hdl[search_idx];
		}
		search_idx = search_idx < (RCP_NUM_BUFFERS - 1) ? search_idx + 1 : 0;
	}
	rcp_os_mutex_unlock(&buffer_mtx);
	// No free buffer
	return NULL;
}

void release_buffer(rcp_buffer_hdl_t **buffer)
{
	rcp_os_mutex_lock(&buffer_mtx);
	rcp_buffer_hdl_t *buff_ptr = *buffer;
	if (buff_ptr) {
		if (buff_ptr->type == RCP_INT_BUFFER_TYPE) {
			release_internal_buffer(buff_ptr);
		} else {
			release_ext_buffer(buff_ptr);
		}
		*buffer = NULL;
	} else {
		RCP_LOG_DBG("Release of NULL buffer: buff_hdls=%d", buffers_available);
	}
	rcp_os_mutex_unlock(&buffer_mtx);
}

bool write_to_buffer(rcp_buffer_hdl_t *buffer, uint8_t *source_ptr, uint16_t size)
{
	if ((buffer == NULL) || (size > buffer->max_size) || !buffer->in_use) {
		return false;
	}
	buffer->size = size;
	memcpy(buffer->data_ptr, source_ptr, size);
	return true;
}

uint16_t read_from_buffer(uint8_t *dest_ptr, rcp_buffer_hdl_t *buffer, uint16_t size)
{
	if ((buffer == NULL) || !buffer->in_use) {
		return 0;
	}
	uint16_t xfer_size = size > buffer->size ? buffer->size : size;
	memcpy(dest_ptr, buffer->data_ptr, xfer_size);
	return xfer_size;
}

bool has_free_buffer(void)
{
	bool result;

	rcp_os_mutex_lock(&buffer_mtx);
	result = buffers_available > 0;
	rcp_os_mutex_unlock(&buffer_mtx);
	return result;
}

rcp_buffer_hdl_t *give_rcp_buff_ownership(uint8_t type, void *mem_ptr, uint16_t mem_size)
{
	rcp_os_mutex_lock(&buffer_mtx);
	switch (type) {
	case RCP_EXT_BUFF_TEST:
#ifndef RCP_OS_LINUX
		RCP_LOG_ERR("buffer type %d not supported\n", type);
		rcp_os_mutex_unlock(&buffer_mtx);
		return NULL;
#else
		break;
#endif
	default:
		RCP_LOG_ERR("Invalid buffer type %d\n", type);
		rcp_os_mutex_unlock(&buffer_mtx);
		return NULL;
		break;
	}
	// Assign handlle
	for (int idx = 0; idx < RCP_NUM_EXT_BUFFERS; idx++) {
		if (ext_buff_hdl[idx].in_use == false) {
			ext_buff_hdl[idx].in_use = true;
			ext_buff_hdl[idx].type = type;
			ext_buff_hdl[idx].size = 0;
			ext_buff_hdl[idx].max_size = mem_size;
			ext_buff_hdl[idx].data_ptr = mem_ptr;
			rcp_os_mutex_unlock(&buffer_mtx);
			return &ext_buff_hdl[idx];
		}
	}
	RCP_LOG_ERR("All %d external buffer used\n", RCP_NUM_EXT_BUFFERS);

	rcp_os_mutex_unlock(&buffer_mtx);
	return NULL;
}
