/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * RCP link throughput benchmark — host side.
 *
 * Enabled at insmod time with bench_mode=1 (no recompile). Runs a stream
 * loopback through the full RCP protocol stack — the QCC730 firmware echoes
 * the stream back, so no QCC730 shell command and no firmware change are
 * needed. Measures real end-to-end link throughput.
 *
 * Example:
 *   sudo insmod rcp_wifi.ko bench_mode=1 bench_count=1000 bench_slot_size=1596
 *   dmesg | grep bench_loopback
 */

#ifndef RCP_BENCH_H
#define RCP_BENCH_H

struct rcp_priv;

/* Current bench mode (value of the bench_mode module param). 0 = normal WiFi. */
int rcp_bench_mode(void);

/* Start the loopback bench kthread (called from probe when rcp_bench_mode()!=0). */
void rcp_bench_start(struct rcp_priv *priv);

#endif /* RCP_BENCH_H */
