# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

add_subdirectory(drivers/qualcomm_rcp)
add_subdirectory(drivers/qualcomm_rcp_vserial)

# Local rcp_protocol module
set(EXTRA_ZEPHYR_MODULES
    ${CMAKE_CURRENT_LIST_DIR}/../rcp_protocol
)
