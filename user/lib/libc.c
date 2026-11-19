/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Minimal C runtime for the stage E4 programs (replaced by musl in E6). */
#include <opaline/sys.h>

_Noreturn void _exit(int code) {
    for (;;) syscall1(SYS_exit_group, code);
}

void sleep_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}

long time_ms(void) {
    struct timespec t;
    clock_gettime(1, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ---- printf: %s %c %d %ld %u %lu %x %lx %p %% (to standard output) ---- */

struct out {
    char buf[256];
    size_t n;
};

static void put(struct out *o, char c) {
    o->buf[o->n++] = c;
    if (o->n == sizeof o->buf) {
        write(1, o->buf, o->n);
        o->n = 0;
    }
}

static void put_number(struct out *o, unsigned long v, unsigned base, int negative) {
    char tmp[24];
    int i = 0;
    do tmp[i++] = "0123456789abcdef"[v % base]; while (v /= base);
    if (negative) put(o, '-');
    while (i) put(o, tmp[--i]);
}

int printf(const char *fmt, ...) {
    struct out o = {.n = 0};
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        int is_long = 0;
        while (*++fmt == 'l') is_long = 1;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            for (s = s ? s : "(null)"; *s; s++) put(&o, *s);
            break;
        }
        case 'c': put(&o, (char)va_arg(ap, int)); break;
        case 'd': {
            long v = is_long ? va_arg(ap, long) : va_arg(ap, int);
            put_number(&o, v < 0 ? 0UL - (unsigned long)v : (unsigned long)v, 10, v < 0);
            break;
        }
        case 'u': put_number(&o, is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, 0); break;
        case 'x': put_number(&o, is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 0); break;
        case 'p':
            put(&o, '0');
            put(&o, 'x');
            put_number(&o, (unsigned long)va_arg(ap, void *), 16, 0);
            break;
        case '%': put(&o, '%'); break;
        default:
            put(&o, '%');
            if (!*fmt) goto done;
            put(&o, *fmt);
        }
    }
done:
    va_end(ap);
    if (o.n) write(1, o.buf, o.n);
    return 0;
}
