/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "rcp_event.h"
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>

#include <rcp_bus.h>
#include <rcp_bus_spi_header.h>
#include <buffer_mgmt.h>

LOG_MODULE_REGISTER(rcp_bus_spi_slave, CONFIG_QUALCOMM_RCP_BUS_LOG_LEVEL);

#define DT_DRV_COMPAT qcom_rcp_bus_spi_slave

#define TX_BUFFER_SIZE RCP_BUF_SIZE
#define RX_BUFFER_SIZE RCP_BUF_SIZE

#define MAX_RX_PACKETS CONFIG_RCP_BUS_SPI_RX_QUEUE_SIZE
#define MAX_TX_PACKETS CONFIG_RCP_BUS_SPI_TX_QUEUE_SIZE

/* Maximum payload size (MTU) over SPI */
#define MAX_DATA_SIZE       (TX_BUFFER_SIZE - SPI_HEADER_SIZE)
/* The transmission uses fixed size messages. Actual payload length will be
 * set in the header. */
#define SPI_FIXED_XFER_SIZE TX_BUFFER_SIZE

static const struct device *const spi_bus = DEVICE_DT_GET(DT_BUS(DT_DRV_INST(0)));
static struct spi_config spi_cfg = {
	.operation = SPI_WORD_SET(8) | SPI_OP_MODE_SLAVE,
};
static const struct gpio_dt_spec irq_gpio = GPIO_DT_SPEC_INST_GET(0, irq_gpios);

static struct {
	bool open;
} ctx;

/* Packet descriptor for TX / RX. */
struct spi_pkt {
	void *fifo; /* Reserved for Zephyr FIFO use */
	rcp_buffer_hdl_t *buf;
} __aligned(32);

static struct spi_pkt rx_pkts[MAX_RX_PACKETS];
static struct spi_pkt tx_pkts[MAX_TX_PACKETS];

static uint8_t rx_hdr_buf[RX_BUFFER_SIZE] __aligned(32);
static uint8_t tx_hdr_buf[TX_BUFFER_SIZE] __aligned(32);

static K_FIFO_DEFINE(rx_pkts_q);
static K_FIFO_DEFINE(tx_pkts_q);
static K_FIFO_DEFINE(free_rx_pkts_q);
static K_FIFO_DEFINE(free_tx_pkts_q);

/* Initialize free TX and RX packets. */
static void init_pkts(void);
/* Allocate RX packet. */
static struct spi_pkt *alloc_rx_pkt(void);
/* Allocate TX packet. */
static struct spi_pkt *alloc_tx_pkt(void);
/* Return RX packet to free queue. */
static void free_rx_pkt(struct spi_pkt *pkt);
/* Return TX packet to free queue. */
static void free_tx_pkt(struct spi_pkt *pkt);

static int spi_slave_init(const struct device *dev);
static void spi_xfer(uint8_t *rx, uint8_t *tx, uint16_t tx_len, uint16_t rx_len, uint16_t *nread,
		     uint16_t *nwrite);
static void rxtx_thread(void *, void *, void *);

static enum rcp_bus_status spi_slave_open(void);
static enum rcp_bus_status spi_slave_close(void);
static enum rcp_bus_status spi_slave_write(uint8_t *tx, uint16_t len);
static enum rcp_bus_status spi_slave_read(uint8_t *rx, uint16_t len, uint16_t *nread);
static enum rcp_bus_status spi_slave_get_caps(struct rcp_bus_caps *caps);
static uint16_t spi_slave_get_rx_len(void);

K_THREAD_DEFINE(rcp_bus_spi_slave_rx, 1024, rxtx_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, 0);

static struct rcp_bus_api spi_slave_api = {
	.open = spi_slave_open,
	.close = spi_slave_close,
	.write = spi_slave_write,
	.read = spi_slave_read,
	.get_caps = spi_slave_get_caps,
	.get_rx_len = spi_slave_get_rx_len,
};

static void free_rx_pkt(struct spi_pkt *pkt)
{
	if (pkt) {
		release_buffer(&pkt->buf);
		k_fifo_put(&free_rx_pkts_q, pkt);
	}
}

static void free_tx_pkt(struct spi_pkt *pkt)
{
	if (pkt) {
		release_buffer(&pkt->buf);
		k_fifo_put(&free_tx_pkts_q, pkt);
	}
}

static struct spi_pkt *alloc_rx_pkt(void)
{
	rcp_buffer_hdl_t *buf = NULL;
	struct spi_pkt *pkt = NULL;

	buf = get_buffer(RX_BUFFER_SIZE);
	if (!buf) {
		return NULL;
	}

	pkt = k_fifo_get(&free_rx_pkts_q, K_NO_WAIT);
	if (!pkt) {
		LOG_ERR("RX queue full!");
		release_buffer(&buf);
		return NULL;
	}

	pkt->buf = buf;
	return pkt;
}

static struct spi_pkt *alloc_tx_pkt(void)
{
	rcp_buffer_hdl_t *buf = NULL;
	struct spi_pkt *pkt = NULL;

	buf = get_buffer(TX_BUFFER_SIZE);
	if (!buf) {
		return NULL;
	}

	pkt = k_fifo_get(&free_tx_pkts_q, K_NO_WAIT);
	if (!pkt) {
		LOG_ERR("TX queue full!");
		release_buffer(&buf);
		return NULL;
	}

	pkt->buf = buf;
	return pkt;
}

static void init_pkts(void)
{
	for (int i = 0; i < MAX_RX_PACKETS; i++) {
		rx_pkts[i].buf = NULL;
		k_fifo_put(&free_rx_pkts_q, &rx_pkts[i]);
	}
	for (int i = 0; i < MAX_TX_PACKETS; i++) {
		tx_pkts[i].buf = NULL;
		k_fifo_put(&free_tx_pkts_q, &tx_pkts[i]);
	}
}

static void rxtx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		/* Poll master for incoming data. */
		if (ctx.open) {
			uint16_t nread = 0;
			uint16_t nwrite = 0;
			uint16_t rx_len = 0;
			uint16_t tx_len = 0;
			uint8_t *rx_buf;
			uint8_t *tx_buf;

			/* Check if there is room to receive a packet. If not, master can
			 * only read data from us. */
			struct spi_pkt *rx_pkt = alloc_rx_pkt();
			if (rx_pkt) {
				rx_len = rx_pkt->buf->max_size - SPI_HEADER_SIZE;
				rx_buf = rx_pkt->buf->data_ptr;
			} else {
				/* Use a static buffer to accept master header. */
				rx_len = 0;
				rx_buf = rx_hdr_buf;
			}

			/* Check if there are any TX packets for sending. */
			struct spi_pkt *tx_pkt = (struct spi_pkt *)k_fifo_peek_head(&tx_pkts_q);
			if (tx_pkt) {
				tx_len = tx_pkt->buf->size;
				tx_buf = tx_pkt->buf->data_ptr;
			} else {
				/* Use send header only. */
				tx_len = 0;
				tx_buf = tx_hdr_buf;
			}

			/* Transceive data. */
			spi_xfer(rx_buf, tx_buf, tx_len, rx_len, &nread, &nwrite);

			if (nwrite > 0) {
				__ASSERT_NO_MSG(nwrite == tx_len);
				/* Data written to master. Packet can be freed. */
				(void)k_fifo_get(&tx_pkts_q, K_NO_WAIT);
				free_tx_pkt(tx_pkt);
				/* No more data, reset IRQ signal. */
				if (k_fifo_is_empty(&tx_pkts_q)) {
					gpio_pin_set_dt(&irq_gpio, 0);
				}
			} else {
				; /* Keep trying to send the same packet. */
			}

			if (nread > 0) {
				/* Data read from master. Put packet to RX queue. */
				rx_pkt->buf->size = nread;
				k_fifo_put(&rx_pkts_q, rx_pkt);
				rcp_events_set(RCP_EVENT_RX_DATA);
			} else {
				/* Free unused packet. */
				free_rx_pkt(rx_pkt);
			}
		} else {
			k_sleep(K_SECONDS(1));
		}
	}
}

static enum rcp_bus_status spi_slave_open(void)
{
	ctx.open = true;
	rcp_events_set(RCP_EVENT_TX_RDY);
	return RCP_BUS_OK;
};

static enum rcp_bus_status spi_slave_close(void)
{
	rcp_events_clear(RCP_EVENT_TX_RDY);
	ctx.open = false;
	return RCP_BUS_OK;
}

static void spi_xfer(uint8_t *rx, uint8_t *tx, uint16_t tx_len, uint16_t rx_len, uint16_t *nread,
		     uint16_t *nwrite)
{
	/* Use fixed transmission size. Fixed transmission size between master and
	 * slave reduces the delay in slave driver after transfer is completed as well
	 * as removes the need for extra transaction to query data length from slave.
	 * Longer transaction means some cycles are wasted for small packets but
	 * overall it reduces the delay and improves the bandwidth.
	 */
	uint16_t xfer_len = SPI_FIXED_XFER_SIZE;

	struct spi_buf tx_buf = {
		.buf = tx,
		.len = xfer_len,
	};
	struct spi_buf rx_buf = {
		.buf = rx,
		.len = xfer_len,
	};

	struct spi_buf_set rx_bufs = {.buffers = &rx_buf, .count = 1};
	struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};

	/* Buffer will have room for the headers at the beginning. */
	rcp_bus_spi_header_init(tx_buf.buf);
	rcp_bus_spi_header_set_accept_length(tx_buf.buf, rx_len);
	rcp_bus_spi_header_set_data_length(tx_buf.buf, tx_len);

	LOG_HEXDUMP_DBG(tx_buf.buf, tx_len + SPI_HEADER_SIZE, "SPI TX: header + data");

	int act_xfer_len = spi_transceive(spi_bus, &spi_cfg, &tx_bufs, &rx_bufs);
	if (act_xfer_len < 0) {
		LOG_ERR("SPI transceive failed: %d", act_xfer_len);
	} else {
		LOG_DBG("Last transfer length: %d", act_xfer_len);
	}

	uint8_t *rx_frame = (uint8_t *)rx_buf.buf;
	if (!rcp_bus_spi_header_is_valid(rx_frame)) {
		LOG_ERR("Invalid master header.");
		return;
	}

	uint16_t master_accept = rcp_bus_spi_header_get_accept_length(rx_frame);
	uint16_t master_data = rcp_bus_spi_header_get_data_length(rx_frame);

	/* 0xFFFF is often indicatation of bus inactive / failure. */
	if (master_accept == 0xFFFF || master_data == 0xFFFF) {
		LOG_ERR("SPI master corrupted header.");
		return;
	}

	/* Master couldn't read the message. */
	if (master_accept < tx_len) {
		*nwrite = 0;
	} else {
		/* assume master has accepted the message. */
		*nwrite = tx_len;
	}

	if (master_data > rx_len) {
		/* Master tried to write more data than possible. */
		LOG_ERR("Can't read data from master: %d > %d", master_data, rx_len);
		*nread = 0;
	} else {
		*nread = master_data;
	}

	LOG_HEXDUMP_DBG(&rx_frame[SPI_HEADER_SIZE], master_data, "SPI RX: data");
}

static enum rcp_bus_status spi_slave_read(uint8_t *rx, uint16_t len, uint16_t *nread)
{
	struct spi_pkt *pkt = NULL;

	pkt = k_fifo_get(&rx_pkts_q, K_NO_WAIT);
	if (!pkt) {
		LOG_WRN("No data to read");
		return RCP_BUS_ERR_EMPTY;
	}

	memcpy(rx, &pkt->buf->data_ptr[SPI_HEADER_SIZE], pkt->buf->size);
	*nread = pkt->buf->size;
	free_rx_pkt(pkt);

	/* No more data. */
	if (k_fifo_is_empty(&rx_pkts_q)) {
		rcp_events_clear(RCP_EVENT_RX_DATA);
	}

	return RCP_BUS_OK;
}

static enum rcp_bus_status spi_slave_write(uint8_t *tx, uint16_t len)
{
	struct spi_pkt *pkt = alloc_tx_pkt();
	if (!pkt) {
		LOG_ERR("Failed to alloc packet for writing");
		return RCP_BUS_ERR_MEMORY;
	}
	if (len > (TX_BUFFER_SIZE - SPI_HEADER_SIZE)) {
		LOG_ERR("Too much data to write over SPI");
		return RCP_BUS_ERR_MEMORY;
	}
	memset(pkt->buf->data_ptr, 0, TX_BUFFER_SIZE);
	/* Leave room for header. */
	pkt->buf->size = len;
	memcpy(&pkt->buf->data_ptr[SPI_HEADER_SIZE], tx, len);
	k_fifo_put(&tx_pkts_q, pkt);
	/* Notify master. */
	if (0 != gpio_pin_set_dt(&irq_gpio, 1)) {
		LOG_ERR("Failed to write pin!");
	}

	return RCP_BUS_OK;
}

static enum rcp_bus_status spi_slave_get_caps(struct rcp_bus_caps *caps)
{
	caps->mtu = MAX_DATA_SIZE;
	return RCP_BUS_OK;
}

static uint16_t spi_slave_get_rx_len(void)
{
	struct spi_pkt *pkt = (struct spi_pkt *)k_fifo_peek_head(&rx_pkts_q);

	return (pkt != NULL) ? pkt->buf->size : 0;
}

static int spi_slave_init(const struct device *dev)
{
	if (0 != gpio_pin_configure_dt(&irq_gpio, GPIO_OUTPUT | GPIO_ACTIVE_HIGH)) {
		LOG_ERR("Failed to configure IRQ GPIO");
		return -1;
	}
	(void)gpio_pin_set_dt(&irq_gpio, 0);
	init_pkts();

	if (RCP_BUS_OK != rcp_bus_register(RCP_BUS_SPI, &spi_slave_api)) {
		LOG_ERR("Failed to register RCP Bus SPI API");
		return -1;
	}
	LOG_INF("RCP bus SPI slave initialised");
	return 0;
}

DEVICE_DT_DEFINE(DT_DRV_INST(0), spi_slave_init, NULL, NULL, NULL, POST_KERNEL,
	CONFIG_SPI_INIT_PRIORITY, NULL);
