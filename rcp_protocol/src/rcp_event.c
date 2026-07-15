/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <rcp_event.h>
#include <rcp_os.h>

static struct {
	rcp_atomic_u8_t evts;
	rcp_sem_t sem;
} rcp_evt;

bool rcp_events_init(void)
{
	if (!rcp_os_sem_init(&rcp_evt.sem, 0, 1)) {
		return false;
	}
	rcp_os_atomic_u8_init(&rcp_evt.evts, 0);
	return true;
}

void rcp_events_set(uint8_t evts)
{
	if (evts) {
		(void)rcp_os_atomic_u8_fetch_or(&rcp_evt.evts, evts);
		rcp_os_sem_give(&rcp_evt.sem);
	}
}

void rcp_events_clear(uint8_t evts)
{
	if (evts) {
		(void)rcp_os_atomic_u8_fetch_and(&rcp_evt.evts, (uint8_t)~evts);
	}
}

uint8_t rcp_events_get(void)
{
	return rcp_os_atomic_u8_load(&rcp_evt.evts);
}

uint8_t rcp_events_wait_for_any(uint8_t evts, uint16_t timeout_ms)
{
	uint32_t start = rcp_os_time_ms();
	uint32_t elapsed = 0;

	while (true) {
		uint8_t cur = rcp_os_atomic_u8_load(&rcp_evt.evts);
		if (cur & evts) {
			return (cur & evts);
		}
		/* Return immediately. */
		if (timeout_ms == 0) {
			return 0;
		}

		/* Block forever: stay pending with no kernel timeout so the SoC can
		 * enter deep sleep. Woken only by rcp_events_set (TX/RX events). */
		if (timeout_ms == RCP_WAIT_FOREVER) {
			if (true != rcp_os_sem_take(&rcp_evt.sem, RCP_OS_WAIT_FOREVER)) {
				return 0;
			}
			/* Got notification, loop and re-check flags. */
			continue;
		}

		/* Adjust timeout if we are waiting for more flags. */
		elapsed = rcp_os_time_ms() - start;
		if (elapsed >= timeout_ms) {
			return 0;
		}

		if (true != rcp_os_sem_take(&rcp_evt.sem, timeout_ms - elapsed)) {
			return 0;
		} else {
			/* Got notification, continue with loop and check flags. */
		}
	}
}
