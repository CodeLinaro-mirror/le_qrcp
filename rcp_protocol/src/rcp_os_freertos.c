/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stdbool.h>
#include <rcp_os.h>
#include <FreeRTOS.h>
#include <semphr.h>

bool rcp_os_sem_init(rcp_sem_t *sem, uint32_t cnt, uint32_t limit)
{
	sem->_sem = xSemaphoreCreateCounting(limit, cnt);
	return (sem->_sem != NULL);
}

bool rcp_os_sem_give(rcp_sem_t *sem)
{
	if (xPortIsInsideInterrupt()) {
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		if (pdTRUE != xSemaphoreGiveFromISR(sem->_sem, &xHigherPriorityTaskWoken)) {
			return false;
		}
		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
		return true;
	}

	if (pdTRUE != xSemaphoreGive(sem->_sem)) {
		return false;
	}
	return true;
}

bool rcp_os_sem_take(rcp_sem_t *sem, uint32_t timeout)
{
	TickType_t ticks = (timeout == RCP_OS_WAIT_FOREVER)
				   ? portMAX_DELAY
				   : (timeout / portTICK_PERIOD_MS);

	if (pdTRUE != xSemaphoreTake(sem->_sem, ticks)) {
		return false;
	}
	return true;
}

bool rcp_os_mutex_init(rcp_mutex_t *mtx)
{
	mtx->_mtx = xSemaphoreCreateMutex();
	return (mtx->_mtx != NULL);
}

bool rcp_os_mutex_lock(rcp_mutex_t *mtx)
{
	if (pdTRUE != xSemaphoreTake(mtx->_mtx, portMAX_DELAY)) {
		return false;
	}
	return true;
}

bool rcp_os_mutex_unlock(rcp_mutex_t *mtx)
{
	if (pdTRUE != xSemaphoreGive(mtx->_mtx)) {
		return false;
	}
	return true;
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
	return (xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void rcp_os_delay_ms(uint32_t time_ms)
{
	vTaskDelay((time_ms / portTICK_PERIOD_MS));
}
