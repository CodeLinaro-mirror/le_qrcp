/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef RCP_BUS_SPI_MASTER_LINUX_QCC730_H
#define RCP_BUS_SPI_MASTER_LINUX_QCC730_H

#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>

#include <rcp_bus.h>
#include <rcp_event.h>

int  rcp_spi_master_probe(struct spi_device *spi, struct gpio_desc *irq_gpio,
			  int irq);
void rcp_spi_master_remove(void);

/* Ensure the QCC730 is awake before an SPI write (Task 11 TX path):
 * pulses GPIO16 EXT_WAKEUP, waits for the firmware AWAKE power event, retries,
 * then falls back to reset. Returns 0 once awake, negative on give-up. */
struct rcp_spi_master;
int  qcc730_wakeup(struct rcp_spi_master *priv);

/* No-arg wakeup for callers without a struct rcp_spi_master handle. */
int  rcp_bus_wakeup_qcc730(void);

#endif /* RCP_BUS_SPI_MASTER_LINUX_QCC730_H */
