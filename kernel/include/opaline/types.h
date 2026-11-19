/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Basic types and helpers used everywhere in the kernel. */
#ifndef OPALINE_TYPES_H
#define OPALINE_TYPES_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

#define PACKED      __attribute__((packed))
#define NORETURN    __attribute__((noreturn))
#define UNUSED      __attribute__((unused))
#define ALIGNED(n)  __attribute__((aligned(n)))
#define PRINTF(f, a) __attribute__((format(printf, f, a)))

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((u64)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define KiB ((u64)1024)
#define MiB (1024 * KiB)
#define GiB (1024 * MiB)

#endif
