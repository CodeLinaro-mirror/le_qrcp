/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file buffer_mgmt.h
 *
 * @defgroup rcp_buffer_mgmt RCP buffer management
 *
 * RCP buffers are used to exchange messages between application and RCP protocol.
 * Buffers can be either acquired from common pool using get_buffer or an
 * existing application buffer can be reused using wrap_buffer to avoid extra copy.
 *
 * @ingroup rcp_public
 * @{
 */
#ifndef __BUFFER_MGMT_H__
#define __BUFFER_MGMT_H__

#include <stdint.h>
#include <stdbool.h>

/** Base value for external buffer types */
#define RCP_EXT_BUFF_BASELINE 0x01
/** Macro to generate external buffer type IDs */
#define RCP_EXT_BUFF_TYPE(ID) (RCP_EXT_BUFF_BASELINE + ID)
/** Test external buffer type */
#define RCP_EXT_BUFF_TEST     RCP_EXT_BUFF_TYPE(0x00)

/** Internal buffer type */
#define RCP_INT_BUFFER_TYPE 0x00

/**
 * @struct rcp_buffer_hdl_t
 * Handle for RCP buffers
 */
typedef struct rcp_buffer_hdl_t {
    uint8_t type;       /**< Type of buffer */
    bool in_use;        /**< Indicates if buffer is currently in use */
    uint16_t max_size;  /**< Maximum capacity of the buffer */
    uint16_t size;      /**< Current size of the buffer */
    uint8_t *data_ptr;  /**< Pointer to buffer data */
} rcp_buffer_hdl_t;

/**
 * Initialize RCP buffers
 */
void create_buffers(void);

/**
 * Acquire a free RCP buffer
 *
 * @param size Required size of the buffer
 * @return Pointer to buffer handle if successful, NULL otherwise
 */
rcp_buffer_hdl_t *get_buffer(uint16_t size);

/**
 * Free an RCP buffer
 *
 * @param buffer Pointer to buffer handle to release
 */
void release_buffer(rcp_buffer_hdl_t **buffer);

/**
 * Write raw data to an RCP buffer
 *
 * @param buffer Buffer handle
 * @param source_ptr Pointer to data to write
 * @param size Amount of data to write
 * @return true if successful, false otherwise
 */
bool write_to_buffer(rcp_buffer_hdl_t *buffer, uint8_t *source_ptr, uint16_t size);

/**
 * Read data from an RCP buffer
 *
 * @param dest_ptr Destination pointer to store data
 * @param buffer Buffer handle to read from
 * @param size Amount of data to read
 * @return Number of bytes actually read
 */
uint16_t read_from_buffer(uint8_t *dest_ptr, rcp_buffer_hdl_t *buffer, uint16_t size);

/**
 * Wrap external memory into an RCP buffer handle
 *
 * @param type Buffer type
 * @param mem_ptr Pointer to memory to wrap
 * @param mem_size Size of the memory
 * @return Pointer to buffer handle
 */
rcp_buffer_hdl_t *wrap_buffer(uint8_t type, void *mem_ptr, uint16_t mem_size);

/**
 * Check if at least one buffer is available without allocating it.
 *
 * @return true if a free buffer exists, false if pool is exhausted
 */
bool has_free_buffer(void);

/** @} end of rcp_buffer_mgmt */

#endif // __BUFFER_MGMT_H__
