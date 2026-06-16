/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <semaphore.h>
#include <time.h>
#include <pthread.h>
#include "rcp_os.h"

bool rcp_os_sem_init(rcp_sem_t *sem, uint32_t cnt, uint32_t limit)
{
	sem_init(&(sem->_sem), 0, 0);
	return true;
}

bool rcp_os_sem_give(rcp_sem_t *sem)
{
	sem_post(&(sem->_sem));
	return true;
}

bool rcp_os_sem_take(rcp_sem_t *sem, uint32_t timeout)
{
	sem_wait(&(sem->_sem));
	return true;
}

bool rcp_os_mutex_init(rcp_mutex_t *mtx)
{
	return (pthread_mutex_init(&(mtx->_mtx), NULL) == 0);
}

bool rcp_os_mutex_lock(rcp_mutex_t *mtx)
{
	return (pthread_mutex_lock(&(mtx->_mtx)) == 0);
}

bool rcp_os_mutex_unlock(rcp_mutex_t *mtx)
{
	return (pthread_mutex_unlock(&(mtx->_mtx)) == 0);
}

void rcp_os_atomic_u8_init(rcp_atomic_u8_t *a, uint8_t value)
{
	__atomic_store_n(&a->_val, value, __ATOMIC_RELAXED);
}

uint8_t rcp_os_atomic_u8_fetch_or(rcp_atomic_u8_t *a, uint8_t value)
{
	return __atomic_fetch_or(&a->_val, value, __ATOMIC_RELEASE);
}

uint8_t rcp_os_atomic_u8_fetch_and(rcp_atomic_u8_t *a, uint8_t value)
{
	return __atomic_fetch_and(&a->_val, value, __ATOMIC_RELEASE);
}

uint8_t rcp_os_atomic_u8_load(rcp_atomic_u8_t *a)
{
	return __atomic_load_n(&a->_val, __ATOMIC_ACQUIRE);
}

uint32_t rcp_os_time_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

void rcp_os_delay_ms(uint32_t time_ms)
{
	struct timespec ts = {
		.tv_sec = time_ms / 1000,
		.tv_nsec = (time_ms % 1000) * 1000000ULL,
	};

	nanosleep(&ts, NULL);
}
