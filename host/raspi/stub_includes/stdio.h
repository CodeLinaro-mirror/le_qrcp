/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/* Kernel stub for stdio.h — printf via pr_info */
#ifndef _STDIO_H
#define _STDIO_H
#include <linux/kernel.h>
#ifndef printf
#define printf(fmt, ...) pr_info(fmt, ##__VA_ARGS__)
#endif
#ifndef fprintf
#define fprintf(stream, fmt, ...) pr_err(fmt, ##__VA_ARGS__)
#endif
#endif
