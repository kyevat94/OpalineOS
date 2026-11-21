/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * The CMOS real-time clock (ports 0x70/0x71), read once at boot.
 *
 * Registers: 0 seconds, 2 minutes, 4 hours, 7 day, 8 month, 9 year, 0x32 century
 * (present on QEMU, VirtualBox and practically every PC since the 1990s). Register
 * B says whether values are BCD or binary and whether hours are 12- or 24-hour.
 * The clock is read twice until both readings agree, so an update in the middle
 * cannot give a mixed time. The RTC holds UTC (the test VMs are set up that way;
 * dual boot with Windows, which keeps local time there, is handled in stage E14).
 */
#include <opaline/arch/cpu.h>
#include <opaline/console.h>
#include <opaline/sched.h>
#include <opaline/spinlock.h>
#include <opaline/time.h>

static u64 boot_epoch_ns;   /* real time at monotonic time 0 */
static spinlock_t lock = SPINLOCK_INIT;

static u8 cmos(u8 reg) {
    outb(0x70, reg | 0x80);   /* bit 7 keeps NMIs disabled while selecting */
    return inb(0x71);
}

static bool updating(void) { return cmos(0x0A) & 0x80; }

struct rtc_time {
    u8 sec, min, hour, day, month, year, century;
};

static void read_raw(struct rtc_time *t) {
    for (int i = 0; i < 100000 && updating(); i++) {}
    t->sec = cmos(0x00);
    t->min = cmos(0x02);
    t->hour = cmos(0x04);
    t->day = cmos(0x07);
    t->month = cmos(0x08);
    t->year = cmos(0x09);
    t->century = cmos(0x32);
}

static u8 bcd(u8 v) { return (u8)((v & 0x0F) + (v >> 4) * 10); }

u64 time_from_date(u32 year, u32 month, u32 day, u32 hour, u32 minute, u32 second) {
    /* days since 1970-01-01 (Howard Hinnant's days_from_civil) */
    i64 y = (i64)year - (month <= 2);
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * ((i64)month + (month > 2 ? -3 : 9)) + 2) / 5 + (i64)day - 1;
    i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    i64 days = era * 146097 + doe - 719468;
    return (u64)(days * 86400 + (i64)hour * 3600 + (i64)minute * 60 + (i64)second);
}

void time_init(void) {
    struct rtc_time a, b;
    read_raw(&b);
    for (int tries = 0; tries < 10; tries++) {
        a = b;
        read_raw(&b);
        if (a.sec == b.sec && a.min == b.min && a.hour == b.hour && a.day == b.day && a.month == b.month &&
            a.year == b.year && a.century == b.century)
            break;
    }
    u8 regb = cmos(0x0B);
    bool pm = b.hour & 0x80;
    b.hour &= 0x7F;
    if (!(regb & 0x04)) {   /* BCD */
        b.sec = bcd(b.sec), b.min = bcd(b.min), b.hour = bcd(b.hour);
        b.day = bcd(b.day), b.month = bcd(b.month), b.year = bcd(b.year), b.century = bcd(b.century);
    }
    if (!(regb & 0x02)) b.hour = (u8)(b.hour % 12 + (pm ? 12 : 0));   /* 12-hour clock */
    u32 century = b.century >= 19 && b.century <= 30 ? b.century : 20;
    u32 year = century * 100 + b.year;
    u64 secs = 0;
    if (b.month >= 1 && b.month <= 12 && b.day >= 1 && b.day <= 31 && b.hour < 24 && b.min < 60 && b.sec < 61)
        secs = time_from_date(year, b.month, b.day, b.hour, b.min, b.sec);
    u64 f = spin_lock(&lock);
    boot_epoch_ns = secs * NS_PER_SEC - MIN(secs * NS_PER_SEC, time_monotonic_ns());
    spin_unlock(&lock, f);
    kprintf("Clock: %04u-%02u-%02u %02u:%02u:%02u UTC (from the RTC)\n", year, b.month, b.day, b.hour, b.min, b.sec);
}

u64 time_monotonic_ns(void) { return sched_ticks() * (NS_PER_SEC / TIMER_HZ); }

u64 time_realtime_ns(void) {
    u64 f = spin_lock(&lock);
    u64 base = boot_epoch_ns;
    spin_unlock(&lock, f);
    return base + time_monotonic_ns();
}

void time_set_realtime_ns(u64 ns) {
    u64 now = time_monotonic_ns();
    u64 f = spin_lock(&lock);
    boot_epoch_ns = ns > now ? ns - now : 0;
    spin_unlock(&lock, f);
}
