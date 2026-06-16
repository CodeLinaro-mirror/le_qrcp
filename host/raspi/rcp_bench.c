/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * RCP link throughput benchmark — host kthread.
 *
 * Enabled with bench_mode=1 at insmod time (see rcp_bench.h). Runs a stream
 * loopback: the host sends on an RCP stream that the QCC730 firmware echoes
 * back (built-in loopback control message — no QCC730 shell command and no
 * firmware change needed), then times the round-trips. The traffic rides the
 * full RCP protocol stack (seq/ACK/buffers/dispatch), so this measures the
 * real end-to-end link throughput, not the raw ring ceiling.
 */

#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <linux/string.h>

#include <rcp_bus.h>
#include <rcp_interface.h>
#include <rcp_interface_impl.h>
#include <rcp_cntrl_msgs.h>
#include <buffer_mgmt.h>
#include <rcp_definitions.h>

#include "rcp_wifi_cfg80211.h"
#include "rcp_bench.h"

/* ---- Runtime configuration (insmod module params) ----------------------- */

static int bench_mode;        /* 0 = normal WiFi, 1 = loopback benchmark */
module_param(bench_mode, int, 0444);
MODULE_PARM_DESC(bench_mode, "0=WiFi 1=loopback throughput benchmark");

static int bench_count = 1000;   /* number of round-trips to run */
module_param(bench_count, int, 0444);
MODULE_PARM_DESC(bench_count, "number of round-trips");

static int bench_slot_size = 1596;  /* bytes per message (clamped to bus MTU) */
module_param(bench_slot_size, int, 0444);
MODULE_PARM_DESC(bench_slot_size, "bytes per message");

static int bench_stream = RCP_STREAM_TEST_ONLY;  /* stream to loop back */
module_param(bench_stream, int, 0444);
MODULE_PARM_DESC(bench_stream, "RCP stream to loop back (default 6=TEST_ONLY)");

static int bench_inflight = 4;   /* outstanding msgs (<= ACK window) */
module_param(bench_inflight, int, 0444);
MODULE_PARM_DESC(bench_inflight, "outstanding messages in flight (1..7)");

int rcp_bench_mode(void)
{
	return bench_mode;
}

/* Usable data payload per message. The RCP header eats RCP_HDR_SIZE of the
 * bus slot, so the payload must be MTU - header — sending MTU bytes would
 * marshal to MTU+4 and overflow the QCC730 slot buf into the next slot's len
 * field (SRAM corruption → QCC730 crash). */
static int bench_slot_len(void)
{
	struct rcp_bus_caps caps;
	int max = RCP_BUF_SIZE;
	int len = bench_slot_size;

	if (rcp_bus_get_caps(RCP_BUS_SPI, &caps) == RCP_BUS_OK)
		max = caps.mtu;
	max -= RCP_HDR_SIZE;

	if (len <= 0 || len > max)
		len = max;
	return len;
}

/* =========================================================================
 * RCP stream loopback  (round-trip, full protocol stack)
 *
 * The QCC730 firmware has a built-in loopback: on RCP_CNTRL_LOOPBACK_REQ it
 * swaps the stream's RX handler for one that echoes every payload back. So
 * this needs NO QCC730 shell command and NO firmware change — the host just
 * requests loopback on bench_stream, then pumps messages and counts the echoes.
 *
 * Everything runs in this one kthread (send + run_event_loop_once + the RX
 * callback), so the counters need no locking. We keep bench_inflight messages
 * outstanding to fill the pipe without exceeding the RCP ACK window.
 * ========================================================================= */
static int loop_target;
static int loop_recvd;
static int loop_sent;
static int loop_slot;
static int loop_inflight;

static bool bench_loop_send_one(void)
{
	rcp_buffer_hdl_t *buf;

	if (loop_sent >= loop_target)
		return false;

	buf = get_buffer(loop_slot);
	if (!buf)
		return false;   /* pool momentarily exhausted; retry next pump */

	memset(buf->data_ptr, 0x5A, loop_slot);
	buf->size = loop_slot;

	if (link_send(bench_stream, buf) != RCP_LNK_SUCCESS) {
		release_buffer(&buf);
		return false;   /* TX queue full; retry next pump */
	}
	loop_sent++;
	return true;
}

/* Top the pipe back up to loop_inflight outstanding messages — but only once
 * the stream is ACTIVE. While it is still SYNCING (before the first echo lands)
 * build_data_stream_msg re-issues a *sync* message for every send, so we must
 * keep exactly one message outstanding until sync completes. */
static void bench_loop_refill(void)
{
	if (link_get_state(bench_stream) != LINK_STATE_ACTIVE)
		return;

	while ((loop_sent - loop_recvd) < loop_inflight) {
		if (!bench_loop_send_one())
			break;
	}
}

/* Echo handler: one message arrives back per completed round-trip. Both the
 * INIT echo (which completes the sync handshake) and the DATA echoes carry the
 * payload and count as a round-trip. We only ramp up to loop_inflight AFTER the
 * first echo, so no second send happens while the stream is still SYNCING
 * (build_data_stream_msg would otherwise re-issue a sync message). */
static uint8_t bench_loop_rx(enum rcp_rx_status status,
			     rcp_buffer_hdl_t *payload,
			     rcp_error_info_t *err_info)
{
	if (status == RCP_RX_STS_DATA_MSG || status == RCP_RX_STS_INIT_MSG)
		loop_recvd++;

	bench_loop_refill();
	return RCP_RX_NO_ACTION;
}

static int bench_loopback_thread(void *unused)
{
	ktime_t t0, t1;
	u64 us, bytes;

	loop_slot     = bench_slot_len();
	loop_target   = bench_count;
	loop_recvd    = 0;
	loop_sent     = 0;
	loop_inflight = bench_inflight;

	if (loop_inflight < 1)
		loop_inflight = 1;
	if (loop_inflight > RCP_SEQ_NUM_ACK_WINDOW)
		loop_inflight = RCP_SEQ_NUM_ACK_WINDOW;

	pr_info("bench_loopback: %d round-trips x %d B on stream %d (inflight %d)\n",
		loop_target, loop_slot, bench_stream, loop_inflight);

	/* Ask the QCC730 to echo this stream (built-in firmware loopback). */
	if (!send_loopback_request(bench_stream)) {
		pr_err("bench_loopback: send_loopback_request failed\n");
		goto park;
	}
	link_register_rx_handler(bench_stream, bench_loop_rx);

	/* Prime with a SINGLE message: it drives the stream sync handshake. The
	 * firmware echo completes the sync; bench_loop_rx then ramps up to
	 * loop_inflight outstanding, all flowing as DATA_XFER on the ACTIVE link. */
	t0 = ktime_get();
	bench_loop_send_one();

	/* Single-threaded pump: run_event_loop_once() sends queued TX, reads RX,
	 * and dispatches to bench_loop_rx (which refills). Runs until all echoes
	 * are back. */
	while (loop_recvd < loop_target && !kthread_should_stop()) {
		run_event_loop_once();
		/* Backup nudge: retry sends that stalled earlier on a full TX
		 * queue / empty buffer pool (no-op once the pipe is full). */
		bench_loop_refill();
	}
	t1 = ktime_get();

	us    = (u64)ktime_to_us(ktime_sub(t1, t0));
	bytes = (u64)loop_recvd * loop_slot;
	/* Round-trip: bytes crossed the bus twice (host->QCC730->host). */
	pr_info("bench_loopback: %d rt, %llu B each way in %llu us = %llu KB/s (one-way), %llu KB/s (bus, x2)\n",
		loop_recvd, bytes, us,
		us > 0 ? div64_u64(bytes * 1000, us) : 0,
		us > 0 ? div64_u64(bytes * 2 * 1000, us) : 0);

	send_loopback_cancel();
park:
	while (!kthread_should_stop())
		msleep(100);
	return 0;
}

/* =========================================================================
 * Entry point — called from rcp_wifi_probe() when bench_mode != 0.
 * Stores the bench kthread handle in priv->event_thread so that
 * rcp_wifi_remove() can stop it via the normal kthread_stop() path.
 * ========================================================================= */
void rcp_bench_start(struct rcp_priv *priv)
{
	priv->event_thread = kthread_run(bench_loopback_thread, NULL,
					 "rcp_bench_loop");
	if (IS_ERR(priv->event_thread))
		pr_err("bench: failed to start loopback kthread: %ld\n",
		       PTR_ERR(priv->event_thread));
}
