/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/* Kernel stub for stdint.h — fixed-width types from linux/types.h */
#ifndef _STDINT_H
#define _STDINT_H
#include <linux/types.h>
/* linux/types.h provides u8/u16/u32/u64; map to stdint names */
#ifndef uint8_t
typedef __u8  uint8_t;
#endif
#ifndef uint16_t
typedef __u16 uint16_t;
#endif
#ifndef uint32_t
typedef __u32 uint32_t;
#endif
#ifndef uint64_t
typedef __u64 uint64_t;
#endif
#ifndef int8_t
typedef __s8  int8_t;
#endif
#ifndef int16_t
typedef __s16 int16_t;
#endif
#ifndef int32_t
typedef __s32 int32_t;
#endif
#ifndef int64_t
typedef __s64 int64_t;
#endif
/* Least-width types (used by nanopb) */
typedef __u16 uint_least16_t;
typedef __s16 int_least16_t;
typedef __u8  uint_least8_t;
typedef __s8  int_least8_t;
typedef __u32 uint_least32_t;
typedef __s32 int_least32_t;
typedef __u64 uint_least64_t;
typedef __s64 int_least64_t;
/* Fast types (nanopb pb_decode.c uses uint_fast8_t) */
typedef __u8  uint_fast8_t;
typedef __s8  int_fast8_t;
typedef __u32 uint_fast16_t;
typedef __s32 int_fast16_t;
typedef __u32 uint_fast32_t;
typedef __s32 int_fast32_t;
typedef __u64 uint_fast64_t;
typedef __s64 int_fast64_t;
#ifndef UINT8_MAX
#define UINT8_MAX  0xFF
#endif
#ifndef UINT16_MAX
#define UINT16_MAX 0xFFFF
#endif
#ifndef UINT32_MAX
#define UINT32_MAX 0xFFFFFFFFU
#endif
#ifndef SIZE_MAX
#define SIZE_MAX   (~(size_t)0)
#endif
#endif
