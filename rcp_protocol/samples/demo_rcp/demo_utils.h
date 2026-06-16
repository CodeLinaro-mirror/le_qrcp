/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __DEMO_UTILS_H_
#define __DEMO_UTILS_H_

#include <stdint.h>
#include <rcp_protocol.h>
#include <rcp_interface.h>
#include "buffer_mgmt.h"

#define DEMO_MAX_PAYLOAD_SIZE 128

#define EXIT_MSG "EXIT"

#define RESP_TEST_MSG "ACK_REQUIRED"

uint16_t random_fill(rcp_buffer_hdl_t *buffer_ptr, uint16_t max_size);

void display_buffer(char *title, rcp_buffer_hdl_t *buffer_ptr, bool dispaly_as_text);

int get_rnd_number(int max_bound);

char *display_state(link_state_t state);

char *display_error(link_result_t error);

link_result_t send_numbered_msg(uint8_t stream, const char *base_msg, int message_num,
				rcp_buffer_hdl_t *msg_buffer);
char *display_err_code(uint8_t error);

#endif
