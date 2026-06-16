# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

cmake_minimum_required(VERSION 3.20.0)

# Local rcp_protocol module
set(EXTRA_ZEPHYR_MODULES
    ${CMAKE_CURRENT_LIST_DIR}/../../../rcp_protocol
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})


project(rcp_app)

target_sources(app PRIVATE
    src/main.c
    src/vserial_shell.c
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../drivers/qualcomm_rcp_vserial
)

if (CONFIG_QUALCOMM_RCP_APP_VSERIAL_LOG)
target_sources(app PRIVATE
    src/rcp_vserial_log_backend.c
)
endif()

if (CONFIG_BT)
target_sources(app PRIVATE
    src/proxy_bt.c
)
endif()

if (CONFIG_WIFI)
target_sources(app PRIVATE
    src/proxy_wifi_data.c
    src/proxy_wifi_mgmt.c
)

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

# Protobuf for RCP WiFi management
zephyr_nanopb_sources(app ${ZEPHYR_RCP_PROTOCOL_MODULE_DIR}/proto/wifi_mgmt.proto)

endif()

if ((NOT CONFIG_NET_DHCPV4) AND CONFIG_WIFI_OPAL)
target_sources(app PRIVATE
    src/dhcp_stub.c
)
endif()
