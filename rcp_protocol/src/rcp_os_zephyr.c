/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include "rcp_config.h"
#include "rcp_os.h"

#if RCP_LOG == 1
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(qualcomm_rcp, RCP_LOG_LEVEL);

#endif /* RCP_LOG */

bool rcp_os_sem_init(rcp_sem_t *sem, uint32_t cnt, uint32_t limit)
{
	if (0 != k_sem_init(&(sem->_sem), cnt, limit)) {
		return false;
	}
	return true;
}

bool rcp_os_sem_give(rcp_sem_t *sem)
{
	k_sem_give(&(sem->_sem));
	return true;
}

bool rcp_os_sem_take(rcp_sem_t *sem, uint32_t timeout)
{
	k_timeout_t t = (timeout == RCP_OS_WAIT_FOREVER) ? K_FOREVER : K_MSEC(timeout);

	if (0 != k_sem_take(&(sem->_sem), t)) {
		return false;
	}
	return true;
}

bool rcp_os_mutex_init(rcp_mutex_t *mtx)
{
	if (0 != k_mutex_init(&(mtx->_mtx))) {
		return false;
	}
	return true;
}

bool rcp_os_mutex_lock(rcp_mutex_t *mtx)
{
	if (0 != k_mutex_lock(&(mtx->_mtx), K_FOREVER)) {
		return false;
	}
	return true;
}

bool rcp_os_mutex_unlock(rcp_mutex_t *mtx)
{
	k_mutex_unlock(&(mtx->_mtx));
	return true;
}

void rcp_os_atomic_u8_init(rcp_atomic_u8_t *a, uint8_t value)
{
	atomic_set(&a->_val, value);
}

uint8_t rcp_os_atomic_u8_fetch_or(rcp_atomic_u8_t *a, uint8_t value)
{
	return (uint8_t)atomic_or(&a->_val, value);
}

uint8_t rcp_os_atomic_u8_fetch_and(rcp_atomic_u8_t *a, uint8_t value)
{
	return (uint8_t)atomic_and(&a->_val, (atomic_val_t)value);
}

uint8_t rcp_os_atomic_u8_load(rcp_atomic_u8_t *a)
{
	return (uint8_t)atomic_get(&a->_val);
}

uint32_t rcp_os_time_ms(void)
{
	return k_uptime_get_32();
}

void rcp_os_delay_ms(uint32_t time_ms)
{
	k_sleep(K_MSEC(time_ms));
}
