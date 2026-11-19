/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Memory and string functions (kernel/lib/string.c). */
#ifndef OPALINE_STRING_H
#define OPALINE_STRING_H

#include <opaline/types.h>

void *memset(void *dst, int value, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
const char *strstr(const char *haystack, const char *needle);

/* True if `word` appears in the space-separated `list` (used for the command line). */
bool has_word(const char *list, const char *word);

#endif
