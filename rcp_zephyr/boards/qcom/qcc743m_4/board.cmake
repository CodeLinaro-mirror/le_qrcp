# Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries
# SPDX-License-Identifier: Apache-2.0

board_set_flasher_ifnset(qconn_flash)
board_finalize_runner_args(qconn_flash)

get_filename_component(ZEPHYR_HAL_QCOM_MODULE_DIR "${ZEPHYR_BASE}/../modules/hal/hal_qcom/qcc7xx" ABSOLUTE)
set(QC7XX_SDK_PATH ${ZEPHYR_HAL_QCOM_MODULE_DIR} CACHE STRING "QC7XX SDK path")
assert(QC7XX_SDK_PATH "QC7XX_SDK_PATH is not set")
