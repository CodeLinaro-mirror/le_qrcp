/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/logging/log_output.h>

#include <rcp_definitions.h>
#include <rcp_interface.h>

/*
 * Keep this backend self-contained so it does not depend on the VSerial driver
 * exposing a public header in every tree revision.
 */
int rcp_vserial_uart_write_port(uint8_t port, const uint8_t *data, size_t len,
				bool drop_when_busy);

#define RCP_VSERIAL_LOG_BACKOFF_MS 1000U

struct rcp_vserial_log_backend_ctx {
	uint8_t port;
	bool drop_when_busy;
	bool duplicate_to_console;
	bool panic;
	bool line_terminated;
	bool suppress_message;
	uint32_t log_format_current;
	uint32_t backoff_until_ms;
	const struct device *console_uart;
};

static int rcp_vserial_log_char_out(uint8_t *buf, size_t len, void *ctx_ptr);
static bool rcp_vserial_log_transport_ready(void);
static bool rcp_vserial_log_backend_in_backoff(
	const struct rcp_vserial_log_backend_ctx *ctx);
static void rcp_vserial_log_backend_enter_backoff(struct rcp_vserial_log_backend_ctx *ctx);

static uint8_t rcp_vserial_log_output_buf[CONFIG_QUALCOMM_RCP_APP_VSERIAL_LOG_LINE_MAX];
LOG_OUTPUT_DEFINE(rcp_vserial_log_output, rcp_vserial_log_char_out, rcp_vserial_log_output_buf,
		  sizeof(rcp_vserial_log_output_buf));

static struct rcp_vserial_log_backend_ctx rcp_vserial_log_ctx = {
	.port = CONFIG_QUALCOMM_RCP_APP_VSERIAL_LOG_PORT,
	.drop_when_busy = IS_ENABLED(CONFIG_QUALCOMM_RCP_APP_VSERIAL_LOG_DROP_WHEN_BUSY),
	.duplicate_to_console = IS_ENABLED(CONFIG_QUALCOMM_RCP_APP_VSERIAL_LOG_DUPLICATE_TO_PHYS_UART),
	.panic = false,
	.line_terminated = true,
	.suppress_message = false,
	.log_format_current = LOG_OUTPUT_TEXT,
	.backoff_until_ms = 0U,
	.console_uart = NULL,
};

static bool rcp_vserial_log_transport_ready(void)
{
	if (!link_is_initialized()) {
		return false;
	}

	/*
	 * Remote log transport is ready only after the host explicitly syncs the VSerial0
	 * data stream. This avoids repeated INIT_DATA ping-pong if the MCU tries to start
	 * the stream itself from within the logging path.
	 */
	return link_get_state(RCP_STREAM_VSERIAL0) == LINK_STATE_ACTIVE;
}

static bool rcp_vserial_log_backend_in_backoff(const struct rcp_vserial_log_backend_ctx *ctx)
{
	if ((ctx == NULL) || ctx->panic || (ctx->backoff_until_ms == 0U)) {
		return false;
	}

	return (int32_t)(k_uptime_get_32() - ctx->backoff_until_ms) < 0;
}

static void rcp_vserial_log_backend_enter_backoff(struct rcp_vserial_log_backend_ctx *ctx)
{
	if ((ctx == NULL) || ctx->panic) {
		return;
	}

	ctx->backoff_until_ms = k_uptime_get_32() + RCP_VSERIAL_LOG_BACKOFF_MS;
}

static int rcp_vserial_log_char_out(uint8_t *buf, size_t len, void *ctx_ptr)
{
	struct rcp_vserial_log_backend_ctx *ctx = ctx_ptr;
	int rc;
	bool vserial_ready;

	if ((ctx == NULL) || (buf == NULL) || (len == 0U)) {
		return (int)len;
	}

	vserial_ready = rcp_vserial_log_transport_ready() &&
		       !rcp_vserial_log_backend_in_backoff(ctx) &&
		       !ctx->suppress_message;

	/*
	 * Do not start emitting VSerial log traffic before host<->MCU runtime transport
	 * is active. The control stream alone is not enough because it does not drive the
	 * data-stream state machine to ACTIVE.
	 */
	if (vserial_ready) {
		rc = rcp_vserial_uart_write_port(ctx->port, buf, len,
						 ctx->drop_when_busy && !ctx->panic);
		if (rc < 0) {
			/*
			 * Logging must never compete with or destabilize the control/data path.
			 * When the shared transport is congested, stop mirroring for a short
			 * cooldown window and drop the rest of the current formatted message.
			 */
			ctx->suppress_message = true;
			rcp_vserial_log_backend_enter_backoff(ctx);
		}
	}

	if (ctx->duplicate_to_console && (ctx->console_uart != NULL) &&
	    device_is_ready(ctx->console_uart)) {
		for (size_t i = 0U; i < len; i++) {
			uart_poll_out(ctx->console_uart, buf[i]);
		}
	}

	ctx->line_terminated = (buf[len - 1U] == '\n');
	return (int)len;
}

static void rcp_vserial_log_backend_process(const struct log_backend *const backend,
					    union log_msg_generic *msg)
{
	struct rcp_vserial_log_backend_ctx *ctx = backend->cb->ctx;
	log_format_func_t log_output_func;
	uint32_t flags = log_backend_std_get_flags();
	uint8_t lf = '\n';

	if ((ctx == NULL) ||
	    (!ctx->duplicate_to_console &&
	     (!rcp_vserial_log_transport_ready() || rcp_vserial_log_backend_in_backoff(ctx)))) {
		return;
	}

	ctx->suppress_message = false;
	ctx->line_terminated = true;

	flags &= ~LOG_OUTPUT_FLAG_COLORS;
	flags |= LOG_OUTPUT_FLAG_CRLF_LFONLY;

	log_output_func = log_format_func_t_get(ctx->log_format_current);
	log_output_func(&rcp_vserial_log_output, &msg->log, flags);

	if (!ctx->line_terminated && !ctx->suppress_message) {
		(void)rcp_vserial_log_char_out(&lf, 1U, ctx);
	}
}

static void rcp_vserial_log_backend_panic(const struct log_backend *const backend)
{
	struct rcp_vserial_log_backend_ctx *ctx = backend->cb->ctx;

	ctx->panic = true;
	log_backend_std_panic(&rcp_vserial_log_output);
}

static void rcp_vserial_log_backend_init(const struct log_backend *const backend)
{
	struct rcp_vserial_log_backend_ctx *ctx = backend->cb->ctx;

	log_output_ctx_set(&rcp_vserial_log_output, (void *)ctx);

#if DT_HAS_CHOSEN(zephyr_console)
	ctx->console_uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
#endif
}

static int rcp_vserial_log_backend_format_set(const struct log_backend *const backend,
					      uint32_t log_type)
{
	struct rcp_vserial_log_backend_ctx *ctx = backend->cb->ctx;

	ctx->log_format_current = log_type;
	return 0;
}

static const struct log_backend_api rcp_vserial_log_backend_api = {
	.process = rcp_vserial_log_backend_process,
	.panic = rcp_vserial_log_backend_panic,
	.init = rcp_vserial_log_backend_init,
	.dropped = NULL,
	.format_set = rcp_vserial_log_backend_format_set,
};

LOG_BACKEND_DEFINE(rcp_vserial_log_backend, rcp_vserial_log_backend_api,
		   IS_ENABLED(CONFIG_QUALCOMM_RCP_APP_VSERIAL_LOG), (void *)&rcp_vserial_log_ctx);
