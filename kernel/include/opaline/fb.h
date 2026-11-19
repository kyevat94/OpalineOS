/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Linear framebuffer from the bootloader (GOP). Colors are 0xRRGGBB. */
#ifndef OPALINE_FB_H
#define OPALINE_FB_H

#include <opaline/types.h>
#include <boot/bootinfo.h>

struct fb {
    u8 *base;
    u32 width, height, pitch;
    u8 rs, gs, bs;          /* bit position of red, green and blue */
};

extern struct fb fb;

bool fb_init(const struct boot_framebuffer *info);
void fb_fill(u32 x, u32 y, u32 w, u32 h, u32 rgb);
/* Draws an 8-bit alpha mask in color `fg` over the solid color `bg`. */
void fb_alpha_mask(u32 x, u32 y, u32 w, u32 h, const u8 *alpha, u32 fg, u32 bg);
/* Moves a block of rows up by `dy` pixels (used for scrolling). */
void fb_scroll_up(u32 y, u32 h, u32 dy);

#endif
