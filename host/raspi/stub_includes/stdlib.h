/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/* Kernel stub for stdlib.h — malloc/free via kmalloc/kfree */
#ifndef _STDLIB_H
#define _STDLIB_H
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/random.h>
#ifndef malloc
#define malloc(sz)  kmalloc((sz), GFP_KERNEL)
#endif
#ifndef free
#define free(ptr)   kfree(ptr)
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#endif
#ifndef EXIT_FAILURE
#define EXIT_FAILURE 1
#endif
/* Zephyr compat macro */
#ifndef ARG_UNUSED
#define ARG_UNUSED(x) (void)(x)
#endif
/* Random number stubs — kernel equivalents */
static inline void srand(unsigned int seed) { (void)seed; }
static inline int  rand(void) { return (int)(get_random_u32() & 0x7fffffff); }
#endif
