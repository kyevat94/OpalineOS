/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
#include <opaline/fb.h>
#include <opaline/string.h>

struct fb fb;

bool fb_init(const struct boot_framebuffer *info) {
    if (!info->base || info->bpp != 32) return false;
    fb.base = (u8 *)info->base;
    fb.width = info->width;
    fb.height = info->height;
    fb.pitch = info->pitch;
    fb.rs = info->red_shift;
    fb.gs = info->green_shift;
    fb.bs = info->blue_shift;
    return true;
}

static inline u32 native(u32 rgb) {
    return (((rgb >> 16) & 0xFF) << fb.rs) | (((rgb >> 8) & 0xFF) << fb.gs) | ((rgb & 0xFF) << fb.bs);
}

void fb_fill(u32 x, u32 y, u32 w, u32 h, u32 rgb) {
    if (!fb.base || x >= fb.width || y >= fb.height) return;
    w = MIN(w, fb.width - x);
    h = MIN(h, fb.height - y);
    u32 px = native(rgb);
    for (u32 row = 0; row < h; row++) {
        u32 *p = (u32 *)(fb.base + (u64)(y + row) * fb.pitch) + x;
        for (u32 i = 0; i < w; i++) p[i] = px;
    }
}

void fb_alpha_mask(u32 x, u32 y, u32 w, u32 h, const u8 *alpha, u32 fg, u32 bg) {
    if (!fb.base || x + w > fb.width || y + h > fb.height) return;
    i32 fr = (fg >> 16) & 0xFF, fgc = (fg >> 8) & 0xFF, fbl = fg & 0xFF;
    i32 br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bbl = bg & 0xFF;
    for (u32 row = 0; row < h; row++) {
        u32 *p = (u32 *)(fb.base + (u64)(y + row) * fb.pitch) + x;
        for (u32 i = 0; i < w; i++) {
            i32 a = *alpha++;
            u32 r = (u32)(br + (fr - br) * a / 255);
            u32 g = (u32)(bgc + (fgc - bgc) * a / 255);
            u32 b = (u32)(bbl + (fbl - bbl) * a / 255);
            p[i] = native(r << 16 | g << 8 | b);
        }
    }
}

void fb_scroll_up(u32 y, u32 h, u32 dy) {
    if (!fb.base || dy >= h) return;
    memmove(fb.base + (u64)y * fb.pitch, fb.base + (u64)(y + dy) * fb.pitch, (u64)(h - dy) * fb.pitch);
}
