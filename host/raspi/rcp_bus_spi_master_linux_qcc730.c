/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * Linux QcSPI master bus driver for QCC730 RCP.
 *
 * Low-level: pre-allocated DMA-safe TX/RX buffers, spi_lock mutex, half-duplex
 * write_then_read for all commands.
 *
 * High-level ring protocol (matches rcp_bus_spi_slave_qcc730.c): fixed
 * SHMEM_BASE with 8-slot m2s / s2m rings. HOST_INT0 doorbell = m2s slot
 * committed (RPi -> QCC730); HOST_INT1 = s2m slot consumed (QCC730 -> RPi);
 * GPIO8 rising-edge IRQ = QCC730 signals s2m data ready.
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/types.h>

#include "rcp_user_config.h"
#include "rcp_bus_spi_master_linux_qcc730.h"
#include "rcp_wifi_cfg80211.h"  /* struct rcp_priv, g_priv, power FSM fields */

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
	struct gpio_desc   *wakeup_gpio;  /* GPIO16 -> QCC730 EXT_WAKEUP, active high */
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
 * Set TRNS_LEN, send [0x0B][addr_be4][10 dummy] + RX data in one CS-low
 * spi_message, then poll RDSR BUSY. poll_busy overwrites spi_rx_buf[0..3], so
 * the (non-NULL) buf is filled before it runs.
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
 * WRITE — write `size` bytes from `buf` to AHB address `addr` (zero-padded to
 * 4 bytes). Header + data are packed into ONE spi_transfer: two separate
 * transfers leave a MOSI-idle gap at the DMA-descriptor boundary that triggers
 * a QcSPI TX-underrun (TXUERR, RDSR bit 28) and corrupts the first data byte.
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
 * Doorbell — pulse HOST_INT bit in SPI_SLAVE_CONFIG.
 *
 * QCC730 QcSPI fires HOST_INT TWICE for a single IRW (hardware quirk): once
 * when the command is processed, once at CS deassert (SEQMOD=1). Fix: send
 * SET + CLEAR as two IRWs within ONE CS assertion (12 bytes), so by CS deassert
 * the bit is already 0 and the second evaluation does not re-trigger.
 */
static int qcspi_notify(struct rcp_spi_master *priv, u32 int_bit)
{
	u32 cfg_set = priv->slave_config_cache | int_bit;
	u32 cfg_clr = priv->slave_config_cache;

	/* IRW #1: set the INT bit (fires interrupt) */
	priv->spi_tx_buf[0]  = QCSPI_CMD_IRW; priv->spi_tx_buf[1]  = SPI_SLAVE_CONFIG;
	priv->spi_tx_buf[2]  = (cfg_set >> 24) & 0xFF;
	priv->spi_tx_buf[3]  = (cfg_set >> 16) & 0xFF;
	priv->spi_tx_buf[4]  = (cfg_set >>  8) & 0xFF;
	priv->spi_tx_buf[5]  =  cfg_set        & 0xFF;
	/* IRW #2: clear it (within same CS — prevents CS-deassert re-trigger) */
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

/* Hard-reset the QCC730 (active-low pulse) and retry until it answers on QcSPI. */
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

/* A2F wake pulse timing. GPIO16 idles HIGH; wake = drive LOW, hold, then HIGH.
 * The low period must be long enough for the 730 to exit BMPS and re-init
 * QcSPI before we drive the line back high and start the SPI transaction. */
#define QCC730_WAKE_LOW_MS       300

/*
 * qcc730_wakeup - ensure the QCC730 is AWAKE before a bus write.
 *
 * If pwr_state is ASLEEP the chip may be in BMPS/S2RAM, so pulse GPIO16
 * (EXT_WAKEUP) LOW then HIGH to fire the 730's wake ISR, then wait a fixed
 * settle delay. The host cannot observe the wake transition (firmware BMPS
 * raises no host-visible event), so there is no completion wait — if the 730
 * did not wake, the subsequent SPI write fails via the ring busy-poll and the
 * caller's error path handles it.
 */
int qcc730_wakeup(struct rcp_spi_master *priv)
{
	unsigned long flags;
	bool awake;

	/* Pre-connect / no power state tracked yet: assume reachable. */
	if (!g_priv)
		return 0;

	/* No wake line bound (pre-wiring builds): assume always reachable. */
	if (!priv->wakeup_gpio)
		return 0;

	/* Fast path: already awake, nothing to do. */
	spin_lock_irqsave(&g_priv->pwr_lock, flags);
	awake = (g_priv->pwr_state == RCP_PWR_AWAKE);
	spin_unlock_irqrestore(&g_priv->pwr_lock, flags);
	if (awake)
		return 0;

	/* We do NOT latch pwr_state=AWAKE here: while power save is enabled the 730
	 * re-enters BMPS whenever it goes idle (no host-visible SLEEP event). So we
	 * keep ASLEEP and pulse before every write until set_power_mgmt off. */
	gpiod_set_value(priv->wakeup_gpio, 0);
	msleep(QCC730_WAKE_LOW_MS);
	gpiod_set_value(priv->wakeup_gpio, 1);

	return 0;
}

/* No-arg wrapper for callers outside this file (e.g. cfg80211 set_power_mgmt)
 * that have no handle to the static g_spi_master. */
int rcp_bus_wakeup_qcc730(void)
{
	return qcc730_wakeup(&g_spi_master);
}

/* Bring the QcSPI ring to a known-clean state (spi_lock held). Zeroes both ring
 * headers and all slot.len fields, re-reads m2s.rd_idx into the local cache, and
 * clears stale HOST_INT bits (else a spurious interrupt fires on the first
 * doorbell). Idempotent after a hard reset; also covers a warm re-open. */
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

	/* Step 1: hard-reset for a clean boot (fresh shmem + re-init s2m
	 * semaphore). Fixes ring desync on host reload while the QCC730 still has
	 * in-flight state: the host can zero SRAM but only a reboot resets the
	 * QCC730's own counters/semaphore. Runs before request_irq(). */
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

	/* Check m2s ring capacity. Fast path: cached rd_idx (no SPI). Slow path:
	 * if the ring looks full, refresh rd_idx with one FREAD of the 4-byte m2s
	 * header [wr_idx|rd_idx|_pad] from QCC730 SRAM. */
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
	ret = qcspi_notify(priv, HOST_INT0_BIT);
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
	/* Clear RX_DATA BEFORE reading slot.len, then re-arm if data is present.
	 * Ordering is critical: an IRQ firing between a len-first read and a later
	 * clear would be wiped, and since QCC730 stops toggling once its ring fills,
	 * nothing would ever wake the reader again (permanent s2m deadlock). */
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

	/* Use caller-supplied len (get_rx_len() already FREAD it); re-reading here
	 * would waste 3 spi_sync() calls. len==0 / len>BUF_SIZE handled below. */
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
		qcspi_notify(priv, HOST_INT1_BIT);
		priv->s2m_rd_idx = (rd + 1) % RCP_SHMEM_SLOTS;
		*nread = 0;
		mutex_unlock(&priv->spi_lock);
		return RCP_BUS_ERR_IO;
	}

	ret = qcspi_fread(priv, SHMEM_S2M_SLOT_BUF_ADDR(rd), rx, n);
	if (ret) {
		mutex_unlock(&priv->spi_lock);
		return RCP_BUS_ERR_IO;
	}
	*nread = n;

	/* Release the slot: zero slot[rd].len in SRAM — the source of truth the
	 * QCC730 producer polls for a free slot. Must be written before the INT1
	 * doorbell so a producer woken by INT1 sees the release. */
	{
		u8 zero_len[4] = {0};

		ret = qcspi_write(priv, SHMEM_S2M_SLOT_LEN_ADDR(rd), zero_len, 4);
		if (ret) {
			mutex_unlock(&priv->spi_lock);
			return RCP_BUS_ERR_IO;
		}
	}

	/* HOST_INT1 doorbell — wake a producer that may be blocked on a full ring. */
	ret = qcspi_notify(priv, HOST_INT1_BIT);
	if (ret) {
		mutex_unlock(&priv->spi_lock);
		return RCP_BUS_ERR_IO;
	}

	priv->s2m_rd_idx = (rd + 1) % RCP_SHMEM_SLOTS;
	atomic_set(&priv->rx_pending, 0);

	/* Do not re-arm RX_DATA here — get_rx_len() re-arms it on the next
	 * non-zero slot.len, so the event loop sleeps until the next GPIO IRQ. */

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

	/* GPIO16 -> QCC730 EXT_WAKEUP (A2F). Idles HIGH; wake is an active-LOW
	 * pulse (see qcc730_wakeup). Optional: pre-wiring builds run without it
	 * and qcc730_wakeup() then assumes the chip is reachable. */
	priv->wakeup_gpio = devm_gpiod_get_optional(&spi->dev, "wakeup",
						    GPIOD_OUT_HIGH);
	if (IS_ERR(priv->wakeup_gpio)) {
		pr_warn("rcp: no wakeup GPIO (%ld) - S2RAM wake disabled\n",
			PTR_ERR(priv->wakeup_gpio));
		priv->wakeup_gpio = NULL;
	}

	/* Register bus API here.  open() is called later by activate_bus()
	 * from init_rcp(), after rcp_events_init() has run. */
	return rcp_bus_register(RCP_BUS_SPI, &spi_master_api) == RCP_BUS_OK
		? 0 : -EIO;
}

void rcp_spi_master_remove(void)
{
	spi_master_close();
}
