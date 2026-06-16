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

#endif /* RCP_BUS_SPI_MASTER_LINUX_QCC730_H */
