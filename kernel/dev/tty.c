/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * The terminal (/dev/console, /dev/tty): output goes to the kernel console,
 * input comes from the keyboard, edited one line at a time ("canonical mode"):
 *   - typed characters are echoed; Backspace erases the last character (UTF-8 aware);
 *   - Enter makes the line (with '\n') available to read();
 *   - Ctrl+D on an empty line is end of file (read returns 0);
 *   - Ctrl+C discards the line being typed.
 */
#include <opaline/console.h>
#include <opaline/errno.h>
#include <opaline/sched.h>
#include <opaline/spinlock.h>
#include <opaline/string.h>
#include <opaline/vfs.h>

#define LINE_MAX  1024
#define READY_MAX 4096
#define TIOCGWINSZ 0x5413

static struct {
    char line[LINE_MAX];          /* line being edited */
    u32 line_len;
    char ready[READY_MAX];        /* finished lines, waiting for read() */
    u32 ready_head, ready_len;
    u32 eof_pending;              /* Ctrl+D presses not yet consumed */
    struct waitqueue readers;
} tty;

static void echo(const char *s, u32 n) { kputs(s, n); }

static void push_ready(const char *s, u32 n) {
    for (u32 i = 0; i < n && tty.ready_len < READY_MAX; i++)
        tty.ready[(tty.ready_head + tty.ready_len++) % READY_MAX] = s[i];
}

/* Called by the keyboard driver with the UTF-8 bytes of one key press. */
void tty_input(const char *s, u32 n) {
    bool wake = false;
    for (u32 i = 0; i < n; i++) {
        char c = s[i];
        u64 f = irq_save();
        if (c == '\n' || c == '\r') {
            push_ready(tty.line, tty.line_len);
            push_ready("\n", 1);
            tty.line_len = 0;
            wake = true;
            irq_restore(f);
            echo("\n", 1);
        } else if (c == 0x08 || c == 0x7F) {   /* backspace: drop one whole UTF-8 character */
            bool erased = false;
            while (tty.line_len > 0) {
                u8 last = (u8)tty.line[--tty.line_len];
                erased = true;
                if ((last & 0xC0) != 0x80) break;
            }
            irq_restore(f);
            if (erased) echo("\b \b", 3);
        } else if (c == 0x04) {   /* Ctrl+D */
            if (tty.line_len == 0) tty.eof_pending++;
            else {
                push_ready(tty.line, tty.line_len);
                tty.line_len = 0;
            }
            wake = true;
            irq_restore(f);
        } else if (c == 0x03) {   /* Ctrl+C */
            tty.line_len = 0;
            irq_restore(f);
            echo("^C\n", 3);
        } else if ((u8)c >= 0x20 || c == '\t') {
            bool fits = tty.line_len < LINE_MAX - 1;
            if (fits) tty.line[tty.line_len++] = c;
            irq_restore(f);
            if (fits) echo(&c, 1);
        } else {
            irq_restore(f);
        }
    }
    if (wake) waitqueue_wake_all(&tty.readers);
}

static i64 tty_read(struct vnode *v, u64 off, void *buf, u64 len) {
    (void)v, (void)off;
    if (len == 0) return 0;
    u64 f = irq_save();
    while (tty.ready_len == 0 && tty.eof_pending == 0) waitqueue_wait(&tty.readers);
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

void tty_init(void) { memset(&tty, 0, sizeof tty); }
