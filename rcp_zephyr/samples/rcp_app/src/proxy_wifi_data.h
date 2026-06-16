/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __PROXY_WIFI_DATA_H__
#define __PROXY_WIFI_DATA_H__

#include <stdbool.h>

#define PROXY_WIFI_IFACE_STA 1U
#define PROXY_WIFI_IFACE_AP  2U

#define PROXY_WIFI_DATA_HDR_VERSION 1U
#define PROXY_WIFI_DATA_HDR_SIZE    2U

bool proxy_wifi_data_init(void);

#endif
