/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stdint.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>

#ifdef CONFIG_NET_L2_OPENTHREAD
#include <zephyr/net/openthread.h>
#endif

#include <rcp_interface_impl.h>
#include <rcp_cntrl_msgs.h>
#include <rcp_encryption.h>
#include <rcp_config.h>

#include "proxy_bt.h"
#include "proxy_wifi_data.h"
#include "proxy_wifi_mgmt.h"
#include "vserial_shell.h"

LOG_MODULE_REGISTER(rcp_app, CONFIG_QUALCOMM_RCP_APP_LOG_LEVEL);

#define RCP_VSERIAL_PORT_COUNT DT_NUM_INST_STATUS_OKAY(qcom_rcp_vserial_uart)
#define RCP_INIT_PRIORITY CONFIG_QUALCOMM_RCP_INIT_PRIORITY

static int sys_init_rcp()
{
	(void)init_rcp();
	return 0;
}

/* Initialize RCP early (specifically before vserial & ieee802154) so
 * that other drivers can access RCP resources.
 * Note that messages will not be processed until event_loop() starts.
 */
SYS_INIT(sys_init_rcp, POST_KERNEL, RCP_INIT_PRIORITY);

#ifdef CONFIG_NET_L2_OPENTHREAD

static void bring_up_ot_iface(void)
{
	struct net_if *iface;
	int ret;

	iface = net_if_get_first_by_type(&NET_L2_GET_NAME(OPENTHREAD));
	if (iface == NULL) {
		LOG_WRN("OpenThread net_if not found");
		return;
	}

	ret = net_if_up(iface);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to bring OpenThread iface up (%d)", ret);
		return;
	}

	LOG_INF("OpenThread iface is up");
}
#endif

int main(void)
{
	uint8_t capabilities = 0;

#ifdef CONFIG_BT
	if (!proxy_bt_init()) {
		LOG_ERR("Failed to initialize BT proxy");
		return 0;
	}
	capabilities |= RCP_BT_CAPABILITY_FLAG;
#endif
#ifdef CONFIG_WIFI
	if (!proxy_wifi_mgmt_init()) {
		LOG_ERR("Failed to initialize Wi-Fi proxy");
		return 0;
	}
	if (!proxy_wifi_data_init()) {
		LOG_ERR("Failed to initialize Wi-Fi proxy");
		return 0;
	}
	capabilities |= RCP_WIFI_CAPABILITY_FLAG;
#endif
	uint8_t port_count = RCP_VSERIAL_PORT_COUNT;

#ifdef CONFIG_NET_L2_OPENTHREAD
	/* One of the ports is reserved for Thread.
	* TODO: Make sure that ports are continous, especially if they
	* are reserved for some technology.
	*/
	if (RCP_STREAM_THREAD_USE_VSERIAL) {
		port_count -= 1;
	}
	bring_up_ot_iface();
	capabilities |= RCP_THREAD_CAPABILITY_FLAG;
#endif
	init_mcu_capability(capabilities);
	init_mcu_vserial_port_count(port_count);
	vserial_shell_init();
	/* Run link event loop. */
	run_event_loop();
}
