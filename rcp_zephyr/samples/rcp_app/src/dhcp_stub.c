/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/* The precompiled Wi-Fi driver for QCC74x depends on auto DHCP negotiation
 * to finalize Wi-Fi connection.
 * To turn off DHCPV4 and all IP stack we need to:
 * - provide net_dhcpv4_start/stop stubs for the driver.
 * - emit a fake NET_EVENT_IPV4_DHCP_BOUND event.
 *
 * With this we can use the driver interface for sending / receiving raw
 * ethernet packets with IP stack fully disabled on the MCU side.
 */

#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>

void net_dhcpv4_start(struct net_if *iface)
{
	net_mgmt_event_notify(NET_EVENT_IPV4_DHCP_BOUND, iface);
}

void net_dhcpv4_stop(struct net_if *iface)
{
	return;
}
