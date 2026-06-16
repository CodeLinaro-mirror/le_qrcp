/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/**
 * @file rcp_log.h
 * RCP logging helpers.
 *
 * @defgroup rcp_log RCP logging macros
 *
 * These logging macros provide common logging API for RCP modules implemented
 * using OS-specific logging mechanisms (e.g. printf(), LOG_*).
 *
 * @ingroup rcp_internal
 */
#ifndef __RCP_LOG_H__
#define __RCP_LOG_H__

#include "rcp_config.h"

#if defined(RCP_OS_ZEPHYR) && defined(RCP_LOG)

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(qualcomm_rcp, RCP_LOG_LEVEL);

#if !RCP_LOG_IGNORE_WARNINGS
#define RCP_LOG_ERR(...) LOG_ERR(__VA_ARGS__)
#define RCP_LOG_WRN(...) LOG_WRN(__VA_ARGS__)
#else
#define RCP_LOG_ERR(...)
#define RCP_LOG_WRN(...)
#endif

#define RCP_LOG_INF(...) LOG_INF(__VA_ARGS__)
#define RCP_LOG_DBG(...) LOG_DBG(__VA_ARGS__)

#elif (defined(RCP_OS_FREERTOS) || defined(RCP_OS_LINUX)) && defined(RCP_LOG)

#include <stdio.h>

#if RCP_LOG_LEVEL >= 4
#define RCP_LOG_DBG(msg, ...)                                                                      \
	printf("DBG: " __FILE_NAME__ ":%s: " msg "\n"                                                      \
			     "\x1b[0m",                                                            \
	       __func__, ##__VA_ARGS__)
#else
#define RCP_LOG_DBG(...)
#endif /* RCP_LOG_LEVEL >= 4 */

#if RCP_LOG_LEVEL >= 3
#define RCP_LOG_INF(msg, ...)                                                                      \
	printf("INF: " __FILE_NAME__ ":%s: " msg "\n"                                                      \
			     "\x1b[0m",                                                            \
	       __func__, ##__VA_ARGS__)
#else
#define RCP_LOG_INF(...)
#endif

#if RCP_LOG_LEVEL >= 2 && !RCP_LOG_IGNORE_WARNINGS
#define RCP_LOG_WRN(msg, ...) printf("\x1b[33m" "WRN: " __FILE_NAME__ ": " msg "\x1b[0m\n", ##__VA_ARGS__)
#else
#define RCP_LOG_WRN(...)
#endif /* RCP_LOG_LEVEL >= 2 */

#if RCP_LOG_LEVEL >= 1 && !RCP_LOG_IGNORE_WARNINGS
#define RCP_LOG_ERR(msg, ...) printf("\x1b[31m" "ERR: " __FILE_NAME__ ": " msg "\x1b[0m\n", ##__VA_ARGS__)
#else
#define RCP_LOG_ERR(...)
#endif /* RCP_LOG_LEVEL >= 1 */

#else
#define RCP_LOG_DBG(...)
#define RCP_LOG_INF(...)
#define RCP_LOG_WRN(...)
#define RCP_LOG_ERR(...)
#endif

#endif
