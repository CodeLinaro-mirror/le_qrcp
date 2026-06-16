# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

zephyr_library()
zephyr_library_include_directories(${CMAKE_CURRENT_LIST_DIR})

zephyr_library_sources_ifdef(CONFIG_QUALCOMM_RCP_VSERIAL_UART
    ${CMAKE_CURRENT_LIST_DIR}/uart_rcp_vserial.c
)
