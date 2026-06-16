/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __RCP_EVENT_H__
#define __RCP_EVENT_H__

#include <stdint.h>
#include <rcp_os.h>

/* RCP bus ready for TX. */
#define RCP_EVENT_TX_RDY 	1
/* RX data available on RCP bus. */
#define RCP_EVENT_RX_DATA	2
/* Data available to send in RCP TX queue. */
#define RCP_EVENT_TX_DATA	4

#define RCP_EVENT_ALL (RCP_EVENT_TX_RDY  | \
		       RCP_EVENT_RX_DATA | \
		       RCP_EVENT_TX_DATA)

/* Initialize events */
bool rcp_events_init(void);
/* Set a specific event flag. */
void rcp_events_set(uint8_t evt);
/* Clear an RCP event flag. */
void rcp_events_clear(uint8_t evt);
/* Get current event flags */
uint8_t rcp_events_get(void);
/* Wait for any of the specified events to occur. */
uint8_t rcp_events_wait_for_any(uint8_t evts, uint16_t timeout_ms);

#endif
