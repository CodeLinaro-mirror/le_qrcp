# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

option(RCP_OS_ZEPHYR "Enable Zephyr OS bindings" OFF)
option(RCP_OS_LINUX "Enable Linux OS bindings" OFF)
option(RCP_OS_FREERTOS "Enable FreeRTOS OS bindings" OFF)
option(RCP_WIFI_MGMT "Enable generic RCP WiFi management module" OFF)
option(RCP_ENCRYPTION "Enable RCP message encryption support" OFF)

add_library(qualcomm_rcp_protocol
    ${CMAKE_CURRENT_LIST_DIR}/src/rcp_bus.c
    ${CMAKE_CURRENT_LIST_DIR}/src/rcp_interface_impl.c
    ${CMAKE_CURRENT_LIST_DIR}/src/rcp_protocol.c
    ${CMAKE_CURRENT_LIST_DIR}/src/rcp_mux_dispatcher.c
    ${CMAKE_CURRENT_LIST_DIR}/src/rcp_tx_scheduler.c
    ${CMAKE_CURRENT_LIST_DIR}/src/buffer_mgmt.c
    ${CMAKE_CURRENT_LIST_DIR}/src/rcp_cntrl_msgs.c
    ${CMAKE_CURRENT_LIST_DIR}/src/rcp_error_codes.c
    ${CMAKE_CURRENT_LIST_DIR}/src/rcp_event.c
    ${CMAKE_CURRENT_LIST_DIR}/src/rcp_bus_spi_header.c
)

target_include_directories(qualcomm_rcp_protocol PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include/
)

if (RCP_OS_LINUX)

target_sources(qualcomm_rcp_protocol PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/rcp_os_linux.c
)

endif()

if (RCP_OS_ZEPHYR)

target_sources(qualcomm_rcp_protocol PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/rcp_os_zephyr.c
)

endif()

if (RCP_OS_FREERTOS)

target_sources(qualcomm_rcp_protocol PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/rcp_os_freertos.c
)

endif()

if (RCP_WIFI_MGMT)

target_sources(qualcomm_rcp_protocol PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/src/wifi/rcp_wifi_mgmt.c
)

# nanopb
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/ext/nanopb/extra)
find_package(Nanopb REQUIRED)

set(PROTO_DIR ${CMAKE_CURRENT_LIST_DIR}/proto)

nanopb_generate_cpp(TARGET rcp_wifi_mgmt_proto RELPATH ${PROTO_DIR}
	${PROTO_DIR}/wifi_mgmt.proto
)

target_link_libraries(qualcomm_rcp_protocol PRIVATE rcp_wifi_mgmt_proto)

endif()

if (RCP_ENCRYPTION)

# Encryption requires an implementation of the PSA Crypto API.
# Each platform provides and links the relevant provider outside of this module.
# STM32 uses its standalone middleware Mbed TLS build, while Zephyr uses the
# OS-provided Mbed TLS integration.

target_sources(qualcomm_rcp_protocol PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/src/rcp_crypto.c
    ${CMAKE_CURRENT_LIST_DIR}/src/rcp_encryption.c
)

endif()
