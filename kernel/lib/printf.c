/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* printf-style formatting into a callback (no floating point). */
#include <opaline/console.h>
#include <opaline/panic.h>
#include <opaline/spinlock.h>
#include <opaline/string.h>

typedef void (*sink_fn)(void *ctx, char c);

static int emit_padded(sink_fn put, void *ctx, const char *s, size_t len, int width, bool left, char pad) {
    int n = 0;
    if (!left)
        for (; width > (int)len; width--, n++) put(ctx, pad);
    for (size_t i = 0; i < len; i++, n++) put(ctx, s[i]);
    if (left)
        for (; width > (int)len; width--, n++) put(ctx, ' ');
    return n;
}

static int format(sink_fn put, void *ctx, const char *fmt, va_list ap) {
    int total = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(ctx, *fmt);
            total++;
            continue;
        }
        fmt++;
        bool left = false, alt = false;
        char pad = ' ';
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') pad = '0';
            else if (*fmt == '#') alt = true;
            else break;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int longs = 0;
        bool size_t_arg = false;
        while (*fmt == 'l') longs++, fmt++;
        if (*fmt == 'z') size_t_arg = true, fmt++;

        char buf[24];
        char *end = buf + sizeof buf, *p = end;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            total += emit_padded(put, ctx, s, strlen(s), width, left, ' ');
            continue;
        }
        case 'c':
            buf[0] = (char)va_arg(ap, int);
            total += emit_padded(put, ctx, buf, 1, width, left, ' ');
            continue;
        case '%':
            put(ctx, '%');
            total++;
            continue;
        case 'd':
        case 'i': {
            i64 v = size_t_arg ? (i64)va_arg(ap, size_t) : longs ? va_arg(ap, i64) : va_arg(ap, int);
            u64 u = v < 0 ? (u64)(-(v + 1)) + 1 : (u64)v;
            do *--p = (char)('0' + u % 10); while (u /= 10);
            if (v < 0) {
                if (pad == '0' && width) {   /* sign goes before the zeros */
                    put(ctx, '-');
                    total++;
                    width--;
                } else {
                    *--p = '-';
                }
            }
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'p': {
            u64 v;
            if (*fmt == 'p') {
                v = (u64)va_arg(ap, void *);
                put(ctx, '0'), put(ctx, 'x');
                total += 2;
                pad = '0';
                width = 16;
            } else {
                v = size_t_arg ? va_arg(ap, size_t) : longs ? va_arg(ap, u64) : va_arg(ap, unsigned);
                if (alt && *fmt != 'u') {
                    put(ctx, '0'), put(ctx, 'x');
                    total += 2;
                }
            }
            unsigned base = *fmt == 'u' ? 10 : 16;
            const char *digits = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do *--p = digits[v % base]; while (v /= base);
            break;
        }
        default:   /* unknown: print it as is */
            put(ctx, '%');
            put(ctx, *fmt);
            total += 2;
            if (!*fmt) return total;
            continue;
        }
        total += emit_padded(put, ctx, p, (size_t)(end - p), width, left, pad);
    }
    return total;
}

/* ---- kprintf: buffered output to the console ------------------------- */

struct console_sink {
    char buf[256];
    size_t n;
};

static void console_put(void *ctx, char c) {
    struct console_sink *s = ctx;
    s->buf[s->n++] = c;
    if (s->n == sizeof s->buf) {
        console_write(s->buf, s->n);
        s->n = 0;
    }
}

/* One kprintf at a time, so lines from different threads do not interleave.
   Skipped during a panic: the panicking code may have been interrupted while holding it. */
static spinlock_t console_lock = SPINLOCK_INIT;

int kvprintf(const char *fmt, va_list ap) {
    bool locked = !panic_in_progress();
    u64 flags = locked ? spin_lock(&console_lock) : 0;
    struct console_sink s;
    s.n = 0;
    int n = format(console_put, &s, fmt, ap);
    if (s.n) console_write(s.buf, s.n);
    if (locked) spin_unlock(&console_lock, flags);
    return n;
}

int kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}

/* ---- ksnprintf: into a buffer ----------------------------------------- */

struct buffer_sink {
    char *buf;
    size_t size, n;
};

static void buffer_put(void *ctx, char c) {
    struct buffer_sink *s = ctx;
    if (s->n + 1 < s->size) s->buf[s->n] = c;
    s->n++;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    struct buffer_sink s = {buf, size, 0};
    va_list ap;
    va_start(ap, fmt);
    int n = format(buffer_put, &s, fmt, ap);
    va_end(ap);
    if (size) buf[MIN(s.n, size - 1)] = 0;
    return n;
}
