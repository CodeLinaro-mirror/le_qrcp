/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * QCC730 RCP bus driver - QcSPI HOST_INT0/HOST_INT1 interrupt model
 * 8-slot bidirectional ring buffers in SRAM (.rcp_shmem section).
 *
 * HOST_INT0 fires when RPi writes a slot into m2s ring.
 * HOST_INT1 fires when RPi reads a slot from s2m ring.
 * IRQ GPIO (GPIO8) signals to RPi that s2m ring has data.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <string.h>

#include <rcp_bus.h>
#include <rcp_event.h>

LOG_MODULE_REGISTER(rcp_bus_qcspi, CONFIG_QUALCOMM_RCP_BUS_LOG_LEVEL);

#define DT_DRV_COMPAT qcom_rcp_bus_qcspi

#define RCP_SHMEM_SLOTS    8
#define RCP_SHMEM_BUF_SIZE 1596  /* 4 bytes for len + 1596 buf = 1600 per slot, buf is 4-byte aligned */

struct rcp_qcspi_slot {
	volatile uint32_t len;            /* 4 bytes: ensures buf[] is 4-byte aligned for QcSPI */
	volatile uint8_t  buf[RCP_SHMEM_BUF_SIZE];
};

struct rcp_qcspi_ring {
	volatile uint8_t wr_idx;
	volatile uint8_t rd_idx;
	volatile uint8_t _pad[2];
	struct rcp_qcspi_slot slot[RCP_SHMEM_SLOTS];
};

struct rcp_qcspi_shmem {
	struct rcp_qcspi_ring m2s;  /* RPi -> QCC730 */
	struct rcp_qcspi_ring s2m;  /* QCC730 -> RPi */
};

__attribute__((section(".rcp_shmem")))
static struct rcp_qcspi_shmem g_rcp_shmem;

static const struct gpio_dt_spec irq_gpio =
	GPIO_DT_SPEC_INST_GET(0, irq_gpios);

/* Set to true only after qcc730_qcspi_open() → rcp_events_init() has run.
 * Guards ring_rx_handler / ring_all_tx_consumed against being called from
 * spi_qcc730_spis_isr while the RCP event subsystem is still uninitialized
 * (e.g. when the host sends SPI commands during QCC730 early boot). */
static bool g_rcp_bus_open;

/*
 * s2m ring wakeup semaphore (QCC730 → RPi).
 *
 * NOT a free-slot counter. The single source of truth for whether a slot is
 * free is slot[i].len in shared memory: len==0 means the host has consumed
 * (and zeroed) the slot, len!=0 means it still holds unread data. This sem is
 * used purely to sleep the producer when the ring is full and wake it when the
 * host frees a slot (INT1 -> k_sem_give). A missed/coalesced INT1 is harmless:
 * the write path re-checks slot.len on a short timeout and self-heals. Init
 * count 0 (semaphores can't be "given" ahead); the timeout is the backstop.
 */
static struct k_sem s2m_space_sem;

/*
 * Called from spi_qcc730_spis_isr() when HOST_INT0 fires.
 * Runs in ISR context - only set flags.
 */
void ring_rx_handler(void)
{
	if (!g_rcp_bus_open)
		return;

	uint8_t wr = g_rcp_shmem.m2s.wr_idx;

	/* Guard against the QcSPI hardware double-fire quirk: one host IRW
	 * generates two INT0 pulses.  The second ISR invocation runs after
	 * the first has already incremented wr_idx, so slot[wr] will be empty.
	 * Only advance wr_idx if the current slot actually has data. */
	if (g_rcp_shmem.m2s.slot[wr].len == 0) {
		LOG_DBG("ring_rx_handler: spurious (slot[%u] empty)", wr);
		return;
	}

	g_rcp_shmem.m2s.wr_idx = (wr + 1) % RCP_SHMEM_SLOTS;
	rcp_events_set(RCP_EVENT_RX_DATA);
}

/*
 * Called from spi_qcc730_spis_isr() when HOST_INT1 fires.
 * Runs in ISR context - only set flags + GPIO.
 */
bool ring_all_tx_consumed(void)
{
	if (!g_rcp_bus_open)
		return true;

	/* Wake a producer that may be blocked in qcc730_qcspi_write (it re-reads
	 * slot.len to decide). The host owns slot release: it zeroes slot[rd].len
	 * when it consumes a slot, which is the sole "slot free" signal. We do NOT
	 * advance rd_idx or clear len here: inferring host progress by counting
	 * INT1 edges is exactly what desynced under edge coalescing at high rate. */
	k_sem_give(&s2m_space_sem);

	/* Re-arm TX_RDY so the shared event loop wakes and retries the write. */
	rcp_events_set(RCP_EVENT_TX_RDY);

	/* True only when every s2m slot is free (len==0) — the host has drained
	 * the whole ring, so there is no in-flight TX and the caller may drop the
	 * PM-busy hold. slot.len is the single source of truth for slot state. */
	for (int i = 0; i < RCP_SHMEM_SLOTS; i++) {
		if (g_rcp_shmem.s2m.slot[i].len != 0)
			return false;
	}
	return true;
}

static enum rcp_bus_status qcc730_qcspi_open(void);
static enum rcp_bus_status qcc730_qcspi_close(void);
static enum rcp_bus_status qcc730_qcspi_write(uint8_t *tx, uint16_t len);
static enum rcp_bus_status qcc730_qcspi_read(uint8_t *rx, uint16_t len,
					     uint16_t *nread);
static uint16_t            qcc730_qcspi_get_rx_len(void);
static enum rcp_bus_status qcc730_qcspi_get_caps(struct rcp_bus_caps *caps);

static struct rcp_bus_api qcspi_api = {
	.open       = qcc730_qcspi_open,
	.close      = qcc730_qcspi_close,
	.write      = qcc730_qcspi_write,
	.read       = qcc730_qcspi_read,
	.get_rx_len = qcc730_qcspi_get_rx_len,
	.get_caps   = qcc730_qcspi_get_caps,
};

static enum rcp_bus_status qcc730_qcspi_open(void)
{
	int ret;

	if (!device_is_ready(irq_gpio.port)) {
		LOG_ERR("IRQ GPIO not ready");
		return RCP_BUS_ERR_IO;
	}

	ret = gpio_pin_configure_dt(&irq_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("IRQ GPIO configure failed: %d", ret);
		return RCP_BUS_ERR_IO;
	}

	memset(&g_rcp_shmem, 0, sizeof(g_rcp_shmem));

	/* Wake-only sem (see decl): starts at 0, INT1 gives, write path takes with
	 * a timeout and re-checks slot.len. Not a free-slot counter. */
	k_sem_init(&s2m_space_sem, 0, RCP_SHMEM_SLOTS);

	rcp_events_set(RCP_EVENT_TX_RDY);
	g_rcp_bus_open = true;
	LOG_INF("QCC730 QcSPI RCP bus open, shmem=%p", &g_rcp_shmem);
	return RCP_BUS_OK;
}

static enum rcp_bus_status qcc730_qcspi_close(void)
{
	g_rcp_bus_open = false;
	gpio_pin_set_dt(&irq_gpio, 0);
	return RCP_BUS_OK;
}

/* Total time to wait for the host to free slot[wr] before dropping the frame.
 * Polled in short sem-waits so a coalesced/missed INT1 self-heals on timeout.
 * On drop, BUS_TX_RETRIES in the event loop retries the message. */
#define S2M_SLOT_WAIT_MS   5
#define S2M_POLL_SLICE     K_USEC(500)

static enum rcp_bus_status qcc730_qcspi_write(uint8_t *tx, uint16_t len)
{
	uint8_t wr = g_rcp_shmem.s2m.wr_idx;

	/* Single source of truth: slot[wr].len == 0 means the host has consumed
	 * and released this slot. Sleep on the wake sem in short slices and
	 * re-check; the timeout backstops a missed/coalesced INT1 so we never
	 * deadlock waiting for a wake that was folded into a prior edge. */
	uint32_t waited_us = 0;

	while (g_rcp_shmem.s2m.slot[wr].len != 0) {
		(void)k_sem_take(&s2m_space_sem, S2M_POLL_SLICE);
		waited_us += 500U;
		if (waited_us >= (S2M_SLOT_WAIT_MS * 1000U)) {
			LOG_WRN("TX DROP: slot[%u] still busy after %ums",
				wr, S2M_SLOT_WAIT_MS);
			return RCP_BUS_ERR_PEER_MEMORY;
		}
	}

	memcpy((void *)g_rcp_shmem.s2m.slot[wr].buf, tx, len);
	barrier_dmem_fence_full();
	g_rcp_shmem.s2m.slot[wr].len = len;
	g_rcp_shmem.s2m.wr_idx = (wr + 1) % RCP_SHMEM_SLOTS;

#if defined(CONFIG_PM_DEVICE) && defined(CONFIG_SPI)
    /* Prevent BMPS sleep until host reads the data */
    const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(qcspi));
    if (device_is_ready(spi_dev))
        pm_device_busy_set(spi_dev);
#endif

	/* Toggle LOW→HIGH to guarantee a rising edge notifying the host. */
	gpio_pin_set_dt(&irq_gpio, 0);
	k_busy_wait(2);
	gpio_pin_set_dt(&irq_gpio, 1);

	return RCP_BUS_OK;
}

static enum rcp_bus_status qcc730_qcspi_read(uint8_t *rx, uint16_t len,
					     uint16_t *nread)
{
	uint8_t rd = g_rcp_shmem.m2s.rd_idx;
	uint16_t slot_len = g_rcp_shmem.m2s.slot[rd].len;

	if (slot_len == 0) {
		*nread = 0;
		rcp_events_clear(RCP_EVENT_RX_DATA);
		return RCP_BUS_ERR_EMPTY;
	}

	uint16_t copy_len = (slot_len < len) ? slot_len : len;
	memcpy(rx, (void *)g_rcp_shmem.m2s.slot[rd].buf, copy_len);
	*nread = copy_len;

	g_rcp_shmem.m2s.slot[rd].len = 0;
	g_rcp_shmem.m2s.rd_idx = (rd + 1) % RCP_SHMEM_SLOTS;

	return RCP_BUS_OK;
}

static uint16_t qcc730_qcspi_get_rx_len(void)
{
	uint8_t rd = g_rcp_shmem.m2s.rd_idx;
	if (g_rcp_shmem.m2s.slot[rd].len == 0)
		rcp_events_clear(RCP_EVENT_RX_DATA);
	return g_rcp_shmem.m2s.slot[rd].len;
}

static enum rcp_bus_status qcc730_qcspi_get_caps(struct rcp_bus_caps *caps)
{
	caps->mtu = RCP_SHMEM_BUF_SIZE;
	return RCP_BUS_OK;
}

static int rcp_bus_qcspi_sys_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	/* Only register the bus API here. The full open() (GPIO setup,
	 * shmem init, rcp_events_set) is called later by init_rcp() via
	 * activate_bus() after rcp_events_init() has run. */
	return rcp_bus_register(RCP_BUS_SPI, &qcspi_api) == RCP_BUS_OK ? 0 : -EIO;
}

DEVICE_DT_INST_DEFINE(0, rcp_bus_qcspi_sys_init, NULL, NULL, NULL,
		      POST_KERNEL, 56, NULL);

