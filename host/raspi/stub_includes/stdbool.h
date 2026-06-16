/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/* Kernel stub for stdbool.h — bool/true/false from linux/types.h */
#ifndef _STDBOOL_H
#define _STDBOOL_H
#include <linux/types.h>
#ifndef bool
#define bool _Bool
#endif
#ifndef true
#define true  1
#endif
#ifndef false
#define false 0
#endif
#endif
