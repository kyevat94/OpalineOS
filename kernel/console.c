/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Kernel console. Every write goes to COM1 unchanged (UTF-8 bytes) and, when
 * a framebuffer and a font exist, is decoded from UTF-8 and drawn on screen
 * with the console font (assets/fonts/console.fnt, format in tools/gen_font.py).
 */
#include <opaline/console.h>
#include <opaline/fb.h>
#include <opaline/serial.h>
#include <opaline/string.h>

#define MARGIN 16          /* pixels around the text area */
#define TAB_WIDTH 4

struct font_header {
    char magic[4];
    u16 version;
    u8 cell_w, cell_h, baseline, flags;
    u16 reserved;
    u32 count, codepoints_offset, bitmaps_offset;
    u8 reserved2[8];
} PACKED;

static struct {
    const u32 *codepoints;
    const u8 *bitmaps;
    u32 count, cell_w, cell_h, glyph_size;
    u32 replacement;           /* index of U+FFFD */
} font;

static struct {
    bool screen;
    u32 cols, rows, col, row;
    u32 fg, bg;
    u32 utf8_cp, utf8_need;    /* UTF-8 decoder state */
} con = {.fg = COLOR_TEXT, .bg = COLOR_BACKGROUND};

static i64 glyph_index(u32 cp) {
    u32 lo = 0, hi = font.count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (font.codepoints[mid] == cp) return mid;
        if (font.codepoints[mid] < cp) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

void console_init(const void *font_file, size_t size) {
    const struct font_header *h = font_file;
    if (!font_file || !fb.base || size < sizeof *h || memcmp(h->magic, "OPFN", 4) || h->version != 1) {
        con.screen = false;
        return;
    }
    font.codepoints = (const u32 *)((const u8 *)font_file + h->codepoints_offset);
    font.bitmaps = (const u8 *)font_file + h->bitmaps_offset;
    font.count = h->count;
    font.cell_w = h->cell_w;
    font.cell_h = h->cell_h;
    font.glyph_size = font.cell_w * font.cell_h;
    if (h->bitmaps_offset + (u64)font.count * font.glyph_size > size) return;
    i64 r = glyph_index(0xFFFD);
    font.replacement = r < 0 ? 0 : (u32)r;

    con.cols = (fb.width - 2 * MARGIN) / font.cell_w;
    con.rows = (fb.height - 2 * MARGIN) / font.cell_h;
    con.screen = con.cols > 0 && con.rows > 0;
    console_clear(con.bg);
}

bool console_has_screen(void) { return con.screen; }

void console_clear(u32 background) {
    con.bg = background;
    con.col = con.row = 0;
    if (con.screen) fb_fill(0, 0, fb.width, fb.height, background);
}

void console_set_color(u32 fg) { con.fg = fg; }

static void newline(void) {
    con.col = 0;
    if (++con.row < con.rows) return;
    con.row = con.rows - 1;
    fb_scroll_up(MARGIN, con.rows * font.cell_h, font.cell_h);
    fb_fill(MARGIN, MARGIN + con.row * font.cell_h, con.cols * font.cell_w, font.cell_h, con.bg);
}

static void draw_codepoint(u32 cp) {
    if (cp == '\n') {
        newline();
        return;
    }
    if (cp == '\r') {
        con.col = 0;
        return;
    }
    if (cp == '\t') {
        do draw_codepoint(' '); while (con.col % TAB_WIDTH);
        return;
    }
    if (cp < 0x20) return;
    if (con.col >= con.cols) newline();
    i64 idx = glyph_index(cp);
    const u8 *glyph = font.bitmaps + (u64)(idx < 0 ? font.replacement : (u32)idx) * font.glyph_size;
    fb_alpha_mask(MARGIN + con.col * font.cell_w, MARGIN + con.row * font.cell_h, font.cell_w, font.cell_h,
                  glyph, con.fg, con.bg);
    con.col++;
}

/* Feeds one byte to the UTF-8 decoder; invalid sequences become U+FFFD. */
static void utf8_byte(u8 b) {
    if (con.utf8_need) {
        if ((b & 0xC0) == 0x80) {
            con.utf8_cp = (con.utf8_cp << 6) | (b & 0x3F);
            if (--con.utf8_need == 0) draw_codepoint(con.utf8_cp);
            return;
        }
        con.utf8_need = 0;
        draw_codepoint(0xFFFD);   /* sequence cut short; reprocess this byte below */
    }
    if (b < 0x80) draw_codepoint(b);
    else if ((b & 0xE0) == 0xC0) con.utf8_cp = b & 0x1F, con.utf8_need = 1;
    else if ((b & 0xF0) == 0xE0) con.utf8_cp = b & 0x0F, con.utf8_need = 2;
    else if ((b & 0xF8) == 0xF0) con.utf8_cp = b & 0x07, con.utf8_need = 3;
    else draw_codepoint(0xFFFD);
}

void console_write(const char *s, size_t n) {
    serial_write(s, n);
    if (!con.screen) return;
    for (size_t i = 0; i < n; i++) utf8_byte((u8)s[i]);
}
