/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * The terminal (/dev/console, /dev/tty): output goes to the kernel console,
 * input comes from the keyboard, edited one line at a time ("canonical mode"):
 *   - typed characters are inserted at the cursor and echoed;
 *   - Left/Right move the cursor, Home/End jump to the start/end of the line;
 *   - Backspace erases the character before the cursor, Delete the one under it;
 *   - Up/Down walk through the last lines entered (history);
 *   - Enter makes the line (with '\n') available to read();
 *   - Ctrl+D on an empty line is end of file (read returns 0);
 *   - Ctrl+C discards the line being typed.
 *
 * Special keys arrive as ANSI escape sequences (ESC [ D for Left...), the
 * same bytes a terminal emulator sends.
 *
 * The screen must always show exactly what the line contains. The terminal
 * records how many screen cells the echo of every byte took (a Tab takes 1 to
 * 4) and redraws the rest of the line after any change in the middle.
 */
#include <opaline/console.h>
#include <opaline/errno.h>
#include <opaline/sched.h>
#include <opaline/spinlock.h>
#include <opaline/string.h>
#include <opaline/vfs.h>

#define LINE_MAX  1024
#define READY_MAX 4096
#define HISTORY   16
#define TIOCGWINSZ 0x5413

static struct {
    char line[LINE_MAX];          /* line being edited */
    u8 cells[LINE_MAX];           /* screen cells the echo of each byte took */
    u32 len, pos;                 /* length and cursor position (bytes) */
    char ready[READY_MAX];        /* finished lines, waiting for read() */
    u32 ready_head, ready_len;
    u32 eof_pending;              /* Ctrl+D presses not yet consumed */
    struct waitqueue readers;
    char history[HISTORY][LINE_MAX];
    u32 history_count;            /* lines stored (most recent last) */
    u32 browse;                   /* history_count = editing a new line */
    char draft[LINE_MAX];         /* the new line, kept while browsing the history */
    u32 draft_len;
    int esc;                      /* escape sequence state: 0 none, 1 after ESC, 2 after ESC [ */
    u32 esc_param;
} tty;

/* ---- drawing (all with the console lock held) ------------------------------------------ */

/* Echoes byte i of the line and records how many cells the cursor moved. */
static void echo_byte(u32 i) {
    if (!console_has_screen()) {   /* serial only: no columns to measure */
        console_write(&tty.line[i], 1);
        tty.cells[i] = 1;
        return;
    }
    u32 cols, rows, c0, r0, c1, r1;
    console_get_size(&cols, &rows);
    console_get_cursor(&c0, &r0);
    console_write(&tty.line[i], 1);
    console_get_cursor(&c1, &r1);
    u64 before = (u64)r0 * cols + c0, after = (u64)r1 * cols + c1;
    if (after < before) after += cols;   /* the screen scrolled up by one line */
    tty.cells[i] = (u8)MIN(after - before, 255);
}

static void back(u32 cells) {
    for (u32 i = 0; i < cells; i++) console_write("\b", 1);
}

static u32 cells_between(u32 from, u32 to) {
    u32 n = 0;
    for (u32 i = from; i < to; i++) n += tty.cells[i];
    return n;
}

/* Redraws the line from byte `from` (the cursor is there) to the end, erases what is left of
   the old text (`old_cells` cells from `from`), and puts the cursor back at `from`. */
static void redraw_from(u32 from, u32 old_cells) {
    u32 drawn = 0;
    for (u32 i = from; i < tty.len; i++) {
        echo_byte(i);
        drawn += tty.cells[i];
    }
    if (old_cells > drawn) {
        for (u32 i = 0; i < old_cells - drawn; i++) console_write(" ", 1);
        back(old_cells - drawn);
    }
    back(drawn);
}

static u32 char_start(u32 p) {   /* start of the UTF-8 character that ends at p */
    while (p > 0 && ((u8)tty.line[--p] & 0xC0) == 0x80) {}
    return p;
}

static u32 char_end(u32 p) {     /* end of the UTF-8 character that starts at p */
    p++;
    while (p < tty.len && ((u8)tty.line[p] & 0xC0) == 0x80) p++;
    return p;
}

/* ---- editing ------------------------------------------------------------------------------ */

static void insert_byte(char c) {
    if (tty.len >= LINE_MAX - 1) return;
    u32 old_tail = cells_between(tty.pos, tty.len);
    memmove(tty.line + tty.pos + 1, tty.line + tty.pos, tty.len - tty.pos);
    memmove(tty.cells + tty.pos + 1, tty.cells + tty.pos, tty.len - tty.pos);
    tty.line[tty.pos] = c;
    tty.len++;
    echo_byte(tty.pos);
    u32 own = tty.cells[tty.pos];
    tty.pos++;
    if (tty.pos < tty.len || old_tail) redraw_from(tty.pos, old_tail > own ? old_tail - own : 0);
}

static void remove_range(u32 from, u32 to) {
    memmove(tty.line + from, tty.line + to, tty.len - to);
    memmove(tty.cells + from, tty.cells + to, tty.len - to);
    tty.len -= to - from;
}

static void backspace(void) {
    if (tty.pos == 0) return;
    u32 start = char_start(tty.pos);
    u32 width = cells_between(start, tty.pos), old_tail = cells_between(tty.pos, tty.len);
    back(width);
    remove_range(start, tty.pos);
    tty.pos = start;
    redraw_from(tty.pos, width + old_tail);
}

static void delete_forward(void) {
    if (tty.pos >= tty.len) return;
    u32 end = char_end(tty.pos);
    u32 old = cells_between(tty.pos, tty.len);
    remove_range(tty.pos, end);
    redraw_from(tty.pos, old);
}

static void move_left(void) {
    if (tty.pos == 0) return;
    u32 start = char_start(tty.pos);
    back(cells_between(start, tty.pos));
    tty.pos = start;
}

static void move_right(void) {
    if (tty.pos >= tty.len) return;
    u32 end = char_end(tty.pos);
    for (u32 i = tty.pos; i < end; i++) echo_byte(i);   /* drawing the character again moves past it */
    tty.pos = end;
}

static void move_home(void) {
    back(cells_between(0, tty.pos));
    tty.pos = 0;
}

static void move_end(void) {
    for (u32 i = tty.pos; i < tty.len; i++) echo_byte(i);
    tty.pos = tty.len;
}

/* Replaces the whole line (history), leaving the cursor at its end. */
static void replace_line(const char *text, u32 len) {
    move_home();
    u32 old = cells_between(0, tty.len);
    memcpy(tty.line, text, len);
    tty.len = len;
    redraw_from(0, old);
    move_end();
}

static void history_up(void) {
    if (tty.browse == 0) return;
    if (tty.browse == tty.history_count) {   /* leaving the new line: keep it */
        memcpy(tty.draft, tty.line, tty.len);
        tty.draft_len = tty.len;
    }
    tty.browse--;
    replace_line(tty.history[tty.browse], (u32)strlen(tty.history[tty.browse]));
}

static void history_down(void) {
    if (tty.browse >= tty.history_count) return;
    tty.browse++;
    if (tty.browse == tty.history_count) replace_line(tty.draft, tty.draft_len);
    else replace_line(tty.history[tty.browse], (u32)strlen(tty.history[tty.browse]));
}

static void remember(void) {
    if (tty.len == 0) return;
    tty.line[tty.len] = 0;
    if (tty.history_count && !strcmp(tty.history[tty.history_count - 1], tty.line)) return;
    if (tty.history_count == HISTORY) {
        memmove(tty.history[0], tty.history[1], (HISTORY - 1) * LINE_MAX);
        tty.history_count--;
    }
    memcpy(tty.history[tty.history_count++], tty.line, tty.len + 1);
}

static void push_ready(const char *s, u32 n) {
    u64 f = irq_save();
    for (u32 i = 0; i < n && tty.ready_len < READY_MAX; i++)
        tty.ready[(tty.ready_head + tty.ready_len++) % READY_MAX] = s[i];
    irq_restore(f);
}

static void reset_line(void) {
    tty.len = tty.pos = 0;
    tty.browse = tty.history_count;
}

/* Handles the final byte of an escape sequence ESC [ <param> <final>. */
static void escape(char final, u32 param) {
    switch (final) {
    case 'A': history_up(); break;
    case 'B': history_down(); break;
    case 'C': move_right(); break;
    case 'D': move_left(); break;
    case 'H': move_home(); break;
    case 'F': move_end(); break;
    case '~':
        if (param == 3) delete_forward();
        else if (param == 1) move_home();
        else if (param == 4) move_end();
        break;
    default: break;
    }
}

/* One byte from the keyboard (console lock held). Returns true if a reader must be woken. */
static bool input_byte(char c) {
    if (tty.esc == 1) {
        tty.esc = c == '[' ? 2 : 0;
        tty.esc_param = 0;
        return false;
    }
    if (tty.esc == 2) {
        if (c >= '0' && c <= '9') {
            tty.esc_param = tty.esc_param * 10 + (u32)(c - '0');
            return false;
        }
        tty.esc = 0;
        escape(c, tty.esc_param);
        return false;
    }
    switch (c) {
    case 0x1B:
        tty.esc = 1;
        return false;
    case '\n':
    case '\r':
        move_end();   /* so the output starts below a line that wrapped */
        remember();
        push_ready(tty.line, tty.len);
        push_ready("\n", 1);
        reset_line();
        console_write("\n", 1);
        return true;
    case 0x08:
    case 0x7F:
        backspace();
        return false;
    case 0x04:   /* Ctrl+D */
        if (tty.len == 0) {
            u64 f = irq_save();
            tty.eof_pending++;
            irq_restore(f);
        } else {
            push_ready(tty.line, tty.len);
            reset_line();
        }
        return true;
    case 0x03:   /* Ctrl+C */
        move_end();
        console_write("^C\n", 3);
        reset_line();
        return false;
    default:
        if ((u8)c >= 0x20 || c == '\t') insert_byte(c);
        return false;
    }
}

/* Called by the keyboard driver with the bytes of one key press. */
void tty_input(const char *s, u32 n) {
    bool wake = false;
    u64 f = console_lock();
    for (u32 i = 0; i < n; i++) wake |= input_byte(s[i]);
    console_unlock(f);
    if (wake) waitqueue_wake_all(&tty.readers);
}

static i64 tty_read(struct vnode *v, u64 off, void *buf, u64 len) {
    (void)v, (void)off;
    if (len == 0) return 0;
    u64 f = irq_save();
    while (tty.ready_len == 0 && tty.eof_pending == 0)
        if (waitqueue_wait_intr(&tty.readers) < 0) {   /* a signal arrived */
            irq_restore(f);
            return -EINTR;
        }
    if (tty.ready_len == 0) {   /* Ctrl+D on an empty line */
        tty.eof_pending--;
        irq_restore(f);
        return 0;
    }
    /* return at most one line */
    u64 n = 0;
    while (n < len && tty.ready_len > 0) {
        char c = tty.ready[tty.ready_head];
        tty.ready_head = (tty.ready_head + 1) % READY_MAX;
        tty.ready_len--;
        ((char *)buf)[n++] = c;
        if (c == '\n') break;
    }
    irq_restore(f);
    return (i64)n;
}

static i64 tty_write(struct vnode *v, u64 off, const void *buf, u64 len) {
    (void)v, (void)off;
    kputs(buf, len);
    return (i64)len;
}

static int tty_ioctl(struct vnode *v, u64 request, u64 arg) {
    (void)v;
    if (request != TIOCGWINSZ) return -ENOTTY;
    u32 cols, rows;
    console_get_size(&cols, &rows);
    u16 *ws = (u16 *)arg;   /* struct winsize: rows, cols, xpixel, ypixel (checked by the caller) */
    ws[0] = (u16)rows;
    ws[1] = (u16)cols;
    ws[2] = ws[3] = 0;
    return 0;
}

static int tty_stat(struct vnode *v, struct vnode_stat *st) {
    memset(st, 0, sizeof *st);
    st->ino = v->ino;
    st->mode = 0x2000 | 0620;   /* S_IFCHR */
    st->links = 1;
    st->dev = 2;
    st->rdev = 0x0501;          /* like Linux /dev/console (5, 1) */
    return 0;
}

static const struct vnode_ops tty_ops = {
    .read = tty_read, .write = tty_write, .ioctl = tty_ioctl, .stat = tty_stat};
static struct vnode tty_node = {.type = VN_CHARDEV, .refs = 1, .ino = 2, .ops = &tty_ops};

struct vnode *tty_vnode(void) { return &tty_node; }

void tty_init(void) {
    memset(&tty, 0, sizeof tty);
    tty.browse = 0;
}
