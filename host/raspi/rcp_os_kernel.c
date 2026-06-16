/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * RCP OS abstraction — Linux kernel implementation.
 * Provides rcp_os.h API using kernel semaphore/mutex/atomic primitives.
 */

#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/types.h>

#include "rcp_os.h"

bool rcp_os_sem_init(rcp_sem_t *sem, uint32_t cnt, uint32_t limit)
{
	sema_init(&sem->_sem, (int)cnt);
	return true;
}

bool rcp_os_sem_give(rcp_sem_t *sem)
{
	up(&sem->_sem);
	return true;
}

bool rcp_os_sem_take(rcp_sem_t *sem, uint32_t timeout_ms)
{
	int ret;

	if (timeout_ms == UINT_MAX || timeout_ms == 0xFFFF) {
		ret = down_interruptible(&sem->_sem);
	} else if (timeout_ms == 0) {
		ret = down_trylock(&sem->_sem);
	} else {
		ret = down_timeout(&sem->_sem, msecs_to_jiffies(timeout_ms));
	}
	return ret == 0;
}

bool rcp_os_mutex_init(rcp_mutex_t *mtx)
{
	mutex_init(&mtx->_mtx);
	return true;
}

bool rcp_os_mutex_lock(rcp_mutex_t *mtx)
{
	mutex_lock(&mtx->_mtx);
	return true;
}

bool rcp_os_mutex_unlock(rcp_mutex_t *mtx)
{
	mutex_unlock(&mtx->_mtx);
	return true;
}

void rcp_os_atomic_u8_init(rcp_atomic_u8_t *a, uint8_t value)
{
	atomic_set(&a->_val, (int)value);
}

uint8_t rcp_os_atomic_u8_fetch_or(rcp_atomic_u8_t *a, uint8_t value)
{
	return (uint8_t)atomic_fetch_or((int)value, &a->_val);
}

uint8_t rcp_os_atomic_u8_fetch_and(rcp_atomic_u8_t *a, uint8_t value)
{
	return (uint8_t)atomic_fetch_and((int)value, &a->_val);
}

uint8_t rcp_os_atomic_u8_load(rcp_atomic_u8_t *a)
{
	return (uint8_t)atomic_read(&a->_val);
}

uint32_t rcp_os_time_ms(void)
{
	return (uint32_t)jiffies_to_msecs(jiffies);
}

void rcp_os_delay_ms(uint32_t time_ms)
{
	msleep(time_ms);
}
