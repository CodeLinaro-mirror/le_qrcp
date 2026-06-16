/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * Linux QcSPI master bus driver for QCC730 RCP.
 *
 * Low-level layer:
 *   - Pre-allocated DMA-safe TX/RX buffers in driver state struct
 *   - spi_lock mutex protecting shared buffers
 *   - Half-duplex write_then_read pattern for all commands
 *
 * High-level layer (ring protocol) matches rcp_bus_spi_slave_qcc730.c:
 *   - Fixed SHMEM_BASE layout with 8-slot m2s / s2m rings
 *   - HOST_INT0 doorbell: m2s slot committed (RPi -> QCC730)
 *   - HOST_INT1 doorbell: s2m slot consumed  (QCC730 -> RPi)
 *   - GPIO8 rising edge IRQ: QCC730 signals s2m data ready
 *
 * Write sequence (m2s: RPi -> QCC730):
 *   WRITE slot[wr].buf  -> RDSR poll
 *   WRITE slot[wr].len  -> RDSR poll  (commit)
 *   IRR CONFIG + set HOST_INT0 + IRW + clear + IRW  (doorbell)
 *
 * Read sequence (s2m: QCC730 -> RPi):
 *   FREAD slot[rd].len  (4B, take low 16 bits)
 *   FREAD [len_addr .. len+buf] padded (single transaction)
 *   IRR CONFIG + set HOST_INT1 + IRW + clear + IRW  (doorbell)
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/types.h>

#include "rcp_user_config.h"
#include "rcp_bus_spi_master_linux_qcc730.h"

/* =========================================================================
 * QcSPI command codes (from Fermion qcc730.h)
 * ========================================================================= */
#define QCSPI_CMD_NOP       0x00
#define QCSPI_CMD_WREN      0x06  /* Write Enable (required before SRAM writes) */
#define QCSPI_CMD_FREAD     0x0B  /* Fast Read from AHB address */
#define QCSPI_CMD_WRITE     0x02  /* Write to AHB address */
#define QCSPI_CMD_IRW       0x82  /* Internal Register Write: [cmd][reg][val_be4] */
#define QCSPI_CMD_IRR       0x81  /* Internal Register Read:  [cmd][reg][2 dummy] -> 4B */
#define QCSPI_CMD_RDSR      0x05  /* Read Status Register: [cmd] -> 4B big-endian */

/* QcSPI internal register addresses */
#define SPI_SLAVE_TRNS_LEN  0x50  /* Transfer length (bits [31:16]) */
#define SPI_SLAVE_CONFIG    0x0C  /* Config / HOST_INT bits */

/* SPI_SLAVE_CONFIG interrupt bits */
#define HOST_INT0_BIT       BIT(5)  /* m2s: RPi committed a slot */
#define HOST_INT1_BIT       BIT(6)  /* s2m: RPi consumed a slot  */

/* RDSR busy bit (bit 24 = bit 0 of first response byte) */
#define RDSR_BUSY_BIT       BIT(24)
#define RDSR_BUSY_RETRIES   50
#define RDSR_BUSY_DELAY_US  10

/* FREAD dummy byte count (from Fermion: QCC730_SPI_CMDCODE_FREAD_DUMMY_BYTES) */
#define QCSPI_FREAD_DUMMY   10

/* QCC730 hard reset via GPIO (active low). Mirrors the FreeRTOS host demo
 * qcc730_reset(): pulse reset then retry until the chip answers on QcSPI.
 * Two nested loops: the outer re-pulses reset (handles "one reset had no
 * effect, needs a second"), the inner polls NOP/RDSR for a slow boot. */
#define QCC730_RESET_ASSERT_MS   300  /* hold reset asserted (low) */
#define QCC730_RESET_RELEASE_MS  300  /* settle after release (high) */
#define QCC730_BOOT_WAIT_MS      2000 /* wait for cold boot before probing */
#define QCC730_RESET_MAX_RETRIES 10   /* outer: max reset pulses */
#define QCC730_PROBE_POLL_TRIES  5    /* inner: probe polls per boot */
#define QCC730_PROBE_POLL_MS     100  /* inner: delay between probe polls */

/* Pre-allocated buffer sizes (from Fermion: QCC730_SPI_MAX_TX/RX_BUFF_LEN) */
#define SPI_TX_BUF_LEN      2048
#define SPI_RX_BUF_LEN      2048

/* =========================================================================
 * Shared memory layout (must match rcp_bus_spi_slave_qcc730.c)
 * ========================================================================= */
#define SHMEM_BASE          0x99800U
#define RCP_SHMEM_SLOTS     8
#define RCP_SHMEM_BUF_SIZE  1596  /* 4B len + 1596B buf = 1600B per slot */

struct rcp_qcspi_slot {
	uint32_t len;
	uint8_t  buf[RCP_SHMEM_BUF_SIZE];
} __packed;

struct rcp_qcspi_ring {
	uint8_t wr_idx;
	uint8_t rd_idx;
	uint8_t _pad[2];
	struct rcp_qcspi_slot slot[RCP_SHMEM_SLOTS];
} __packed;

struct rcp_qcspi_shmem_layout {
	struct rcp_qcspi_ring m2s;
	struct rcp_qcspi_ring s2m;
} __packed;

#define SHMEM_M2S_BASE \
	(SHMEM_BASE + offsetof(struct rcp_qcspi_shmem_layout, m2s))
#define SHMEM_S2M_BASE \
	(SHMEM_BASE + offsetof(struct rcp_qcspi_shmem_layout, s2m))

#define SHMEM_M2S_SLOT_BUF_ADDR(i) \
	(SHMEM_BASE \
	 + offsetof(struct rcp_qcspi_shmem_layout, m2s) \
	 + offsetof(struct rcp_qcspi_ring, slot[(i)].buf))

#define SHMEM_M2S_SLOT_LEN_ADDR(i) \
	(SHMEM_BASE \
	 + offsetof(struct rcp_qcspi_shmem_layout, m2s) \
	 + offsetof(struct rcp_qcspi_ring, slot[(i)].len))

#define SHMEM_S2M_SLOT_LEN_ADDR(i) \
	(SHMEM_BASE \
	 + offsetof(struct rcp_qcspi_shmem_layout, s2m) \
	 + offsetof(struct rcp_qcspi_ring, slot[(i)].len))

#define SHMEM_S2M_SLOT_BUF_ADDR(i) \
	(SHMEM_BASE \
	 + offsetof(struct rcp_qcspi_shmem_layout, s2m) \
	 + offsetof(struct rcp_qcspi_ring, slot[(i)].buf))

/* =========================================================================
 * Driver state
 * ========================================================================= */
struct rcp_spi_master {
	struct spi_device  *spi;
	struct gpio_desc   *irq_gpio;
	struct gpio_desc   *reset_gpio;   /* QCC730 hard reset, active low */
	int                 irq;
	struct mutex        spi_lock;
	/* Pre-allocated DMA-safe SPI buffers (Fermion pattern) */
	u8                  spi_tx_buf[SPI_TX_BUF_LEN];
	u8                  spi_rx_buf[SPI_RX_BUF_LEN];
	/* Ring tracking (local cached indices) */
	u8                  m2s_wr_idx;
	u8                  m2s_rd_idx_cached; /* lazy cache of QCC730's m2s.rd_idx */
	u8                  s2m_rd_idx;
	atomic_t            rx_pending;
	bool                irq_registered;
	/* Cached SPI_SLAVE_CONFIG value (read once at open, INT bits clear).
	 * Doorbells write this value + desired INT bit in a single IRW,
	 * avoiding the IRR that would trigger a spurious second interrupt. */
	u32                 slave_config_cache;
};

static struct rcp_spi_master g_spi_master;

/* Forward declarations for rcp_bus_api */
static enum rcp_bus_status spi_master_open(void);
static enum rcp_bus_status spi_master_close(void);
static enum rcp_bus_status spi_master_write(uint8_t *tx, uint16_t len);
static enum rcp_bus_status spi_master_read(uint8_t *rx, uint16_t len,
					   uint16_t *nread);
static uint16_t            spi_master_get_rx_len(void);
static enum rcp_bus_status spi_master_get_caps(struct rcp_bus_caps *caps);

static struct rcp_bus_api spi_master_api = {
	.open       = spi_master_open,
	.close      = spi_master_close,
	.write      = spi_master_write,
	.read       = spi_master_read,
	.get_rx_len = spi_master_get_rx_len,
	.get_caps   = spi_master_get_caps,
};

/* =========================================================================
 * Low-level QcSPI transport
 *
 * All functions require priv->spi_lock to be held by the caller.
 * All use priv->spi_tx_buf (TX) and priv->spi_rx_buf (RX).
 * ========================================================================= */

/*
 * qcspi_xfer - send n_tx bytes from spi_tx_buf, receive n_rx into spi_rx_buf.
 * Both transfers share one spi_message so CS stays LOW between them.
 */
static int qcspi_xfer(struct rcp_spi_master *priv, unsigned int n_tx,
		      unsigned int n_rx)
{
	struct spi_message  msg;
	struct spi_transfer xfers[2] = {};

	spi_message_init(&msg);

	if (n_tx) {
		xfers[0].tx_buf = priv->spi_tx_buf;
		xfers[0].len    = n_tx;
		spi_message_add_tail(&xfers[0], &msg);
	}
	if (n_rx) {
		xfers[1].rx_buf = priv->spi_rx_buf;
		xfers[1].len    = n_rx;
		spi_message_add_tail(&xfers[1], &msg);
	}

	return spi_sync(priv->spi, &msg);
}

/* NOP */
static int qcspi_nop(struct rcp_spi_master *priv)
{
	priv->spi_tx_buf[0] = QCSPI_CMD_NOP;
	return qcspi_xfer(priv, 1, 0);
}

/* WREN — must send before any SRAM write */
static int qcspi_wren(struct rcp_spi_master *priv)
{
	priv->spi_tx_buf[0] = QCSPI_CMD_WREN;
	return qcspi_xfer(priv, 1, 0);
}

/*
 * RDSR — read 32-bit status register (big-endian).
 * TX: [0x05]  RX: [sr_byte3][sr_byte2][sr_byte1][sr_byte0]
 * bit24 (bit 0 of first RX byte) = BUSY.
 */
static int qcspi_rd_sr(struct rcp_spi_master *priv, u32 *sr)
{
	int ret;

	priv->spi_tx_buf[0] = QCSPI_CMD_RDSR;
	ret = qcspi_xfer(priv, 1, 4);
	if (ret)
		return ret;

	*sr = ((u32)priv->spi_rx_buf[0] << 24) |
	      ((u32)priv->spi_rx_buf[1] << 16) |
	      ((u32)priv->spi_rx_buf[2] <<  8) |
	       (u32)priv->spi_rx_buf[3];
	return 0;
}

/* Poll RDSR BUSY bit until clear. */
static int qcspi_poll_busy(struct rcp_spi_master *priv)
{
	int i;

	for (i = 0; i < RDSR_BUSY_RETRIES; i++) {
		u32 sr = 0;
		int ret = qcspi_rd_sr(priv, &sr);

		if (ret)
			return ret;
		if (!(sr & RDSR_BUSY_BIT))
			return 0;
		udelay(RDSR_BUSY_DELAY_US);
	}
	return -ETIMEDOUT;
}

/*
 * IRR — read QcSPI internal register.
 * TX: [0x81][reg][0x00][0x00]  RX: [val_be4]
 */
static int qcspi_rd_reg(struct rcp_spi_master *priv, u8 reg, u32 *val)
{
	int ret;

	priv->spi_tx_buf[0] = QCSPI_CMD_IRR;
	priv->spi_tx_buf[1] = reg;
	priv->spi_tx_buf[2] = 0x00;  /* 2 dummy bytes */
	priv->spi_tx_buf[3] = 0x00;

	ret = qcspi_xfer(priv, 4, 4);
	if (ret)
		return ret;

	*val = ((u32)priv->spi_rx_buf[0] << 24) |
	       ((u32)priv->spi_rx_buf[1] << 16) |
	       ((u32)priv->spi_rx_buf[2] <<  8) |
	        (u32)priv->spi_rx_buf[3];
	return 0;
}

/*
 * IRW — write QcSPI internal register.
 * TX: [0x82][reg][val_be4]  (6 bytes, no RX)
 */
static int qcspi_wr_reg(struct rcp_spi_master *priv, u8 reg, u32 val)
{
	priv->spi_tx_buf[0] = QCSPI_CMD_IRW;
	priv->spi_tx_buf[1] = reg;
	priv->spi_tx_buf[2] = (val >> 24) & 0xFF;
	priv->spi_tx_buf[3] = (val >> 16) & 0xFF;
	priv->spi_tx_buf[4] = (val >>  8) & 0xFF;
	priv->spi_tx_buf[5] =  val        & 0xFF;
	return qcspi_xfer(priv, 6, 0);
}

/*
 * FREAD — read `size` bytes from AHB address `addr`.
 *
 * Protocol: set TRNS_LEN, then send [0x0B][addr_be4][10 dummy], receive data.
 * CS is held LOW throughout (TX header and RX data in one spi_message).
 * Polls RDSR BUSY after transfer.
 *
 * NOTE: qcspi_poll_busy() overwrites spi_rx_buf[0..3] with the RDSR value.
 * buf must be non-NULL; size bytes are copied to buf before poll_busy runs.
 */
static int qcspi_fread(struct rcp_spi_master *priv, u32 addr, u8 *buf,
		       u16 size)
{
	u16 padded = (size + 3) & ~3U;
	int ret;

	if (WARN_ON(!buf))
		return -EINVAL;

	ret = qcspi_wr_reg(priv, SPI_SLAVE_TRNS_LEN, (u32)padded << 16);
	if (ret)
		return ret;

	priv->spi_tx_buf[0]  = QCSPI_CMD_FREAD;
	priv->spi_tx_buf[1]  = (addr >> 24) & 0xFF;
	priv->spi_tx_buf[2]  = (addr >> 16) & 0xFF;
	priv->spi_tx_buf[3]  = (addr >>  8) & 0xFF;
	priv->spi_tx_buf[4]  =  addr        & 0xFF;
	memset(&priv->spi_tx_buf[5], 0xA5, QCSPI_FREAD_DUMMY);

	ret = qcspi_xfer(priv, 5 + QCSPI_FREAD_DUMMY, padded);
	if (ret)
		return ret;

	/* Copy before poll_busy — poll_busy overwrites spi_rx_buf[0..3]. */
	if (buf != priv->spi_rx_buf)
		memcpy(buf, priv->spi_rx_buf, size);

	return qcspi_poll_busy(priv);
}

/*
 * WRITE — write `size` bytes from `buf` to AHB address `addr`.
 *
 * Header and data are packed into a single contiguous spi_tx_buf and sent
 * as ONE spi_transfer.  Using two separate transfers causes a DMA-descriptor
 * boundary between the 5-byte header and the data; the brief MOSI-idle gap
 * at that boundary triggers a QcSPI TX-underrun (TXUERR, RDSR bit 28),
 * corrupting the first data byte.  Packing avoids this entirely.
 *
 * Data is zero-padded to a 4-byte boundary.
 */
static int qcspi_write(struct rcp_spi_master *priv, u32 addr,
		       const u8 *buf, u16 size)
{
	u16 padded = (size + 3) & ~3U;
	int ret;

	priv->spi_tx_buf[0] = QCSPI_CMD_WRITE;
	priv->spi_tx_buf[1] = (addr >> 24) & 0xFF;
	priv->spi_tx_buf[2] = (addr >> 16) & 0xFF;
	priv->spi_tx_buf[3] = (addr >>  8) & 0xFF;
	priv->spi_tx_buf[4] =  addr        & 0xFF;
	memcpy(&priv->spi_tx_buf[5], buf, size);
	if (padded > size)
		memset(&priv->spi_tx_buf[5 + size], 0, padded - size);

	ret = qcspi_xfer(priv, 5 + padded, 0);
	if (ret) {
		pr_err("rcp: qcspi_write addr=0x%08x len=%u failed: %d\n",
		       addr, size, ret);
		return ret;
	}
	return qcspi_poll_busy(priv);
}

/*
 * Doorbell helpers — pulse HOST_INT bit in SPI_SLAVE_CONFIG.
 *
 * QCC730 QcSPI fires HOST_INT0/INT1 TWICE for a single IRW (hardware quirk):
 * once when the command bytes are processed, and once at CS deassert
 * (end-of-transaction evaluation with SEQMOD=1 active).
 *
 * Fix: send SET + CLEAR as two consecutive IRW commands within a SINGLE CS
 * assertion (12 bytes, SEQMOD=1 allows sequential commands in one frame).
 * By the time CS deasserts, HOST_INT bit is already 0, so the second
 * hardware evaluation finds nothing pending and does not re-trigger.
 */
static int qcspi_notify_int0(struct rcp_spi_master *priv)
{
	u32 cfg_set = priv->slave_config_cache | HOST_INT0_BIT;
	u32 cfg_clr = priv->slave_config_cache;

	/* IRW #1: set INT0 (fires interrupt) */
	priv->spi_tx_buf[0]  = QCSPI_CMD_IRW; priv->spi_tx_buf[1]  = SPI_SLAVE_CONFIG;
	priv->spi_tx_buf[2]  = (cfg_set >> 24) & 0xFF;
	priv->spi_tx_buf[3]  = (cfg_set >> 16) & 0xFF;
	priv->spi_tx_buf[4]  = (cfg_set >>  8) & 0xFF;
	priv->spi_tx_buf[5]  =  cfg_set        & 0xFF;
	/* IRW #2: clear INT0 (within same CS — prevents CS-deassert re-trigger) */
	priv->spi_tx_buf[6]  = QCSPI_CMD_IRW; priv->spi_tx_buf[7]  = SPI_SLAVE_CONFIG;
	priv->spi_tx_buf[8]  = (cfg_clr >> 24) & 0xFF;
	priv->spi_tx_buf[9]  = (cfg_clr >> 16) & 0xFF;
	priv->spi_tx_buf[10] = (cfg_clr >>  8) & 0xFF;
	priv->spi_tx_buf[11] =  cfg_clr        & 0xFF;
	return qcspi_xfer(priv, 12, 0);
}

static int qcspi_notify_int1(struct rcp_spi_master *priv)
{
	u32 cfg_set = priv->slave_config_cache | HOST_INT1_BIT;
	u32 cfg_clr = priv->slave_config_cache;

	priv->spi_tx_buf[0]  = QCSPI_CMD_IRW; priv->spi_tx_buf[1]  = SPI_SLAVE_CONFIG;
	priv->spi_tx_buf[2]  = (cfg_set >> 24) & 0xFF;
	priv->spi_tx_buf[3]  = (cfg_set >> 16) & 0xFF;
	priv->spi_tx_buf[4]  = (cfg_set >>  8) & 0xFF;
	priv->spi_tx_buf[5]  =  cfg_set        & 0xFF;
	priv->spi_tx_buf[6]  = QCSPI_CMD_IRW; priv->spi_tx_buf[7]  = SPI_SLAVE_CONFIG;
	priv->spi_tx_buf[8]  = (cfg_clr >> 24) & 0xFF;
	priv->spi_tx_buf[9]  = (cfg_clr >> 16) & 0xFF;
	priv->spi_tx_buf[10] = (cfg_clr >>  8) & 0xFF;
	priv->spi_tx_buf[11] =  cfg_clr        & 0xFF;
	return qcspi_xfer(priv, 12, 0);
}

/* =========================================================================
 * Ring helpers (called with spi_lock held)
 * ========================================================================= */

/*
 * Read the len field of the current s2m slot.
 * Returns 0 on SPI error (caller treats 0 as "no data").
 */
static uint16_t __get_s2m_len(struct rcp_spi_master *priv)
{
	u8  rd = priv->s2m_rd_idx;
	u8  len_buf[4] = {0};
	int ret;

	ret = qcspi_fread(priv, SHMEM_S2M_SLOT_LEN_ADDR(rd), len_buf, 4);
	if (ret < 0)
		return 0;
	return (uint16_t)len_buf[0] | ((uint16_t)len_buf[1] << 8);
}

/* =========================================================================
 * IRQ handler
 * ========================================================================= */
static irqreturn_t rcp_irq_handler(int irq, void *dev_id)
{
	struct rcp_spi_master *priv = dev_id;

	pr_debug("rcp: IRQ (rising edge)\n");
	atomic_set(&priv->rx_pending, 1);
	rcp_events_set(RCP_EVENT_RX_DATA);
	return IRQ_HANDLED;
}

/* =========================================================================
 * rcp_bus_api implementation
 * ========================================================================= */

/* Hard-reset the QCC730 and wait until it answers on QcSPI.
 *
 * This function pulses the active-low reset
 * line then retry until the chip is alive. 
 */
static int qcc730_reset(struct rcp_spi_master *priv)
{
	int r, p, ret;
	u32 cfg;

	if (!priv->reset_gpio) {
		priv->reset_gpio = devm_gpiod_get(&priv->spi->dev, "reset",
						  GPIOD_OUT_HIGH);
		if (IS_ERR(priv->reset_gpio)) {
			ret = PTR_ERR(priv->reset_gpio);
			priv->reset_gpio = NULL;
			pr_err("rcp: no reset GPIO (%d) - check DTS reset-gpios\n",
			       ret);
			return -ENODEV;
		}
	}

	for (r = 0; r < QCC730_RESET_MAX_RETRIES; r++) {
		/* Pulse reset: assert (logical 1 = physical low) then release. */
		gpiod_set_value(priv->reset_gpio, 1);
		msleep(QCC730_RESET_ASSERT_MS);
		gpiod_set_value(priv->reset_gpio, 0);
		msleep(QCC730_RESET_RELEASE_MS);
		msleep(QCC730_BOOT_WAIT_MS);

		/* Probe: NOP + WREN + a CONFIG read must all succeed. */
		for (p = 0; p < QCC730_PROBE_POLL_TRIES; p++) {
			if (qcspi_nop(priv) == 0 &&
			    qcspi_wren(priv) == 0 &&
			    qcspi_rd_reg(priv, SPI_SLAVE_CONFIG, &cfg) == 0) {
				pr_info("rcp: qcc730 alive after %d reset(s), %d poll(s)\n",
					r + 1, p + 1);
				return 0;
			}
			msleep(QCC730_PROBE_POLL_MS);
		}
		pr_warn("rcp: reset attempt %d: no QcSPI response, retrying\n",
			r + 1);
	}

	pr_err("rcp: qcc730 not responding after %d resets\n",
	       QCC730_RESET_MAX_RETRIES);
	return -ENODEV;
}

/* Bring the QcSPI ring to a known-clean state (spi_lock held).
 *
 * After a hard reset the QCC730 has already zeroed its shmem, but re-zeroing
 * from the host is idempotent and also covers a warm re-open. Zeroes both ring
 * headers and all slot.len fields, re-reads m2s.rd_idx into the local cache,
 * and clears the stale HOST_INT bits (which would otherwise fire a spurious
 * interrupt on the first doorbell). Returns 0 or -EIO.
 */
static int qcc730_ring_reset(struct rcp_spi_master *priv)
{
	static const u8 zeros[4] = {0, 0, 0, 0};
	u8 hdr[4];
	u32 cfg;
	int i, ret;

	priv->m2s_wr_idx        = 0;
	priv->m2s_rd_idx_cached = 0;
	priv->s2m_rd_idx        = 0;
	atomic_set(&priv->rx_pending, 0);

	ret  = qcspi_write(priv, SHMEM_M2S_BASE, zeros, 4);
	ret |= qcspi_write(priv, SHMEM_S2M_BASE, zeros, 4);
	for (i = 0; i < RCP_SHMEM_SLOTS; i++) {
		ret |= qcspi_write(priv, SHMEM_M2S_SLOT_LEN_ADDR(i), zeros, 4);
		ret |= qcspi_write(priv, SHMEM_S2M_SLOT_LEN_ADDR(i), zeros, 4);
	}
	if (ret) {
		pr_err("rcp: ring reset failed: %d\n", ret);
		return -EIO;
	}

	/* Cache m2s.rd_idx so spi_master_write() can check ring capacity
	 * without an extra FREAD on the fast path. */
	ret = qcspi_fread(priv, SHMEM_M2S_BASE, hdr, 4);
	if (ret) {
		pr_err("rcp: read m2s ring header failed: %d\n", ret);
		return -EIO;
	}
	priv->m2s_rd_idx_cached = hdr[1]; /* byte[1] = rd_idx */
	pr_info("rcp: ring state reset, m2s rd_idx=%u\n",
		priv->m2s_rd_idx_cached);

	/* Clear stale HOST_INT bits in SPI_SLAVE_CONFIG. */
	cfg = 0;
	ret = qcspi_rd_reg(priv, SPI_SLAVE_CONFIG, &cfg);
	if (ret) {
		pr_err("rcp: read SPI_SLAVE_CONFIG failed: %d\n", ret);
		return -EIO;
	}
	cfg &= ~(HOST_INT0_BIT | HOST_INT1_BIT);
	ret = qcspi_wr_reg(priv, SPI_SLAVE_CONFIG, cfg);
	if (ret) {
		pr_err("rcp: clear SPI_SLAVE_CONFIG INT failed: %d\n", ret);
		return -EIO;
	}
	pr_info("rcp: SPI_SLAVE_CONFIG INT cleared (cfg=0x%08x)\n", cfg);

	/* Cache CONFIG value (INT bits clear) for single-IRW doorbells. */
	priv->slave_config_cache = cfg;
	return 0;
}

static enum rcp_bus_status spi_master_open(void)
{
	struct rcp_spi_master *priv = &g_spi_master;
	int ret;

	if (priv->irq_registered)
		return RCP_BUS_OK;

	mutex_lock(&priv->spi_lock);

	/* Step 1: hard-reset the chip so it boots into a clean state (fresh
	 * shmem + re-init s2m semaphore). This is what fixes the ring desync
	 * when the host is reloaded while the QCC730 still has in-flight state
	 * from the previous session (host can zero SRAM but cannot reset the
	 * QCC730's software counters/semaphore - only a reboot does). Runs
	 * before request_irq(), so no IRQ handler is attached during the pulse. */
	ret = qcc730_reset(priv);
	if (ret) {
		mutex_unlock(&priv->spi_lock);
		return RCP_BUS_ERR_IO;
	}

	/* Step 2: bring the ring to a known-clean state. */
	ret = qcc730_ring_reset(priv);
	if (ret) {
		mutex_unlock(&priv->spi_lock);
		return RCP_BUS_ERR_IO;
	}

	mutex_unlock(&priv->spi_lock);

	/* Step 3: attach the RX IRQ and arm TX. */
	ret = request_irq(priv->irq, rcp_irq_handler,
			  IRQF_TRIGGER_RISING, "rcp_wifi", priv);
	if (ret < 0) {
		pr_err("rcp: request_irq failed: %d\n", ret);
		return RCP_BUS_ERR_IO;
	}

	priv->irq_registered = true;
	rcp_events_set(RCP_EVENT_TX_RDY);
	pr_info("rcp: QcSPI bus open\n");
	return RCP_BUS_OK;
}

static enum rcp_bus_status spi_master_close(void)
{
	struct rcp_spi_master *priv = &g_spi_master;

	if (priv->irq_registered) {
		free_irq(priv->irq, priv);
		priv->irq_registered = false;
	}
	return RCP_BUS_OK;
}

static enum rcp_bus_status spi_master_write(uint8_t *tx, uint16_t len)
{
	struct rcp_spi_master *priv = &g_spi_master;
	u8  wr = priv->m2s_wr_idx;
	u32 buf_addr = SHMEM_M2S_SLOT_BUF_ADDR(wr);
	u32 len_addr = SHMEM_M2S_SLOT_LEN_ADDR(wr);
	u8  len_bytes[4];
	int ret;

	pr_debug("rcp: write slot=%u len=%u\n", wr, len);

	mutex_lock(&priv->spi_lock);

	/* Check m2s ring capacity before writing.
	 * Fast path: use cached rd_idx (no SPI transaction needed).
	 * Slow path: if ring appears full, refresh rd_idx with one FREAD of the
	 * 4-byte m2s ring header [wr_idx|rd_idx|_pad] from QCC730 SRAM. */
	{
		u8 used = (wr - priv->m2s_rd_idx_cached + RCP_SHMEM_SLOTS)
			  % RCP_SHMEM_SLOTS;
		if (used >= RCP_SHMEM_SLOTS - 1) {
			u8 hdr[4];
			ret = qcspi_fread(priv, SHMEM_M2S_BASE, hdr, 4);
			if (ret) {
				mutex_unlock(&priv->spi_lock);
				return RCP_BUS_ERR_IO;
			}
			priv->m2s_rd_idx_cached = hdr[1]; /* byte[1] = rd_idx */
			used = (wr - priv->m2s_rd_idx_cached + RCP_SHMEM_SLOTS)
			       % RCP_SHMEM_SLOTS;
			if (used >= RCP_SHMEM_SLOTS - 1) {
				mutex_unlock(&priv->spi_lock);
				return RCP_BUS_ERR_PEER_MEMORY;
			}
		}
	}

	/* Step 1: write slot.buf (payload).
	 * QCC730 polls slot.len to detect new messages — buf must be fully
	 * written first so QCC730 never reads a non-zero len with stale buf. */
	ret = qcspi_write(priv, buf_addr, tx, len);
	if (ret) {
		pr_err("rcp: write slot[%u] buf failed: %d\n", wr, ret);
		mutex_unlock(&priv->spi_lock);
		return RCP_BUS_ERR_IO;
	}

	/* wmb: ensure buf write is visible to QCC730 before len commit */
	wmb();

	/* Step 2: write slot.len (commit — signals QCC730 new message ready).
	 * len is uint16_t little-endian stored in a uint32_t field. */
	len_bytes[0] =  len        & 0xFF;
	len_bytes[1] = (len >>  8) & 0xFF;
	len_bytes[2] = 0;
	len_bytes[3] = 0;
	ret = qcspi_write(priv, len_addr, len_bytes, 4);
	if (ret) {
		pr_err("rcp: write slot[%u] len failed: %d\n", wr, ret);
		mutex_unlock(&priv->spi_lock);
		return RCP_BUS_ERR_IO;
	}

	/* Step 3: HOST_INT0 doorbell — notify QCC730 that slot is committed */
	ret = qcspi_notify_int0(priv);
	if (ret) {
		pr_err("rcp: notify_int0 failed: %d\n", ret);
		mutex_unlock(&priv->spi_lock);
		return RCP_BUS_ERR_IO;
	}

	priv->m2s_wr_idx = (wr + 1) % RCP_SHMEM_SLOTS;
	pr_debug("rcp: write slot[%u] OK, m2s_wr=%u\n", wr, priv->m2s_wr_idx);

	mutex_unlock(&priv->spi_lock);
	return RCP_BUS_OK;
}

static uint16_t spi_master_get_rx_len(void)
{
	struct rcp_spi_master *priv = &g_spi_master;
	uint16_t len;

	mutex_lock(&priv->spi_lock);
	/* Clear RX_DATA *before* reading slot.len, then re-arm if data is present.
	 * Ordering matters: the GPIO IRQ handler sets RX_DATA on every s2m write.
	 * If we read len first and clear afterwards, an IRQ that fires between the
	 * read and the clear is lost — the event is wiped, the event loop sleeps,
	 * and since QCC730 stops toggling once its ring fills, nothing ever wakes
	 * the reader again (permanent s2m deadlock). Clearing first means any IRQ
	 * after the read leaves RX_DATA set; reading data re-arms it ourselves. */
	rcp_events_clear(RCP_EVENT_RX_DATA);
	len = __get_s2m_len(priv);
	if (len != 0) {
		rcp_events_set(RCP_EVENT_RX_DATA);
	}
	mutex_unlock(&priv->spi_lock);

	return len;
}

static enum rcp_bus_status spi_master_read(uint8_t *rx, uint16_t len,
					   uint16_t *nread)
{
	struct rcp_spi_master *priv = &g_spi_master;
	u8  rd;
	u16 n;
	int ret;

	pr_debug("rcp: read slot[%u]\n", priv->s2m_rd_idx);

	mutex_lock(&priv->spi_lock);

	rd = priv->s2m_rd_idx;

	/* Use caller-supplied len directly instead of re-reading slot.len from
	 * SRAM.  get_rx_data() already called spi_master_get_rx_len() which
	 * performed the FREAD to obtain the length; repeating it here wastes
	 * 3 spi_sync() calls (~50 µs at 25 MHz SPI).
	 * Validate: len==0 or len>BUF_SIZE fall through to the checks below. */
	n = (u16)len;

	if (n == 0) {
		*nread = 0;
		/* Ring empty. Do not touch RX_DATA here — spi_master_get_rx_len()
		 * is the sole authority for arming/clearing it (clear-before-read),
		 * so clearing here could wipe a concurrent IRQ and lose a wakeup. */
		mutex_unlock(&priv->spi_lock);
		return RCP_BUS_ERR_EMPTY;
	}

	/* Bad len: slot is corrupted (e.g. SPI bus recovered from a timeout).
	 * Release it (zero len in SRAM) and skip so the producer can reuse it. */
	if (n > RCP_SHMEM_BUF_SIZE) {
		u8 zero_len[4] = {0};

		pr_warn("rcp: s2m slot[%u] bad len=%u — skipping\n", rd, n);
		qcspi_write(priv, SHMEM_S2M_SLOT_LEN_ADDR(rd), zero_len, 4);
		qcspi_notify_int1(priv);
		priv->s2m_rd_idx = (rd + 1) % RCP_SHMEM_SLOTS;
		*nread = 0;
		mutex_unlock(&priv->spi_lock);
		return RCP_BUS_ERR_IO;
	}

	/* FREAD slot.buf directly into rx — no +4 offset trick needed since
	 * qcspi_fread now requires non-NULL buf and copies before poll_busy. */
	ret = qcspi_fread(priv, SHMEM_S2M_SLOT_BUF_ADDR(rd), rx, n);
	if (ret) {
		mutex_unlock(&priv->spi_lock);
		return RCP_BUS_ERR_IO;
	}
	*nread = n;

	/* Release the slot: zero slot[rd].len in SRAM. This is the single source
	 * of truth the QCC730 producer polls to decide the slot is free — it must
	 * be written before the INT1 doorbell so a producer woken by INT1 sees the
	 * release. (Previously the QCC730 INT1 ISR cleared len by counting edges,
	 * which desynced under edge coalescing at high rate.) */
	{
		u8 zero_len[4] = {0};

		ret = qcspi_write(priv, SHMEM_S2M_SLOT_LEN_ADDR(rd), zero_len, 4);
		if (ret) {
			mutex_unlock(&priv->spi_lock);
			return RCP_BUS_ERR_IO;
		}
	}

	/* HOST_INT1 doorbell — wake a producer that may be blocked on a full ring. */
	ret = qcspi_notify_int1(priv);
	if (ret) {
		mutex_unlock(&priv->spi_lock);
		return RCP_BUS_ERR_IO;
	}

	priv->s2m_rd_idx = (rd + 1) % RCP_SHMEM_SLOTS;
	atomic_set(&priv->rx_pending, 0);

	/* Do not re-arm RX_DATA here.  spi_master_get_rx_len() re-arms it when
	 * the next slot's len is non-zero, letting the event loop sleep until
	 * the next GPIO IRQ instead of busy-polling. */

	mutex_unlock(&priv->spi_lock);
	return RCP_BUS_OK;
}

static enum rcp_bus_status spi_master_get_caps(struct rcp_bus_caps *caps)
{
	caps->mtu = RCP_SHMEM_BUF_SIZE;
	return RCP_BUS_OK;
}

/* =========================================================================
 * Probe / remove
 * ========================================================================= */

int rcp_spi_master_probe(struct spi_device *spi, struct gpio_desc *irq_gpio,
			 int irq)
{
	struct rcp_spi_master *priv = &g_spi_master;

	priv->spi      = spi;
	priv->irq_gpio = irq_gpio;
	priv->irq      = irq;
	mutex_init(&priv->spi_lock);

	/* Register bus API here.  open() is called later by activate_bus()
	 * from init_rcp(), after rcp_events_init() has run. */
	return rcp_bus_register(RCP_BUS_SPI, &spi_master_api) == RCP_BUS_OK
		? 0 : -EIO;
}

void rcp_spi_master_remove(void)
{
	spi_master_close();
}
