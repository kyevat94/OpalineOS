/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Fatal errors: show a diagnostic on screen and COM1, then stop the machine. */
#ifndef OPALINE_PANIC_H
#define OPALINE_PANIC_H

#include <opaline/types.h>

struct interrupt_frame;

NORETURN void panic(const char *fmt, ...) PRINTF(1, 2);
bool panic_in_progress(void);
NORETURN void panic_exception(const struct interrupt_frame *f);

#define ASSERT(cond)                                                              \
    do {                                                                          \
        if (!(cond)) panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__); \
    } while (0)

#endif
