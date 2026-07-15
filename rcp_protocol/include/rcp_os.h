/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file rcp_os.h
 *
 * @defgroup rcp_os RCP OS abstraction layer
 *
 * Common API for RCP code wrapping required OS functions and primitives
 * for different target platforms.
 * @{
 * @ingroup rcp_internal
 */
#ifndef __RCP_OS_H__
#define __RCP_OS_H__

#include <stdbool.h>
#include <stdint.h>
#include "rcp_config.h"

#ifdef RCP_OS_ZEPHYR
#include <zephyr/kernel.h>
#elif defined(RCP_OS_FREERTOS)
#include <FreeRTOS.h>
#include <semphr.h>
#elif defined(RCP_OS_KERNEL)
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#elif defined(RCP_OS_LINUX)
#include <semaphore.h>
#include <pthread.h>
#endif

/** RCP semaphore wrapper */
typedef struct {
#ifdef RCP_OS_ZEPHYR
	/** OS-specific semaphore. */
	struct k_sem _sem;
#elif defined(RCP_OS_FREERTOS)
	SemaphoreHandle_t _sem;
#elif defined(RCP_OS_KERNEL)
	struct semaphore _sem;
#elif defined(RCP_OS_LINUX)
	sem_t _sem;
#endif
} rcp_sem_t;

typedef struct {
#ifdef RCP_OS_ZEPHYR
	/** OS-specific mutex. */
	struct k_mutex _mtx;
#elif defined(RCP_OS_FREERTOS)
	SemaphoreHandle_t _mtx;
#elif defined(RCP_OS_KERNEL)
	struct mutex _mtx;
#elif defined(RCP_OS_LINUX)
	pthread_mutex_t _mtx;
#endif
} rcp_mutex_t;

typedef struct {
#ifdef RCP_OS_ZEPHYR
	atomic_t _val;
#elif defined(RCP_OS_KERNEL)
	atomic_t _val;
#else
	uint8_t _val;
#endif
} rcp_atomic_u8_t;

/**
 * Initialize a semaphore.
 *
 * @param sem Semaphore to initialize.
 * @param cnt Initial semaphore count.
 * @param limit Count limit.
 *
 * @return true if semaphore was initialized successfully.
 */
bool rcp_os_sem_init(rcp_sem_t *sem, uint32_t cnt, uint32_t limit);

/**
 * Give a semaphore.
 *
 * @param sem Semaphore to give.
 * @return true if semaphore was successfully given.
 */
bool rcp_os_sem_give(rcp_sem_t *sem);

/* Block indefinitely in rcp_os_sem_take (no timeout). Lets the caller's thread
 * stay pending with no kernel timeout so the SoC can enter deep sleep (S2RAM)
 * instead of being woken by a periodic wait timeout. */
#define RCP_OS_WAIT_FOREVER 0xFFFFFFFFU

/**
 * Take a semaphore.
 *
 * @param sem Pointer to semaphore.
 * @param timeout Timeout in milliseconds, or RCP_OS_WAIT_FOREVER to block forever.
 * @return true if semaphore successfully taken, false if timeout or other error occurs.
 */
bool rcp_os_sem_take(rcp_sem_t *sem, uint32_t timeout);

/**
 * Initialize a mutex.
 * @param mtx Mutex to initialize.
 * */
bool rcp_os_mutex_init(rcp_mutex_t *mtx);

/**
 * Lock a mutex (blocks forever).
 * @param mtx Mutex to lock.
 */
bool rcp_os_mutex_lock(rcp_mutex_t *mtx);

/**
 * Unlock a mutex.
 * @param mtx Mutex to unlock.
 */
bool rcp_os_mutex_unlock(rcp_mutex_t *mtx);

/**
 * Initialize an atomic u8 value.
 */
void rcp_os_atomic_u8_init(rcp_atomic_u8_t *a, uint8_t value);

/**
 * Atomic OR, returns the previous value.
 */
uint8_t rcp_os_atomic_u8_fetch_or(rcp_atomic_u8_t *a, uint8_t value);

/**
 * Atomic AND, returns the previous value.
 */
uint8_t rcp_os_atomic_u8_fetch_and(rcp_atomic_u8_t *a, uint8_t value);

/**
 * Atomic load.
 */
uint8_t rcp_os_atomic_u8_load(rcp_atomic_u8_t *a);

/** Get current time in milliseconds.
 *
 * @return runtime in milliseconds
 */
uint32_t rcp_os_time_ms(void);

/** Delay (sleep) for specified time in milliseconds.
 *
 * @param time_ms time to sleep in milliseconds.
 */
void rcp_os_delay_ms(uint32_t time_ms);

/** @} */

#endif
