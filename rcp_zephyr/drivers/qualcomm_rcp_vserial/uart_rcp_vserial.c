/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <buffer_mgmt.h>
#include <rcp_config.h>
#include <rcp_definitions.h>
#include <rcp_interface.h>

#define DT_DRV_COMPAT qcom_rcp_vserial_uart

LOG_MODULE_REGISTER(rcp_vserial_uart, CONFIG_QUALCOMM_RCP_VSERIAL_UART_LOG_LEVEL);

#define RCP_VSERIAL_MAX_PORTS 4U
#define RCP_VSERIAL_STREAM_FROM_PORT(port)                                                   \
	((port) == 0U ? RCP_STREAM_VSERIAL0 :                                                \
	 ((port) == 1U ? RCP_STREAM_VSERIAL1 :                                             \
	  ((port) == 2U ? RCP_STREAM_VSERIAL2 : RCP_STREAM_VSERIAL3)))

struct rcp_vserial_uart_cfg {
	uint8_t port;
	uint8_t stream;
};

struct rcp_vserial_uart_data {
	const struct device *dev;
	struct uart_config cfg;
	struct ring_buf rx_ring;
	struct k_spinlock lock;
	bool rx_irq_enabled;
	bool tx_irq_enabled;
	uart_irq_callback_user_data_t irq_cb;
	void *irq_cb_user_data;
	struct k_work irq_work;
	uint8_t rx_storage[CONFIG_QUALCOMM_RCP_VSERIAL_RX_BUFFER_SIZE];
};

static const uint8_t vserial_streams[RCP_VSERIAL_MAX_PORTS] = {
	RCP_STREAM_VSERIAL0,
	RCP_STREAM_VSERIAL1,
	RCP_STREAM_VSERIAL2,
	RCP_STREAM_VSERIAL3,
};

static const struct device *vserial_devices[RCP_VSERIAL_MAX_PORTS];
static bool vserial_stream_handler_registered[RCP_VSERIAL_MAX_PORTS];

static const struct uart_config default_cfg = {
	.baudrate = 115200,
	.parity = UART_CFG_PARITY_NONE,
	.stop_bits = UART_CFG_STOP_BITS_1,
	.data_bits = UART_CFG_DATA_BITS_8,
	.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
};

static inline bool rcp_vserial_port_enabled(uint8_t port)
{
	return (port < RCP_VSERIAL_MAX_PORTS) && (vserial_devices[port] != NULL);
}

static int rcp_vserial_get_ctx(const struct device *dev,
			       const struct rcp_vserial_uart_cfg **cfg,
			       struct rcp_vserial_uart_data **data)
{
	const struct rcp_vserial_uart_cfg *dev_cfg;

	if ((dev == NULL) || (dev->config == NULL) || (dev->data == NULL)) {
		return -EINVAL;
	}

	dev_cfg = dev->config;
	if (!rcp_vserial_port_enabled(dev_cfg->port)) {
		return -ENODEV;
	}

	if (cfg != NULL) {
		*cfg = dev_cfg;
	}
	if (data != NULL) {
		*data = dev->data;
	}

	return 0;
}

static void rcp_vserial_submit_irq_work(struct rcp_vserial_uart_data *data)
{
	uart_irq_callback_user_data_t cb = NULL;

	K_SPINLOCK(&data->lock) {
		cb = data->irq_cb;
	}

	if (cb != NULL) {
		(void)k_work_submit(&data->irq_work);
	}
}

static size_t rcp_vserial_max_tx_chunk(void)
{
	size_t chunk_size = CONFIG_QUALCOMM_RCP_VSERIAL_TX_CHUNK_SIZE;

	if (chunk_size > RCP_BUF_SIZE) {
		chunk_size = RCP_BUF_SIZE;
	}
	return chunk_size;
}

static bool rcp_vserial_can_drop_on_error(link_result_t result)
{
	return (result == RCP_LNK_ERR_TX_NO_RESOURCES) || (result == RCP_LINK_SYNCING) ||
	       (result == RCP_LNK_TIMEOUT);
}

static int rcp_vserial_send_data(uint8_t stream, const uint8_t *data, size_t len,
				 bool drop_when_busy)
{
	size_t tx_total = 0;
	size_t max_chunk = rcp_vserial_max_tx_chunk();

	if ((data == NULL) || (len == 0U)) {
		return 0;
	}

	if (max_chunk == 0U) {
		return -EINVAL;
	}

	while (tx_total < len) {
		size_t tx_left = len - tx_total;
		size_t chunk = MIN(tx_left, max_chunk);
		rcp_buffer_hdl_t *msg_buffer = get_buffer((uint16_t)chunk);
		link_result_t result;

		if (msg_buffer == NULL) {
			if (drop_when_busy) {
				return tx_total > 0U ? (int)tx_total : -EAGAIN;
			}
			return tx_total > 0U ? (int)tx_total : -ENOMEM;
		}

		if (!write_to_buffer(msg_buffer, (uint8_t *)&data[tx_total], (uint16_t)chunk)) {
			release_buffer(&msg_buffer);
			return tx_total > 0U ? (int)tx_total : -EIO;
		}

		result = link_send(stream, msg_buffer);
		if (result != RCP_LNK_SUCCESS) {
			release_buffer(&msg_buffer);
			if (drop_when_busy && rcp_vserial_can_drop_on_error(result)) {
				return tx_total > 0U ? (int)tx_total : -EAGAIN;
			}
			return tx_total > 0U ? (int)tx_total : -EIO;
		}

		tx_total += chunk;
	}

	return (int)tx_total;
}

int rcp_vserial_uart_write_port(uint8_t port, const uint8_t *data, size_t len, bool drop_when_busy)
{
	if ((data == NULL) || (len == 0U)) {
		return 0;
	}

	if ((port >= RCP_VSERIAL_MAX_PORTS) || !rcp_vserial_port_enabled(port)) {
		return -ENODEV;
	}

	return rcp_vserial_send_data(vserial_streams[port], data, len, drop_when_busy);
}

static void rcp_vserial_irq_work_handler(struct k_work *work)
{
	struct rcp_vserial_uart_data *data =
		CONTAINER_OF(work, struct rcp_vserial_uart_data, irq_work);
	uart_irq_callback_user_data_t cb = NULL;
	void *user_data = NULL;

	K_SPINLOCK(&data->lock) {
		cb = data->irq_cb;
		user_data = data->irq_cb_user_data;
	}

	if (cb != NULL) {
		cb(data->dev, user_data);
	}
}

static int rcp_vserial_uart_poll_in(const struct device *dev, unsigned char *p_char)
{
	struct rcp_vserial_uart_data *data;
	uint32_t read_count = 0;
	int err;

	if (p_char == NULL) {
		return -EINVAL;
	}

	err = rcp_vserial_get_ctx(dev, NULL, &data);
	if (err != 0) {
		return err;
	}

	K_SPINLOCK(&data->lock) {
		read_count = ring_buf_get(&data->rx_ring, p_char, 1U);
	}

	return (read_count == 1U) ? 0 : -1;
}

static void rcp_vserial_uart_poll_out(const struct device *dev, unsigned char out_char)
{
	const struct rcp_vserial_uart_cfg *cfg;
	int err = rcp_vserial_get_ctx(dev, &cfg, NULL);

	if (err != 0) {
		return;
	}

	(void)rcp_vserial_send_data(cfg->stream, &out_char, 1U, false);
}

static int rcp_vserial_uart_err_check(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
static int rcp_vserial_uart_configure(const struct device *dev, const struct uart_config *cfg)
{
	struct rcp_vserial_uart_data *data;
	int err;

	if (cfg == NULL) {
		return -EINVAL;
	}

	err = rcp_vserial_get_ctx(dev, NULL, &data);
	if (err != 0) {
		return err;
	}

	K_SPINLOCK(&data->lock) {
		memcpy(&data->cfg, cfg, sizeof(data->cfg));
	}

	return 0;
}

static int rcp_vserial_uart_config_get(const struct device *dev, struct uart_config *cfg)
{
	struct rcp_vserial_uart_data *data;
	int err;

	if (cfg == NULL) {
		return -EINVAL;
	}

	err = rcp_vserial_get_ctx(dev, NULL, &data);
	if (err != 0) {
		return err;
	}

	K_SPINLOCK(&data->lock) {
		memcpy(cfg, &data->cfg, sizeof(*cfg));
	}

	return 0;
}
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static int rcp_vserial_uart_fifo_fill(const struct device *dev, const uint8_t *tx_data, int size)
{
	const struct rcp_vserial_uart_cfg *cfg;
	int err;

	if ((tx_data == NULL) || (size <= 0)) {
		return 0;
	}

	err = rcp_vserial_get_ctx(dev, &cfg, NULL);
	if (err != 0) {
		return err;
	}

	return rcp_vserial_send_data(cfg->stream, tx_data, (size_t)size, false);
}

static int rcp_vserial_uart_fifo_read(const struct device *dev, uint8_t *rx_data, const int size)
{
	struct rcp_vserial_uart_data *data;
	uint32_t read_count = 0;
	int err;

	if ((rx_data == NULL) || (size <= 0)) {
		return 0;
	}

	err = rcp_vserial_get_ctx(dev, NULL, &data);
	if (err != 0) {
		return err;
	}

	K_SPINLOCK(&data->lock) {
		read_count = ring_buf_get(&data->rx_ring, rx_data, (uint32_t)size);
	}

	return (int)read_count;
}

static void rcp_vserial_uart_irq_tx_enable(const struct device *dev)
{
	struct rcp_vserial_uart_data *data;

	if (rcp_vserial_get_ctx(dev, NULL, &data) != 0) {
		return;
	}

	K_SPINLOCK(&data->lock) {
		data->tx_irq_enabled = true;
	}

	rcp_vserial_submit_irq_work(data);
}

static void rcp_vserial_uart_irq_tx_disable(const struct device *dev)
{
	struct rcp_vserial_uart_data *data;

	if (rcp_vserial_get_ctx(dev, NULL, &data) != 0) {
		return;
	}

	K_SPINLOCK(&data->lock) {
		data->tx_irq_enabled = false;
	}
}

static int rcp_vserial_uart_irq_tx_ready(const struct device *dev)
{
	struct rcp_vserial_uart_data *data;
	bool tx_ready = false;

	if (rcp_vserial_get_ctx(dev, NULL, &data) != 0) {
		return 0;
	}

	K_SPINLOCK(&data->lock) {
		tx_ready = data->tx_irq_enabled;
	}

	return tx_ready ? 1 : 0;
}

static int rcp_vserial_uart_irq_tx_complete(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 1;
}

static void rcp_vserial_uart_irq_rx_enable(const struct device *dev)
{
	struct rcp_vserial_uart_data *data;
	bool has_rx_data = false;

	if (rcp_vserial_get_ctx(dev, NULL, &data) != 0) {
		return;
	}

	K_SPINLOCK(&data->lock) {
		data->rx_irq_enabled = true;
		has_rx_data = !ring_buf_is_empty(&data->rx_ring);
	}

	if (has_rx_data) {
		rcp_vserial_submit_irq_work(data);
	}
}

static void rcp_vserial_uart_irq_rx_disable(const struct device *dev)
{
	struct rcp_vserial_uart_data *data;

	if (rcp_vserial_get_ctx(dev, NULL, &data) != 0) {
		return;
	}

	K_SPINLOCK(&data->lock) {
		data->rx_irq_enabled = false;
	}
}

static int rcp_vserial_uart_irq_rx_ready(const struct device *dev)
{
	struct rcp_vserial_uart_data *data;
	bool rx_ready = false;

	if (rcp_vserial_get_ctx(dev, NULL, &data) != 0) {
		return 0;
	}

	K_SPINLOCK(&data->lock) {
		rx_ready = data->rx_irq_enabled && !ring_buf_is_empty(&data->rx_ring);
	}

	return rx_ready ? 1 : 0;
}

static int rcp_vserial_uart_irq_is_pending(const struct device *dev)
{
	return rcp_vserial_uart_irq_tx_ready(dev) || rcp_vserial_uart_irq_rx_ready(dev);
}

static int rcp_vserial_uart_irq_update(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 1;
}

static void rcp_vserial_uart_irq_callback_set(const struct device *dev,
					      uart_irq_callback_user_data_t cb,
					      void *user_data)
{
	struct rcp_vserial_uart_data *data;

	if (rcp_vserial_get_ctx(dev, NULL, &data) != 0) {
		return;
	}

	K_SPINLOCK(&data->lock) {
		data->irq_cb = cb;
		data->irq_cb_user_data = user_data;
	}
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

static int rcp_vserial_register_rx_handler(uint8_t port);

static int rcp_vserial_uart_dev_init(const struct device *dev)
{
	const struct rcp_vserial_uart_cfg *cfg = dev->config;
	struct rcp_vserial_uart_data *data = dev->data;
	int err;

	if ((cfg == NULL) || (cfg->port >= RCP_VSERIAL_MAX_PORTS)) {
		return -EINVAL;
	}

	data->dev = dev;
	memcpy(&data->cfg, &default_cfg, sizeof(default_cfg));
	ring_buf_init(&data->rx_ring, sizeof(data->rx_storage), data->rx_storage);
	k_work_init(&data->irq_work, rcp_vserial_irq_work_handler);

	if ((vserial_devices[cfg->port] != NULL) && (vserial_devices[cfg->port] != dev)) {
		LOG_ERR("Duplicate VSerial port %u in devicetree", cfg->port);
		return -EINVAL;
	}
	vserial_devices[cfg->port] = dev;

	err = rcp_vserial_register_rx_handler(cfg->port);
	if (err != 0) {
		vserial_devices[cfg->port] = NULL;
		return err;
	}

	return 0;
}

static uint8_t rcp_vserial_rx_handler_common(uint8_t port, enum rcp_rx_status status,
				     rcp_buffer_hdl_t *payload_ptr, rcp_error_info_t *err_info_ptr)
{
	struct rcp_vserial_uart_data *data;
	uint32_t written = 0;
	bool notify = false;

	ARG_UNUSED(status);
	ARG_UNUSED(err_info_ptr);

	if ((payload_ptr == NULL) || (payload_ptr->data_ptr == NULL) || (payload_ptr->size == 0U)) {
		return RCP_RX_ACK;
	}

	if ((port >= RCP_VSERIAL_MAX_PORTS) || !rcp_vserial_port_enabled(port) ||
	    (vserial_devices[port] == NULL)) {
		return RCP_RX_ACK;
	}

	data = vserial_devices[port]->data;

	K_SPINLOCK(&data->lock) {
		written = ring_buf_put(&data->rx_ring, payload_ptr->data_ptr, payload_ptr->size);
		notify = (written > 0U) && data->rx_irq_enabled && (data->irq_cb != NULL);
	}

	if (written < payload_ptr->size) {
		LOG_WRN("Dropped %u RX bytes on VSerial port %u",
			(unsigned int)(payload_ptr->size - written), port);
	}

	if (notify) {
		(void)k_work_submit(&data->irq_work);
	}

	return RCP_RX_ACK;
}

static uint8_t rcp_vserial_rx_handler_0(enum rcp_rx_status status, rcp_buffer_hdl_t *payload_ptr, rcp_error_info_t *err_info_ptr)
{
	return rcp_vserial_rx_handler_common(0U, status, payload_ptr, err_info_ptr);
}

static uint8_t rcp_vserial_rx_handler_1(enum rcp_rx_status status, rcp_buffer_hdl_t *payload_ptr, rcp_error_info_t *err_info_ptr)
{
	return rcp_vserial_rx_handler_common(1U, status, payload_ptr, err_info_ptr);
}

static uint8_t rcp_vserial_rx_handler_2(enum rcp_rx_status status, rcp_buffer_hdl_t *payload_ptr, rcp_error_info_t *err_info_ptr)
{
	return rcp_vserial_rx_handler_common(2U, status, payload_ptr, err_info_ptr);
}

static uint8_t rcp_vserial_rx_handler_3(enum rcp_rx_status status, rcp_buffer_hdl_t *payload_ptr, rcp_error_info_t *err_info_ptr)
{
	return rcp_vserial_rx_handler_common(3U, status, payload_ptr, err_info_ptr);
}

static rcp_rx_event_handler_t const rcp_vserial_rx_handlers[RCP_VSERIAL_MAX_PORTS] = {
	rcp_vserial_rx_handler_0,
	rcp_vserial_rx_handler_1,
	rcp_vserial_rx_handler_2,
	rcp_vserial_rx_handler_3,
};

static int rcp_vserial_register_rx_handler(uint8_t port)
{
	link_result_t result;

	if (port >= RCP_VSERIAL_MAX_PORTS) {
		return -EINVAL;
	}

	if (vserial_stream_handler_registered[port]) {
		return 0;
	}

	result = link_register_rx_handler(vserial_streams[port], rcp_vserial_rx_handlers[port]);
	if (result != RCP_LNK_SUCCESS) {
		LOG_ERR("Failed to register RX handler for VSerial%u stream=0x%02x", port,
			vserial_streams[port]);
		return -EIO;
	}

	vserial_stream_handler_registered[port] = true;
	LOG_INF("VSerial%u enabled on stream=0x%02x", port, vserial_streams[port]);
	return 0;
}

static DEVICE_API(uart, rcp_vserial_uart_api) = {
	.poll_in = rcp_vserial_uart_poll_in,
	.poll_out = rcp_vserial_uart_poll_out,
	.err_check = rcp_vserial_uart_err_check,
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	.configure = rcp_vserial_uart_configure,
	.config_get = rcp_vserial_uart_config_get,
#endif
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.fifo_fill = rcp_vserial_uart_fifo_fill,
	.fifo_read = rcp_vserial_uart_fifo_read,
	.irq_tx_enable = rcp_vserial_uart_irq_tx_enable,
	.irq_tx_disable = rcp_vserial_uart_irq_tx_disable,
	.irq_tx_ready = rcp_vserial_uart_irq_tx_ready,
	.irq_tx_complete = rcp_vserial_uart_irq_tx_complete,
	.irq_rx_enable = rcp_vserial_uart_irq_rx_enable,
	.irq_rx_disable = rcp_vserial_uart_irq_rx_disable,
	.irq_rx_ready = rcp_vserial_uart_irq_rx_ready,
	.irq_callback_set = rcp_vserial_uart_irq_callback_set,
	.irq_update = rcp_vserial_uart_irq_update,
	.irq_is_pending = rcp_vserial_uart_irq_is_pending,
#endif
};

#define RCP_VSERIAL_UART_DEVICE_DT_INST_DEFINE(inst)                                              \
	BUILD_ASSERT(DT_INST_PROP(inst, port) < RCP_VSERIAL_MAX_PORTS,                           \
		     "VSerial port is out of supported range (0..3)");                           \
	static const struct rcp_vserial_uart_cfg rcp_vserial_uart_cfg_##inst = {                  \
		.port = DT_INST_PROP(inst, port),                                                  \
		.stream = RCP_VSERIAL_STREAM_FROM_PORT(DT_INST_PROP(inst, port)),                  \
	};                                                                                        \
	static struct rcp_vserial_uart_data rcp_vserial_uart_data_##inst;                         \
	DEVICE_DT_INST_DEFINE(inst, rcp_vserial_uart_dev_init, NULL,                              \
			      &rcp_vserial_uart_data_##inst, &rcp_vserial_uart_cfg_##inst,          \
			      POST_KERNEL, CONFIG_QUALCOMM_RCP_VSERIAL_INIT_PRIORITY, \
			      &rcp_vserial_uart_api)

DT_INST_FOREACH_STATUS_OKAY(RCP_VSERIAL_UART_DEVICE_DT_INST_DEFINE)
