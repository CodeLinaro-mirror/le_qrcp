# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_QUALCOMM_RCP_BUS_SDIO_SLAVE
    ${CMAKE_CURRENT_LIST_DIR}/rcp_bus_sdio_slave.c
)

zephyr_library_sources_ifdef(CONFIG_QUALCOMM_RCP_BUS_SPI_SLAVE
    ${CMAKE_CURRENT_LIST_DIR}/rcp_bus_spi_slave.c
)

zephyr_library_sources_ifdef(CONFIG_QCC730_RCP_BUS_QCSPI
    ${CMAKE_CURRENT_LIST_DIR}/rcp_bus_spi_slave_qcc730.c
)
