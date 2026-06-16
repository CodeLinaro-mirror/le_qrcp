# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

option(RCP_OS_ZEPHYR "Build RCP for Zephyr platform" ON)

if (CONFIG_QUALCOMM_RCP_ENCRYPTION)
option(RCP_ENCRYPTION "Build RCP encryption" ON)
endif()

add_subdirectory(
    ${ZEPHYR_RCP_PROTOCOL_MODULE_DIR} rcp_protocol
)

# Path to config file
target_include_directories(qualcomm_rcp_protocol
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include/
)

# Zephyr headers
target_link_libraries(qualcomm_rcp_protocol PRIVATE zephyr_interface)

# Link with external qualcomm protocol library
zephyr_link_libraries(qualcomm_rcp_protocol)
