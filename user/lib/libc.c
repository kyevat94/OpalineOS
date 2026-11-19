/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Minimal C runtime for the stage E4-E5 programs (replaced by musl in E6). */
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

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strchr(const char *s, int c) {
    for (; *s; s++)
        if (*s == (char)c) return (char *)s;
    return c ? NULL : (char *)s;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
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

const char *strerror_short(long err) {
    switch (-err) {
    case ENOENT: return "ENOENT (no such file or directory)";
    case EBADF: return "EBADF (bad file descriptor)";
    case EEXIST: return "EEXIST (already exists)";
    case ENOTDIR: return "ENOTDIR (not a directory)";
    case EISDIR: return "EISDIR (is a directory)";
    case EINVAL: return "EINVAL (invalid argument)";
    case ENOTEMPTY: return "ENOTEMPTY (directory not empty)";
    case ENAMETOOLONG: return "ENAMETOOLONG (name too long)";
    case EFAULT: return "EFAULT (bad address)";
    case ENOSYS: return "ENOSYS (not implemented)";
    default: return "error";
    }
}

/* ---- formatted output: %s %c %d %ld %u %lu %x %lx %p %% --------------------------- */

struct out {
    char *buf;        /* NULL: write to fd */
    size_t size, n, total;
    int fd;
    char tmp[256];
};

static void put(struct out *o, char c) {
    o->total++;
    if (o->buf) {
        if (o->n + 1 < o->size) o->buf[o->n++] = c;
        return;
    }
    o->tmp[o->n++] = c;
    if (o->n == sizeof o->tmp) {
        write(o->fd, o->tmp, o->n);
        o->n = 0;
    }
}

static void put_number(struct out *o, unsigned long v, unsigned base, int negative, int width, char pad) {
    char tmp[24];
    int i = 0;
    do tmp[i++] = "0123456789abcdef"[v % base]; while (v /= base);
    if (negative) tmp[i++] = '-';
    for (; width > i; width--) put(o, pad);
    while (i) put(o, tmp[--i]);
}

static void format(struct out *o, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(o, *fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0, left = 0;
        if (*fmt == '-') left = 1, fmt++;
        if (*fmt == '0') pad = '0', fmt++;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int is_long = 0;
        while (*fmt == 'l') is_long = 1, fmt++;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            if (!left)
                for (int k = len; k < width; k++) put(o, ' ');
            while (*s) put(o, *s++);
            if (left)
                for (int k = len; k < width; k++) put(o, ' ');
            break;
        }
        case 'c': put(o, (char)va_arg(ap, int)); break;
        case 'd': {
            long v = is_long ? va_arg(ap, long) : va_arg(ap, int);
            put_number(o, v < 0 ? 0UL - (unsigned long)v : (unsigned long)v, 10, v < 0, width, pad);
            break;
        }
        case 'u': put_number(o, is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, 0, width, pad); break;
        case 'x': put_number(o, is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 0, width, pad); break;
        case 'p':
            put(o, '0');
            put(o, 'x');
            put_number(o, (unsigned long)va_arg(ap, void *), 16, 0, 0, ' ');
            break;
        case '%': put(o, '%'); break;
        default:
            put(o, '%');
            if (!*fmt) return;
            put(o, *fmt);
        }
    }
}

int dprintf(int fd, const char *fmt, ...) {
    struct out o = {.buf = NULL, .fd = fd};
    va_list ap;
    va_start(ap, fmt);
    format(&o, fmt, ap);
    va_end(ap);
    if (o.n) write(fd, o.tmp, o.n);
    return (int)o.total;
}

int printf(const char *fmt, ...) {
    struct out o = {.buf = NULL, .fd = 1};
    va_list ap;
    va_start(ap, fmt);
    format(&o, fmt, ap);
    va_end(ap);
    if (o.n) write(1, o.tmp, o.n);
    return (int)o.total;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    struct out o = {.buf = buf, .size = size};
    va_list ap;
    va_start(ap, fmt);
    format(&o, fmt, ap);
    va_end(ap);
    if (size) buf[o.n] = 0;
    return (int)o.total;
}
