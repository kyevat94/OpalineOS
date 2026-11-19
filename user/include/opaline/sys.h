/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Minimal user-space library for the first OpalineOS programs (stage E4).
 * It is replaced by a real C library (musl) in stage E6.
 *
 * System calls use the Linux x86-64 convention: RAX = number, arguments in
 * RDI, RSI, RDX, R10, R8, R9; the result (or -errno) comes back in RAX.
 */
#ifndef OPALINE_USER_SYS_H
#define OPALINE_USER_SYS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define SYS_write          1
#define SYS_sched_yield   24
#define SYS_nanosleep     35
#define SYS_getpid        39
#define SYS_exit          60
#define SYS_gettid       186
#define SYS_clock_gettime 228
#define SYS_exit_group   231

#define EBADF   9
#define EFAULT 14
#define EINVAL 22
#define ENOSYS 38

static inline long syscall0(long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory");
    return r;
}

static inline long syscall1(long n, long a) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory");
    return r;
}

static inline long syscall2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}

static inline long syscall3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

struct timespec {
    long tv_sec;
    long tv_nsec;
};

static inline long write(int fd, const void *buf, size_t n) { return syscall3(SYS_write, fd, (long)buf, (long)n); }
static inline long getpid(void) { return syscall0(SYS_getpid); }
static inline long gettid(void) { return syscall0(SYS_gettid); }
static inline long sched_yield(void) { return syscall0(SYS_sched_yield); }
static inline long nanosleep(const struct timespec *req, struct timespec *rem) {
    return syscall2(SYS_nanosleep, (long)req, (long)rem);
}
static inline long clock_gettime(int clock, struct timespec *tp) { return syscall2(SYS_clock_gettime, clock, (long)tp); }

_Noreturn void _exit(int code);
void sleep_ms(long ms);
long time_ms(void);
size_t strlen(const char *s);
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
