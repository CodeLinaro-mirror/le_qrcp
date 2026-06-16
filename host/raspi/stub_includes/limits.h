/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/* Kernel stub for limits.h — use linux/limits.h */
#ifndef _LIMITS_H
#define _LIMITS_H
#include <linux/limits.h>
/* Additional constants that nanopb expects */
#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif
#ifndef UINT_MAX
#define UINT_MAX (~0U)
#endif
#ifndef INT_MAX
#define INT_MAX ((int)(~0U >> 1))
#endif
#endif
