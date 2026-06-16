/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file rcp_error_codes.h
 *
 * @defgroup rcp_error_codes RCP NACK message error handling
 * @{
 * @ingroup rcp_public
 */
#ifndef __RCP_ERROR_CODES_H__
#define __RCP_ERROR_CODES_H__

#include "buffer_mgmt.h"

// Includes terminating null
#define MAX_ERROR_MSG_SIZE 31

enum rcp_error_code_t {
	RCP_ERR_UNDEFINED,         /**< No code set/Unknown */
	RCP_ERR_MSG,               /**< Debug message sent */
	RCP_ERR_TSN_OUT_SEQ,       /**< Rx TSN indicates sequence error */
	RCP_ERR_RSN_OUT_SEQ,       /**< Rx RSN indicates (Ack) sequence error */
	RCP_ERR_UNHANDELED_STREAM, /**< Handler has not been set for stream */
	RCP_ERR_HW_ERROR,          /**< HW problem is affecting transport */
};

// For Seq NUm mismatch
typedef struct {
	uint8_t actual;
	uint8_t expected;
} invalid_seq_num_t;

// For Error messages
typedef struct {
	uint8_t msg_size;
	char msg[MAX_ERROR_MSG_SIZE];
} err_msg_ind_t;

typedef struct rcp_error_info_t {
	enum rcp_error_code_t err_code;
	char msg[MAX_ERROR_MSG_SIZE];
	union {
		invalid_seq_num_t out_of_seq;
		err_msg_ind_t err_msg;
	} err_data;
} rcp_error_info_t;

/** Helper functions */
void set_err_msg(const char *msg, uint8_t msg_size, rcp_error_info_t *error_info);

void set_rx_seq_err(uint8_t actual, uint8_t expected, rcp_error_info_t *error_info);

void set_ack_seq_err(uint8_t rx_rsn, uint8_t current_tsn, rcp_error_info_t *error_info);

/** Marshall error code information into message buffer */
bool marshall_nak_error(rcp_buffer_hdl_t **buffer, rcp_error_info_t *error_info);

/** Decode error message buffer into error format*/
bool decode_nak_error_info(rcp_error_info_t *error_info, rcp_buffer_hdl_t *buffer);

/** @} */

#endif
