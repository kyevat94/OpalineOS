/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Kernel console: UTF-8 text on the framebuffer and on COM1.
 *
 * kprintf() understands %s %c %d %i %u %x %X %p %%, with the flags '0' and
 * '-', a width, and the length modifiers l, ll and z.
 */
#ifndef OPALINE_CONSOLE_H
#define OPALINE_CONSOLE_H

#include <opaline/types.h>

/* Theme colors (project.md, section 6.1). */
#define COLOR_BACKGROUND  0x0E1424
#define COLOR_TEXT        0xF2F4F8
#define COLOR_SECONDARY   0xA9B3C4
#define COLOR_ACCENT      0xF28B82
#define COLOR_SUCCESS     0x5BD69A
#define COLOR_WARNING     0xF5C451
#define COLOR_ERROR       0xF0606A
#define COLOR_INFO        0x5AA9F7

/* Sets up the screen part of the console. Without a font, only COM1 is used. */
void console_init(const void *font, size_t font_size);
bool console_has_screen(void);
void console_clear(u32 background);
void console_set_color(u32 fg);
void console_write(const char *utf8, size_t n);

int kprintf(const char *fmt, ...) PRINTF(1, 2);
void kputs(const char *s, size_t n);   /* raw text (UTF-8), as one locked write */
int kvprintf(const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) PRINTF(3, 4);

#endif
