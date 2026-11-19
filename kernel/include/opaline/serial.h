/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* COM1 serial port: the kernel log, also read by the automated tests. */
#ifndef OPALINE_SERIAL_H
#define OPALINE_SERIAL_H

#include <opaline/types.h>

void serial_init(void);
void serial_write(const char *s, size_t n);   /* raw bytes (UTF-8 passes through) */

#endif
