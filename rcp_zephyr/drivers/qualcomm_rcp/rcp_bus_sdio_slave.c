/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sdio_device.h>
#include <zephyr/cache.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/ring_buffer.h>

#include <rcp_event.h>
#include <rcp_bus.h>

#define DT_DRV_COMPAT qcom_rcp_bus_sdio_slave

#define SDIO_DATA_TYPE 0x55
#define SDIO_MAX_PORT_NUM 16

#define SDIO_MAX_PORT_SIZE DT_PROP(DT_BUS(DT_DRV_INST(0)), download_size_max)
#define ALIGN_CACHE_LINE_SIZE CONFIG_DCACHE_LINE_SIZE
#define SDIO_UPLOAD_BUFFER_WARN_INTERVAL_MS 1000U
#define SDIO_HOST_READY_TIMEOUT 3000

LOG_MODULE_REGISTER(rcp_bus_sdio_slave, CONFIG_QUALCOMM_RCP_BUS_LOG_LEVEL);

/* Memory slab for dnld and upld buffers. */
K_MEM_SLAB_DEFINE_STATIC(buf_slab, SDIO_MAX_PORT_SIZE, 2 * SDIO_MAX_PORT_NUM,
	ALIGN_CACHE_LINE_SIZE);
/* Memory slab for RX descriptors. */
K_MEM_SLAB_DEFINE_STATIC(desc_slab, sizeof(sdio_transfer_desc_t), 2 * SDIO_MAX_PORT_NUM,
	ALIGN_CACHE_LINE_SIZE);

/* Queue to keep track of RX sdio_transfer_desc_t structures. */
K_FIFO_DEFINE(rx_queue);

struct sdio_tlv {
	uint16_t type;
	uint16_t len;
	uint8_t data[0];
};

static const struct device *const sdio_bus = DEVICE_DT_GET(DT_BUS(DT_DRV_INST(0)));

static struct {
	bool open;
	enum rcp_bus inst;
	uint16_t to_read;
	struct k_work open_work;
} ctx;

static int sdio_slave_init(const struct device *dev);
static void sdio_dnld_callback(const struct device *dev, sdio_transfer_desc_t *trans_desc);
static void sdio_upld_callback(const struct device *dev, sdio_transfer_desc_t *trans_dec);
static bool init_dnld_buffers(void);

static void log_upload_buffer_unavailable(void)
{
	static uint32_t last_log_ms;
	static uint32_t suppressed_logs;
	uint32_t now = k_uptime_get_32();

	if ((last_log_ms == 0U) || ((now - last_log_ms) >= SDIO_UPLOAD_BUFFER_WARN_INTERVAL_MS)) {
		if (suppressed_logs > 0U) {
			LOG_WRN("Upload buffer remained unavailable; suppressed %u duplicate error(s)",
				(unsigned int)suppressed_logs);
			suppressed_logs = 0U;
		}

		LOG_ERR("Upload buffer not available");
		last_log_ms = now;
		return;
	}

	suppressed_logs++;
}

static enum rcp_bus_status sdio_slave_open(void);
static enum rcp_bus_status sdio_slave_close(void);
static enum rcp_bus_status sdio_slave_write(uint8_t *tx, uint16_t len);
static enum rcp_bus_status sdio_slave_read(uint8_t *rx, uint16_t len, uint16_t *nread);
static enum rcp_bus_status sdio_slave_get_caps(struct rcp_bus_caps *caps);
static uint16_t sdio_slave_get_rx_len(void);
static bool wait_host_ready(uint16_t timeout_ms);
static void sdio_open_work_handler(struct k_work *work);

static struct rcp_bus_api sdio_slave_api = {
	.open = sdio_slave_open,
	.close = sdio_slave_close,
	.write = sdio_slave_write,
	.read = sdio_slave_read,
	.get_caps = sdio_slave_get_caps,
	.get_rx_len = sdio_slave_get_rx_len,

};

static bool wait_host_ready(uint16_t timeout_ms)
{
	bool ready = false;
	uint32_t start_time = k_uptime_get_32();
	while (k_uptime_get_32() < (start_time + (uint32_t)timeout_ms)) {
		ready = (bool)sdio_feature_control(sdio_bus, SDIO_CMD_GET_FUNC_HOST_READY, 0);
		if (ready) {
			break;
		}
		k_sleep(K_MSEC(100));
	}

	return ready;
}

static void sdio_open_work_handler(struct k_work *work)
{
	if (!wait_host_ready(SDIO_HOST_READY_TIMEOUT)) {
		LOG_WRN("SDIO host not ready after %d ms, retrying...", SDIO_HOST_READY_TIMEOUT);
		k_work_submit(&ctx.open_work);
	} else {
		LOG_INF("SDIO host ready");
		rcp_events_set(RCP_EVENT_TX_RDY);
	}
}

static enum rcp_bus_status sdio_slave_open(void)
{
	/* RCP may initialize quite early and try to open the bus and the slave needs to check or
	 * wait for master ready signal. Defer this to a work queue so that initialization
	 * is not delayed too much.
	 */
	k_work_submit(&ctx.open_work);
	return RCP_BUS_OK;
}

static enum rcp_bus_status sdio_slave_close(void)
{
	rcp_events_clear(RCP_EVENT_TX_RDY);
	return RCP_BUS_OK;
}

static enum rcp_bus_status sdio_slave_get_caps(struct rcp_bus_caps *caps)
{
	return RCP_BUS_OK;
}

static uint16_t sdio_slave_get_rx_len(void)
{
	sdio_transfer_desc_t *rx = (sdio_transfer_desc_t *)k_fifo_peek_head(&rx_queue);
	if (rx != NULL) {
		/* Return raw data only. */
		return ((rx->data_len) - sizeof(struct sdio_tlv));
	}
	return 0;
}

static bool init_dnld_buffers(void)
{
	for (int i = 0; i < SDIO_MAX_PORT_NUM; i++) {
		uint8_t *buf = NULL;
		int ret = k_mem_slab_alloc(&buf_slab, (void**)&buf, K_NO_WAIT);

		if (buf == NULL || ret < 0) {
			LOG_ERR("Failed to allocate buf");
			return false;
		}

		sdio_transfer_desc_t dnld_desc = {
			.buff = buf,
			.buff_len = SDIO_MAX_PORT_SIZE,
			.user_arg = NULL
		};
		ret = sdio_dnld_port_push(sdio_bus, &dnld_desc);

		if (ret != 0) {
			LOG_ERR("Failed to push buff");
			return false;
		}
	}
	return true;
}

static void sdio_dnld_callback(const struct device *dev, sdio_transfer_desc_t *trans_desc)
{
	sys_cache_data_invd_range(trans_desc->buff, trans_desc->buff_len);

	sdio_transfer_desc_t *rx;

	int ret = k_mem_slab_alloc(&desc_slab, (void**)&rx, K_NO_WAIT);

	if (rx == NULL || ret < 0) {
		LOG_ERR("Failed to alloc RX sdio_transfer_desc_t");
		sdio_dnld_port_push(sdio_bus, trans_desc);
		return;
	}
	/* Copy RX context. */
	*rx = *trans_desc;

	struct sdio_tlv *tlv = (struct sdio_tlv *)rx->buff;
	rx->data_len = tlv->len + sizeof(struct sdio_tlv);
	/* Garbage data. */
	if (tlv->type != SDIO_DATA_TYPE) {
		LOG_WRN("SDIO garbage data");
		LOG_HEXDUMP_DBG(rx->buff, 20, "first 20 bytes");
		/* Return buffer that was given to us back to the queue. */
		sdio_dnld_port_push(sdio_bus, trans_desc);
		k_mem_slab_free(&desc_slab, rx);
		return;
	}
	if (tlv->len == 0) {
		LOG_WRN("Empty SDIO message");
		/* Return buffer that was given to us back to the queue. */

		sdio_dnld_port_push(sdio_bus, trans_desc);
		k_mem_slab_free(&desc_slab, rx);
		return;
	}

	LOG_DBG("Download done. buf=%p, buf len=%d, data len=%d\n",
		rx->buff, rx->buff_len, rx->data_len);
	k_fifo_alloc_put(&rx_queue, rx);
	/* Notify application. */
	rcp_events_set(RCP_EVENT_RX_DATA);
	return;
}

static void sdio_upld_callback(const struct device *dev, sdio_transfer_desc_t *trans_desc)
{
	LOG_DBG("Upload done");
	k_mem_slab_free(&buf_slab, trans_desc->buff);
	rcp_events_set(RCP_EVENT_TX_RDY);
}

static void sdio_reset_callback(const struct device *dev)
{
	sdio_transfer_desc_t desc;
	/* Free all buffers. */
	LOG_WRN("SDIO reset triggered");
	rcp_events_clear(RCP_EVENT_TX_RDY | RCP_EVENT_RX_DATA);
	int freed = 0;
	while(sdio_dnld_port_pop(dev, &desc) == 0) {
		k_mem_slab_free(&buf_slab, desc.buff);
		freed++;
	}
	LOG_DBG("Freed %d dnld buffers", freed);

	freed = 0;
	while(sdio_upld_port_pop(dev, &desc) == 0) {
		k_mem_slab_free(&buf_slab, desc.buff);
		freed++;
	}
	LOG_DBG("Freed %d upld buffers", freed);

	while(!k_fifo_is_empty(&rx_queue)) {
		sdio_transfer_desc_t *rx_desc = (sdio_transfer_desc_t *)k_fifo_get(
			&rx_queue, K_NO_WAIT);
		k_mem_slab_free(&desc_slab, rx_desc);
	}

	/* Reinitialize peripheral and reset buffer queue. */
	sdio_re_initialize(dev);

	if (!init_dnld_buffers()) {
		LOG_ERR("Failed to reinitialize buffers");
		__ASSERT_NO_MSG(false);
	}
	k_work_submit(&ctx.open_work);
}

static enum rcp_bus_status sdio_slave_write(uint8_t *tx, uint16_t len)
{
	rcp_events_clear(RCP_EVENT_TX_RDY);

	if (len == 0 || tx == NULL) {
		rcp_events_set(RCP_EVENT_TX_RDY);
		return RCP_BUS_ERR_INVALID;
	}
	sdio_transfer_desc_t upld_desc;
	int ret = k_mem_slab_alloc(&buf_slab, (void**)&upld_desc.buff, K_NO_WAIT);

	if (upld_desc.buff == NULL || ret < 0) {
		log_upload_buffer_unavailable();
		/* Do not enable TX. Upload callback will reset TX_RDY flag after host reads a
		   buffer.*/
		return RCP_BUS_ERR_MEMORY;
	}
	upld_desc.buff_len = SDIO_MAX_PORT_SIZE;
	upld_desc.data_len = len + sizeof(struct sdio_tlv);
	uint8_t *tx_buf = (uint8_t *)upld_desc.buff;

	struct sdio_tlv tlv = {
		.type = SDIO_DATA_TYPE,
		.len = len,
	};
	memset(tx_buf, 0, upld_desc.buff_len);

	memcpy(tx_buf, &tlv, sizeof(struct sdio_tlv));
	memcpy(&tx_buf[sizeof(struct sdio_tlv)], tx, len);

	sys_cache_data_flush_range(upld_desc.buff, upld_desc.buff_len);

	LOG_HEXDUMP_DBG(tx_buf, upld_desc.data_len, "TX buf");

	if (0 != sdio_upld_port_push(sdio_bus, &upld_desc)) {
		k_mem_slab_free(&buf_slab, upld_desc.buff);
		LOG_ERR("Failed to push upload buffer");
		rcp_events_set(RCP_EVENT_TX_RDY);
		return RCP_BUS_ERR_IO;
	}
	rcp_events_set(RCP_EVENT_TX_RDY);
	return RCP_BUS_OK;
}

static enum rcp_bus_status sdio_slave_read(uint8_t *rx, uint16_t len, uint16_t *nread)
{
	if (rx == NULL || len == 0 || nread == NULL) {
		return RCP_BUS_ERR_INVALID;
	}

	if (k_fifo_is_empty(&rx_queue)) {
		LOG_WRN("No RX data");
		return RCP_BUS_ERR_EMPTY;
	}

	sdio_transfer_desc_t *rx_desc = (sdio_transfer_desc_t *)k_fifo_get(&rx_queue, K_NO_WAIT);

	if (rx_desc == NULL) {
		LOG_ERR("Failed to get buffer from RX queue");
		return RCP_BUS_ERR_IO;
	}

	uint8_t *rx_buf = (uint8_t *)rx_desc->buff;
	struct sdio_tlv *tlv = (struct sdio_tlv *)rx_buf;

	LOG_HEXDUMP_DBG(rx_buf, rx_desc->data_len, "RX");
	/* Get the actual data without TLV. */
	memcpy(rx, &rx_buf[sizeof(struct sdio_tlv)], tlv->len);
	*nread = tlv->len;

	memset(rx_desc->buff, 0, rx_desc->buff_len);
	/* Return buffer to dnld queue. */
	sdio_dnld_port_push(sdio_bus, rx_desc);
	/* Free the sdio_transfer_desc_t structure. Does not free the underlying buffer.
	 * Buffer is returned to download queue.
	 */
	k_mem_slab_free(&desc_slab, rx_desc);

	if (k_fifo_is_empty(&rx_queue)) {
		rcp_events_clear(RCP_EVENT_RX_DATA);
	}

	return RCP_BUS_OK;
}

static int sdio_slave_init(const struct device *dev)
{
	ctx.inst = RCP_BUS_SDIO;

	if (!device_is_ready(sdio_bus)) {
		LOG_ERR("SDIO bus not ready!");
		return -1;
	}

	sdio_set_download_callback(sdio_bus, sdio_dnld_callback);
	sdio_set_upload_callback(sdio_bus, sdio_upld_callback);
	sdio_set_reset_callback(sdio_bus, sdio_reset_callback);

	if (!init_dnld_buffers()) {
		LOG_ERR("Failed to initialize download buffers");
		return -1;
	}

	k_work_init(&ctx.open_work, sdio_open_work_handler);

	if (RCP_BUS_OK != rcp_bus_register(ctx.inst, &sdio_slave_api)) {
		LOG_ERR("Failed to register RCP bus SDIO API");
		return -1;
	}
	LOG_INF("SDIO slave initialized");
	return 0;
}

DEVICE_DT_DEFINE(DT_DRV_INST(0), sdio_slave_init, NULL, NULL, NULL, POST_KERNEL, 80, NULL);
