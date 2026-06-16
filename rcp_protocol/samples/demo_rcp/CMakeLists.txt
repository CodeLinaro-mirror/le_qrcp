# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

cmake_minimum_required(VERSION 3.15)
project(demo_rcp C)

option(RCP_OS_LINUX "Build RCP for Linux platform" ON)

add_subdirectory(../../ rcp_protocol)

target_include_directories(qualcomm_rcp_protocol PRIVATE ${CMAKE_CURRENT_LIST_DIR})

add_executable(demo_rcp_host
	${CMAKE_CURRENT_LIST_DIR}/demo_rcp_host.c
	${CMAKE_CURRENT_LIST_DIR}/bus_socket.c
	${CMAKE_CURRENT_LIST_DIR}/demo_utils.c
)

add_executable(demo_rcp_mcu
	${CMAKE_CURRENT_LIST_DIR}/demo_rcp_mcu.c
	${CMAKE_CURRENT_LIST_DIR}/bus_socket.c
	${CMAKE_CURRENT_LIST_DIR}/demo_utils.c
)

add_executable(test_rcp_sync_host
	${CMAKE_CURRENT_LIST_DIR}/test_rcp_sync_host.c
	${CMAKE_CURRENT_LIST_DIR}/bus_socket.c
	${CMAKE_CURRENT_LIST_DIR}/demo_utils.c
)

add_executable(test_buffers
    ${CMAKE_CURRENT_LIST_DIR}/test_buffers.c
)

target_link_libraries(
    demo_rcp_host PRIVATE
        qualcomm_rcp_protocol
)

add_executable(test_rcp_sync_mcu
	${CMAKE_CURRENT_LIST_DIR}/test_rcp_sync_mcu.c
	${CMAKE_CURRENT_LIST_DIR}/bus_socket.c
	${CMAKE_CURRENT_LIST_DIR}/demo_utils.c
)

add_executable(test_rcp_data_host
	${CMAKE_CURRENT_LIST_DIR}/test_rcp_data_host.c
	${CMAKE_CURRENT_LIST_DIR}/bus_socket.c
	${CMAKE_CURRENT_LIST_DIR}/demo_utils.c
)

add_executable(test_rcp_cntrl_host
	${CMAKE_CURRENT_LIST_DIR}/test_rcp_cntrl_host.c
	${CMAKE_CURRENT_LIST_DIR}/bus_socket.c
	${CMAKE_CURRENT_LIST_DIR}/demo_utils.c
)

add_executable(test_rcp_data_mcu
	${CMAKE_CURRENT_LIST_DIR}/test_rcp_data_mcu.c
	${CMAKE_CURRENT_LIST_DIR}/bus_socket.c
	${CMAKE_CURRENT_LIST_DIR}/demo_utils.c
)

add_executable(test_rcp_cntrl_mcu
	${CMAKE_CURRENT_LIST_DIR}/test_rcp_cntrl_mcu.c
	${CMAKE_CURRENT_LIST_DIR}/bus_socket.c
	${CMAKE_CURRENT_LIST_DIR}/demo_utils.c
)
target_link_libraries(demo_rcp_host PRIVATE qualcomm_rcp_protocol)

target_link_libraries(demo_rcp_mcu PRIVATE qualcomm_rcp_protocol)

target_link_libraries(test_rcp_sync_host PRIVATE qualcomm_rcp_protocol)

target_link_libraries(test_rcp_sync_mcu PRIVATE qualcomm_rcp_protocol)

target_link_libraries(test_rcp_cntrl_host PRIVATE qualcomm_rcp_protocol)

target_link_libraries(test_rcp_cntrl_mcu PRIVATE qualcomm_rcp_protocol)

target_link_libraries(
    test_buffers PRIVATE
        qualcomm_rcp_protocol
)

target_link_libraries(test_rcp_data_host PRIVATE qualcomm_rcp_protocol)

target_link_libraries(test_rcp_data_mcu PRIVATE qualcomm_rcp_protocol)
