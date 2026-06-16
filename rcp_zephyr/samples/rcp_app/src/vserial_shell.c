/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <rcp_interface.h>

LOG_MODULE_DECLARE(rcp_app, CONFIG_QUALCOMM_RCP_APP_LOG_LEVEL);

#define VSERIAL_SHELL_LINE_MAX 128
#define VSERIAL_ECHO_STACK_SIZE 1024
#define VSERIAL_ECHO_SYNC_WAIT_MS 20
#define VSERIAL_ECHO_FIRST_TX_SETTLE_MS 10
#define VSERIAL_SHELL_UART_NODE DT_ALIAS(rcp_vserial0)

#if !DT_HAS_ALIAS(rcp_vserial0)
#error "vserial_shell requires DT alias rcp-vserial0"
#endif

static struct k_thread vserial_echo_thread_data;
static bool vserial_echo_running;
static bool vserial_echo_first_response_pending = true;
static const struct device *vserial_shell_uart_dev;
/* Static storage keeps deferred LOG_* formatting safe for this test command. */
static char vlog_test_line[VSERIAL_SHELL_LINE_MAX];

static int vserial_uart_write(const struct device *dev, const uint8_t *data, size_t len)
{
	if ((dev == NULL) || (data == NULL)) {
		return -EINVAL;
	}

	for (size_t i = 0; i < len; i++) {
		uart_poll_out(dev, data[i]);
	}

	return (int)len;
}

static bool vserial_log_stream_ready(void)
{
	/*
	 * In the single-channel log flow the host explicitly syncs VSerial0 only when
	 * log forwarding is enabled. Refuse manual writes before the stream reaches
	 * ACTIVE to avoid accidentally starting sync traffic against a disabled host.
	 */
	return link_get_state(RCP_STREAM_VSERIAL0) == LINK_STATE_ACTIVE;
}

static int cmd_vserial_tx0(const struct shell *shell, size_t argc, char **argv)
{
	static const char default_msg[] = "hello from qcc743 vserial0";
	char line[VSERIAL_SHELL_LINE_MAX];
	size_t used = 0U;
	int rc;

	if (argc <= 1U) {
		used = (size_t)snprintf(line, sizeof(line), "%s\n", default_msg);
	} else {
		for (size_t i = 1U; i < argc; i++) {
			int written;

			written = snprintf(&line[used], sizeof(line) - used, "%s%s",
					   (i > 1U) ? " " : "", argv[i]);
			if ((written < 0) || ((size_t)written >= (sizeof(line) - used))) {
				shell_error(shell, "Message too long (max %u chars)",
					    (unsigned int)(sizeof(line) - 2U));
				return -EINVAL;
			}
			used += (size_t)written;
		}

		if ((used == 0U) || (line[used - 1U] != '\n')) {
			if (used >= (sizeof(line) - 1U)) {
				shell_error(shell, "Message too long (newline missing)");
				return -EINVAL;
			}
			line[used++] = '\n';
			line[used] = '\0';
		}
	}

	if (used >= sizeof(line)) {
		shell_error(shell, "Message too long");
		return -EINVAL;
	}

	if (vserial_shell_uart_dev == NULL) {
		shell_error(shell, "VSerial0 UART device not available");
		return -ENODEV;
	}

	if (!vserial_log_stream_ready()) {
		shell_error(shell,
			    "VSerial0 log stream is not active; host logging is probably disabled");
		return -ENOTCONN;
	}

	rc = vserial_uart_write(vserial_shell_uart_dev, (const uint8_t *)line, used);
	if (rc < 0) {
		shell_error(shell, "VSerial0 write failed (%d)", rc);
		return rc;
	}

	shell_print(shell, "Sent %d bytes on VSerial0", rc);
	return 0;
}

static int cmd_vlog_test(const struct shell *shell, size_t argc, char **argv)
{
	size_t used = 0U;

	if (!IS_ENABLED(CONFIG_QUALCOMM_RCP_APP_VSERIAL_LOG)) {
		shell_error(shell, "CONFIG_QUALCOMM_RCP_APP_VSERIAL_LOG is disabled");
		return -ENOSYS;
	}

	if (!vserial_log_stream_ready()) {
		shell_error(shell,
			    "VSerial0 log stream is not active; host logging is probably disabled");
		return -ENOTCONN;
	}

	if (argc <= 1U) {
		used = (size_t)snprintf(vlog_test_line, sizeof(vlog_test_line),
					"minimal vserial log test");
	} else {
		for (size_t i = 1U; i < argc; i++) {
			int written = snprintf(&vlog_test_line[used],
					       sizeof(vlog_test_line) - used, "%s%s",
					       (i > 1U) ? " " : "", argv[i]);
			if ((written < 0) ||
			    ((size_t)written >= (sizeof(vlog_test_line) - used))) {
				shell_error(shell, "Message too long (max %u chars)",
					    (unsigned int)(sizeof(vlog_test_line) - 1U));
				return -EINVAL;
			}
			used += (size_t)written;
		}
	}

	LOG_INF("VLOG_TEST: %s", vlog_test_line);
	shell_print(shell, "Queued LOG_INF on VSerial0 log backend");
	return 0;
}

static void vserial_echo_thread(void *arg1, void *arg2, void *arg3)
{
	const struct device *dev = (const struct device *)arg1;
	uint8_t line[VSERIAL_SHELL_LINE_MAX];
	size_t used = 0U;

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		unsigned char c;

		if (uart_poll_in(dev, &c) == 0) {
			if (c == '\r') {
				continue;
			}

			if (used < sizeof(line)) {
				line[used++] = c;
			}

			if ((c == '\n') || (used == sizeof(line))) {
				int rc;

				if (vserial_echo_first_response_pending) {
					for (uint32_t waited = 0U; waited < VSERIAL_ECHO_SYNC_WAIT_MS;
					     waited++) {
						if (link_get_state(RCP_STREAM_VSERIAL0) ==
						    LINK_STATE_ACTIVE) {
							break;
						}
						k_msleep(1);
					}
					k_msleep(VSERIAL_ECHO_FIRST_TX_SETTLE_MS);
					vserial_echo_first_response_pending = false;
				}

				rc = vserial_uart_write(dev, line, used);

				if (rc < 0) {
					LOG_ERR("VSerial0 echo write failed (%d)", rc);
				} else {
					LOG_INF("VSerial0 echoed %u bytes", (unsigned int)used);
				}
				used = 0U;
			}
		} else {
			k_msleep(1);
		}
	}
}

static int cmd_vserial_echo_start(const struct shell *shell, size_t argc, char **argv)
{
	const struct device *dev;
	k_thread_stack_t *vserial_echo_stack;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (IS_ENABLED(CONFIG_QUALCOMM_RCP_APP_VSERIAL_LOG)) {
		shell_error(shell, "VSerial0 echo is disabled while single-channel log backend is enabled");
		return -EBUSY;
	}

	if (vserial_echo_running) {
		shell_print(shell, "VSerial0 echo thread already running");
		return 0;
	}

	dev = vserial_shell_uart_dev;
	if (dev == NULL) {
		shell_error(shell, "VSerial0 UART device not available");
		return -ENODEV;
	}

	vserial_echo_stack = k_thread_stack_alloc(VSERIAL_ECHO_STACK_SIZE, 0);
	if (vserial_echo_stack == NULL) {
		shell_error(shell, "Failed to allocate VSerial0 echo stack");
		return -ENOMEM;
	}

	vserial_echo_first_response_pending = true;
	vserial_echo_running = true;
	(void)k_thread_create(&vserial_echo_thread_data, vserial_echo_stack,
			      VSERIAL_ECHO_STACK_SIZE, vserial_echo_thread,
			      (void *)dev, NULL, NULL, K_PRIO_COOP(8), 0, K_NO_WAIT);
	k_thread_name_set(&vserial_echo_thread_data, "vserial_echo");
	shell_print(shell, "VSerial0 echo thread started");
	return 0;
}

void vserial_shell_init(void)
{
	vserial_echo_running = false;
	vserial_echo_first_response_pending = true;

	vserial_shell_uart_dev = DEVICE_DT_GET(VSERIAL_SHELL_UART_NODE);
	if (!device_is_ready(vserial_shell_uart_dev)) {
		LOG_ERR("VSerial0 UART device is not ready");
		vserial_shell_uart_dev = NULL;
	}
}

SHELL_CMD_REGISTER(vserial_tx0, NULL,
		   "Send a raw line on QCC VSerial0. Usage: vserial_tx0 [text]",
		   cmd_vserial_tx0);
SHELL_CMD_REGISTER(vlog_test, NULL,
		   "Emit one LOG_INF line to the VSerial log backend. Usage: vlog_test [text]",
		   cmd_vlog_test);
SHELL_CMD_REGISTER(vserial_echo_start, NULL,
		   "Legacy manual VSerial0 echo thread (disabled in single-channel log mode)",
		   cmd_vserial_echo_start);
