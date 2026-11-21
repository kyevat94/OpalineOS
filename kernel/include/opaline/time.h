/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Wall-clock time (kernel/dev/rtc.c).
 *
 * At boot the kernel reads the battery-backed clock of the PC (the CMOS RTC,
 * kept in UTC) once; from then on the time is that reading plus the timer
 * ticks since. Monotonic time counts from boot and never jumps; real time can
 * be set (clock_settime, by root).
 */
#ifndef OPALINE_TIME_H
#define OPALINE_TIME_H

#include <opaline/types.h>

#define NS_PER_SEC 1000000000ULL

void time_init(void);                 /* reads the RTC; needs the timer running */
u64 time_monotonic_ns(void);          /* since boot */
u64 time_realtime_ns(void);           /* since 1970-01-01 00:00:00 UTC */
void time_set_realtime_ns(u64 ns);
/* Seconds since 1970 for a UTC date (month 1-12, day 1-31). */
u64 time_from_date(u32 year, u32 month, u32 day, u32 hour, u32 minute, u32 second);

#endif
