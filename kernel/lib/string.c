/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
#include <opaline/string.h>

void *memset(void *dst, int value, size_t n) {
    u8 *d = dst;
    while (n--) *d++ = (u8)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    u8 *d = dst;
    const u8 *s = src;
    while (n >= 8) {
        *(u64 *)d = *(const u64 *)s;
        d += 8;
        s += 8;
        n -= 8;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    u8 *d = dst;
    const u8 *s = src;
    if (d == s || n == 0) return dst;
    if (d < s) return memcpy(dst, src, n);
    d += n;
    s += n;
    while (n >= 8) {
        d -= 8;
        s -= 8;
        n -= 8;
        *(u64 *)d = *(const u64 *)s;
    }
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (u8)*a - (u8)*b;
        if (!*a) return 0;
    }
    return 0;
}

const char *strstr(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    for (; *haystack; haystack++)
        if (!strncmp(haystack, needle, n)) return haystack;
    return n ? NULL : haystack;
}

bool has_word(const char *list, const char *word) {
    size_t n = strlen(word);
    for (const char *p = list; *p;) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        if ((size_t)(p - start) == n && !strncmp(start, word, n)) return true;
    }
    return false;
}
