/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * Kernel portability shim for rcp_protocol sources compiled into a kernel module.
 * Auto-included via ccflags-y += -include in Kbuild.
 *
 * Provides:
 *   - Kernel type equivalents for <stdbool.h> / <stdint.h> / <string.h>
 *   - malloc/free  → kmalloc/kfree
 *   - printf       → pr_info (used only in debug/logging paths)
 *   - Blocks userspace <stdlib.h>, <stdio.h> from being included
 */

#ifndef RCP_KERNEL_COMPAT_H
#define RCP_KERNEL_COMPAT_H

#ifdef __KERNEL__

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/string.h>

/* Block userspace headers that rcp_protocol source files try to include.
 * Their include guards are defined here so the #include directives
 * become no-ops; kernel headers above already supply the needed types. */
#define _STDLIB_H       1
#define _STDIO_H        1
#define _STDBOOL_H      1
#define _STDINT_H       1
#define _STRING_H       1
/* GCC/glibc variants */
#define _STDLIB_H_      1
#define _STDIO_H_       1

/* Provide userspace-style memory allocation wrappers */
#ifndef malloc
#define malloc(sz)  kmalloc((sz), GFP_KERNEL)
#endif
#ifndef free
#define free(ptr)   kfree(ptr)
#endif

/* Redirect printf to kernel printk */
#ifndef printf
#define printf(fmt, ...) pr_info(fmt, ##__VA_ARGS__)
#endif

/* bool / true / false — kernel defines these via <linux/types.h> */

#endif /* __KERNEL__ */

#endif /* RCP_KERNEL_COMPAT_H */
