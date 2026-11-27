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
#include <opaline/mm.h>
#include <opaline/proc.h>
#include <opaline/sched.h>
#include <opaline/spinlock.h>
#include <opaline/string.h>
#include <opaline/time.h>
#include <opaline/vfs.h>

#define LINE_MAX  1024
#define READY_MAX 4096
#define HISTORY   16
#define TIOCGWINSZ 0x5413

/* termios (Linux values): struct termios as the TCGETS/TCSETS ioctls pass it */
struct ktermios {
    u32 iflag, oflag, cflag, lflag;
    u8 line;
    u8 cc[19];
} PACKED;
#define ICRNL  0x100
#define IXON   0x400
#define OPOST  0x1
#define ONLCR  0x4
#define ISIG   0x1
#define ICANON 0x2
#define ECHO   0x8
#define ECHOE  0x10
#define ECHOK  0x20
#define ECHONL 0x40
#define ECHOCTL 0x200
#define ECHOKE 0x800
#define IEXTEN 0x8000
enum { VINTR = 0, VQUIT, VERASE, VKILL, VEOF, VTIME, VMIN, VSWTC, VSTART, VSTOP, VSUSP, VEOL, VREPRINT,
       VDISCARD, VWERASE, VLNEXT, VEOL2 };
static const struct ktermios termios_default = {
    .iflag = ICRNL | IXON,
    .oflag = OPOST | ONLCR,
    .cflag = 0xF | 0x30 | 0x80,   /* B38400 | CS8 | CREAD */
    .lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN,
    .cc = {3, 0x1C, 0x7F, 0x15, 4, 0, 1, 0, 0x11, 0x13, 0x1A, 0, 0x12, 0x0F, 0x17, 0x16, 0},
};

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
    u64 sid, fg;                  /* the session that owns the terminal and its foreground group */
    struct ktermios tio;          /* canonical (line editing) or raw input, echo, signal keys */
    int pending_signal;           /* from Ctrl+C / Ctrl+Z / Ctrl+\, sent after the console lock */
} tty;

void tty_set_foreground(u64 sid, u64 pgid) {
    tty.sid = sid;
    tty.fg = pgid;
}

u64 tty_session(void) { return tty.sid; }

/* As on Linux, the terminal of a session whose leader exited can be taken by another session. */
void tty_session_ended(u64 sid) {
    if (tty.sid == sid) tty.sid = tty.fg = 0;
}

/* ---- drawing (all with the console lock held) ------------------------------------------ */

/* What a key press draws is collected in `out` and written in one go once the key is handled
   (tty_input): the whole redraw of a long line is one console write, which hides and shows the
   cursor once, instead of one write per cell. Meanwhile the console is in a dry run, so each
   collected byte still moves its cursor as the real write will, and echo_byte can measure it. */
#define OUT_MAX 8192
static char out[OUT_MAX];
static u32 out_len;

static void draw_begin(void) { console_dry_begin(); }

static void draw_end(void) {
    console_dry_end();
    if (out_len) console_write(out, out_len);
    out_len = 0;
}

static void emit(const char *s, u32 n) {
    while (n) {
        if (out_len == OUT_MAX) {   /* only a very long redraw: write what is there, go on */
            draw_end();
            draw_begin();
        }
        u32 k = MIN(n, OUT_MAX - out_len);
        memcpy(out + out_len, s, k);
        console_write(s, k);   /* dry run: only the cursor moves */
        out_len += k;
        s += k;
        n -= k;
    }
}

static void emit_repeat(char c, u32 n) {
    char run[64];
    memset(run, c, sizeof run);
    for (u32 k; n; n -= k) {
        k = MIN(n, (u32)sizeof run);
        emit(run, k);
    }
}

/* Echoes byte i of the line and records how many cells the cursor moved. */
static void echo_byte(u32 i) {
    if (!console_has_screen()) {   /* serial only: no columns to measure */
        emit(&tty.line[i], 1);
        tty.cells[i] = 1;
        return;
    }
    u32 cols, rows, c0, r0, c1, r1;
    console_get_size(&cols, &rows);
    console_get_cursor(&c0, &r0);
    emit(&tty.line[i], 1);
    console_get_cursor(&c1, &r1);
    u64 before = (u64)r0 * cols + c0, after = (u64)r1 * cols + c1;
    if (after < before) after += cols;   /* the screen scrolled up by one line */
    tty.cells[i] = (u8)MIN(after - before, 255);
}

static void back(u32 cells) { emit_repeat('\b', cells); }

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
        emit_repeat(' ', old_cells - drawn);
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

/* Canonical mode without echo (a password prompt): a plain line, Backspace, Enter, Ctrl+D. */
static bool input_quiet(char c) {
    if (c == '\n') {
        push_ready(tty.line, tty.len);
        push_ready("\n", 1);
        reset_line();
        if (tty.tio.lflag & ECHONL) emit("\n", 1);
        return true;
    }
    if (c == (char)tty.tio.cc[VERASE] || c == 0x08) {
        while (tty.len && ((u8)tty.line[tty.len - 1] & 0xC0) == 0x80) tty.len--;   /* a whole UTF-8 character */
        if (tty.len) tty.len--;
        tty.pos = tty.len;
        return false;
    }
    if (c == (char)tty.tio.cc[VEOF]) {
        if (tty.len == 0) {
            u64 f = irq_save();
            tty.eof_pending++;
            irq_restore(f);
        } else {
            push_ready(tty.line, tty.len);
            reset_line();
        }
        return true;
    }
    if (((u8)c >= 0x20 || c == '\t') && tty.len < LINE_MAX - 1) {
        tty.line[tty.len++] = c;
        tty.pos = tty.len;
    }
    return false;
}

/* One byte from the keyboard (console lock held). Returns true if a reader must be woken. */
static bool input_byte(char c) {
    if (c == '\r' && (tty.tio.iflag & ICRNL)) c = '\n';
    if (tty.tio.lflag & ISIG) {   /* the signal keys: SIGINT, SIGQUIT, SIGTSTP for the foreground job */
        int sig = c == (char)tty.tio.cc[VINTR] ? SIGINT : c == (char)tty.tio.cc[VQUIT] ? SIGQUIT
                : c == (char)tty.tio.cc[VSUSP] ? SIGTSTP : 0;
        if (sig) {
            if (tty.tio.lflag & ICANON) move_end();
            if (tty.tio.lflag & ECHO) emit(sig == SIGINT ? "^C\n" : sig == SIGTSTP ? "^Z\n" : "^\\\n", 3);
            reset_line();
            tty.pending_signal = sig;
            return false;
        }
    }
    if (!(tty.tio.lflag & ICANON)) {   /* raw: every byte goes to the reader at once */
        push_ready(&c, 1);
        if (tty.tio.lflag & ECHO) emit(&c, 1);
        return true;
    }
    if (!(tty.tio.lflag & ECHO)) return input_quiet(c);
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
        emit("\n", 1);
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
    default:
        if ((u8)c >= 0x20 || c == '\t') insert_byte(c);
        return false;
    }
}

/* Called by the keyboard driver with the bytes of one key press. */
void tty_input(const char *s, u32 n) {
    bool wake = false;
    u64 f = console_lock();
    /* Esc alone (not the start of an arrow key's sequence) means nothing to the line editor, which
       would take it for the start of a sequence and swallow the next key; raw readers get it */
    if (n == 1 && s[0] == 0x1B && (tty.tio.lflag & (ICANON | ECHO)) == (ICANON | ECHO)) n = 0;
    draw_begin();
    for (u32 i = 0; i < n; i++) wake |= input_byte(s[i]);
    draw_end();
    int sig = tty.pending_signal;
    tty.pending_signal = 0;
    console_unlock(f);
    if (wake) {
        waitqueue_wake_all(&tty.readers);
        vfs_poll_notify();
    }
    if (sig && tty.fg) process_signal_group(tty.fg, sig, false);
}

/* A background job of the terminal's session may not read it: SIGTTIN stops it (as POSIX says). */
static int background_check(int sig) {
    struct thread *t = thread_current();
    struct process *p = t->process;
    if (!p->user || !p->ctty || p->sid != tty.sid || !tty.fg || p->pgid == tty.fg) return 0;
    if (signal_blocked_or_ignored(t, sig)) return sig == SIGTTIN ? -EIO : 0;
    process_signal_group(p->pgid, sig, false);
    return -EINTR;   /* the call starts again once the job is continued in the foreground */
}

/* Raw mode: VMIN and VTIME (tenths of a second) decide when read() returns, as POSIX says:
     VMIN=0 VTIME=0  at once, with whatever is there;
     VMIN=0 VTIME>0  as soon as a byte is there, or with 0 once VTIME has passed;
     VMIN>0 VTIME=0  once VMIN bytes are there;
     VMIN>0 VTIME>0  once VMIN bytes are there, or when VTIME passed after the last byte arrived
                     (the timer starts with the first byte, so the first wait has no limit). */
static i64 read_raw(char *buf, u64 len) {
    u64 vmin = tty.tio.cc[VMIN], vtime = tty.tio.cc[VTIME] * 100ULL;
    u64 want = MIN(len, vmin ? vmin : 1);
    u64 f = irq_save();
    bool nb = thread_current()->io_nonblock;
    if (nb && tty.ready_len == 0) {
        irq_restore(f);
        return -EAGAIN;
    }
    u32 seen = tty.ready_len;   /* bytes there when the inter-byte timer last started */
    u64 deadline = vtime && (!vmin || seen) ? time_ms() + vtime : 0;
    while (tty.ready_len < want && !nb) {
        if (!vmin && !vtime) break;                       /* poll: whatever is there */
        u64 now = time_ms();
        if (deadline && now >= deadline) break;
        int r = waitqueue_wait_timeout(&tty.readers, deadline ? deadline - now : 0);
        if (r == -EINTR) {
            irq_restore(f);
            return -EINTR;
        }
        if (vmin && vtime && tty.ready_len != seen) {     /* a byte arrived: the timer starts again */
            seen = tty.ready_len;
            deadline = seen ? time_ms() + vtime : 0;
        }
    }
    u64 n = 0;
    while (n < len && tty.ready_len > 0) {
        buf[n++] = tty.ready[tty.ready_head];
        tty.ready_head = (tty.ready_head + 1) % READY_MAX;
        tty.ready_len--;
    }
    irq_restore(f);
    return (i64)n;
}

/* Canonical mode: at most one line. */
static i64 read_line(char *buf, u64 len) {
    u64 f = irq_save();
    while (tty.ready_len == 0 && tty.eof_pending == 0) {
        if (thread_current()->io_nonblock) {
            irq_restore(f);
            return -EAGAIN;
        }
        if (waitqueue_wait_intr(&tty.readers) < 0) {   /* a signal arrived */
            irq_restore(f);
            return -EINTR;
        }
    }
    if (tty.ready_len == 0) {   /* Ctrl+D on an empty line */
        tty.eof_pending--;
        irq_restore(f);
        return 0;
    }
    u64 n = 0;
    while (n < len && tty.ready_len > 0) {
        char c = tty.ready[tty.ready_head];
        tty.ready_head = (tty.ready_head + 1) % READY_MAX;
        tty.ready_len--;
        buf[n++] = c;
        if (c == '\n') break;
    }
    irq_restore(f);
    return (i64)n;
}

static i64 tty_read(struct vnode *v, u64 off, void *buf, u64 len) {
    (void)v, (void)off;
    int bg = background_check(SIGTTIN);
    if (bg < 0) return bg;
    if (len == 0) return 0;
    /* the ring is read with interrupts off, user memory only touched with them on: a user access
       may fault and sleep on mm->lock (see pipe.c), so the bytes go through a bounce buffer */
    len = MIN(len, (u64)READY_MAX);   /* never more is there */
    char *bounce = kmalloc(len);
    if (!bounce) return -ENOMEM;
    i64 n = tty.tio.lflag & ICANON ? read_line(bounce, len) : read_raw(bounce, len);
    if (n > 0) memcpy(buf, bounce, (u64)n);
    kfree(bounce);
    return n;
}

static i64 tty_write(struct vnode *v, u64 off, const void *buf, u64 len) {
    (void)v, (void)off;
    /* kputs holds the console lock (interrupts off): user memory goes through a bounce buffer */
    enum { CHUNK = 4096 };
    if (len == 0) return 0;
    char *bounce = kmalloc(MIN(len, (u64)CHUNK));
    if (!bounce) return -ENOMEM;
    for (u64 done = 0; done < len;) {
        u64 n = MIN(len - done, (u64)CHUNK);
        memcpy(bounce, (const char *)buf + done, n);
        kputs(bounce, n);
        done += n;
    }
    kfree(bounce);
    return (i64)len;
}

static int tty_ioctl(struct vnode *v, u64 request, u64 arg) {
    (void)v;
    struct process *p = thread_current()->process;
    switch (request) {
    case 0x540F:   /* TIOCGPGRP: the foreground group */
        if (!p->ctty || p->sid != tty.sid) return -ENOTTY;
        *(i32 *)arg = (i32)tty.fg;
        return 0;
    case 0x5429:   /* TIOCGSID */
        if (!p->ctty || p->sid != tty.sid) return -ENOTTY;
        *(i32 *)arg = (i32)tty.sid;
        return 0;
    case 0x5410: { /* TIOCSPGRP: a new foreground group (tcsetpgrp) */
        if (!p->ctty || p->sid != tty.sid) return -ENOTTY;
        i32 pgid = *(const i32 *)arg;
        if (pgid <= 0) return -EINVAL;
        if (!process_group_exists((u64)pgid, p->sid)) return -EPERM;
        int bg = background_check(SIGTTOU);
        if (bg < 0) return bg;
        tty.fg = (u64)pgid;
        return 0;
    }
    case 0x540E:   /* TIOCSCTTY: a session leader takes the terminal if no other session has it;
                      root may take it from another session with arg 1 (as in Linux) */
        if (p->sid != p->pid) return -EPERM;
        if (tty.sid && tty.sid != p->sid && !(p->cred.euid == 0 && arg == 1)) return -EPERM;
        p->ctty = true;
        tty.sid = p->sid;
        tty.fg = p->pgid;
        return 0;
    case 0x5422:   /* TIOCNOTTY */
        if (!p->ctty) return -ENOTTY;
        p->ctty = false;
        return 0;
    case 0x5401:   /* TCGETS */
        memcpy((void *)arg, &tty.tio, sizeof tty.tio);
        return 0;
    case 0x5402:   /* TCSETS: now */
    case 0x5403:   /* TCSETSW: after the output drained (it never waits here) */
    case 0x5404: { /* TCSETSF: and throw away pending input */
        int bg = background_check(SIGTTOU);
        if (bg < 0) return bg;
        u64 f = console_lock();
        bool was_canonical = tty.tio.lflag & ICANON;
        memcpy(&tty.tio, (const void *)arg, sizeof tty.tio);
        if (request == 0x5404) {   /* the half-typed line is pending input too: it goes as well */
            tty.ready_len = 0;
            tty.eof_pending = 0;
            reset_line();
        } else if (was_canonical && !(tty.tio.lflag & ICANON) && tty.len) {   /* a half-typed line goes to the reader */
            push_ready(tty.line, tty.len);
            reset_line();
        }
        console_unlock(f);
        waitqueue_wake_all(&tty.readers);
        vfs_poll_notify();
        return 0;
    }
    case 0x540B: { /* TCFLSH: 0 input, 1 output, 2 both */
        if (arg == 0 || arg == 2) {
            u64 f = console_lock();
            tty.ready_len = 0;
            tty.eof_pending = 0;
            reset_line();
            console_unlock(f);
        }
        return arg <= 2 ? 0 : -EINVAL;
    }
    case 0x541B:   /* FIONREAD: bytes ready to read */
        *(i32 *)arg = (i32)tty.ready_len;
        return 0;
    case 0x5411:   /* TIOCOUTQ: output never waits */
        *(i32 *)arg = 0;
        return 0;
    case 0x5414:   /* TIOCSWINSZ: the console's size is fixed */
        return 0;
    default: break;
    }
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
    st->mode = 0x2000 | 0666;   /* S_IFCHR; everyone may use the terminal until there is a login (E15) */
    st->links = 1;
    st->dev = 2;
    st->rdev = 0x0501;          /* like Linux /dev/console (5, 1) */
    return 0;
}

static u32 tty_poll(struct vnode *v, u32 flags) {
    (void)v, (void)flags;
    bool eof = (tty.tio.lflag & ICANON) && tty.eof_pending;   /* raw reads do not see Ctrl+D */
    return (tty.ready_len || eof ? POLLIN : 0) | POLLOUT;
}

static const struct vnode_ops tty_ops = {
    .read = tty_read, .write = tty_write, .ioctl = tty_ioctl, .stat = tty_stat, .poll = tty_poll};
static struct vnode tty_node = {.type = VN_CHARDEV, .refs = 1, .ino = 2, .ops = &tty_ops};

struct vnode *tty_vnode(void) { return &tty_node; }

void tty_init(void) {
    memset(&tty, 0, sizeof tty);
    tty.browse = 0;
    tty.tio = termios_default;
}

/* ---- self-test ---------------------------------------------------------------------------- */

static inline u64 rdtsc(void) {   /* the timer stops with interrupts off: the TSC does not */
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* Typing at the start of a long line redraws all of it, with interrupts off (console lock): that
   must cost about what writing those bytes in one go costs, not a console write per cell (which
   hid and showed the cursor and went to COM1 cell by cell). Run at boot, before the keyboard
   starts; the line is thrown away with Ctrl+C. Times are the fastest of several tries (the
   least disturbed by the host). Returns false if a key costs too much. */
bool tty_selftest(void) {
    enum { LONG = 1000, KEYS = 6, LIMIT_PCT = 135 };
    for (u32 i = 0; i < LONG; i++) tty_input("x", 1);
    tty_input("\x1b[H", 3);
    u64 key = ~0ULL;
    for (u32 i = 0; i < KEYS; i++) {
        u64 t0 = rdtsc();
        tty_input("y", 1);
        key = MIN(key, rdtsc() - t0);
    }
    /* the reference: what one key at that place sends, the line from the cursor and back again */
    char *ref = kmalloc(2 * LINE_MAX);
    u64 plain = ~0ULL;
    u32 n = 0;
    if (ref) {
        u64 f = console_lock();
        for (u32 i = tty.pos; i < tty.len; i++) ref[n++] = tty.line[i];
        for (u32 i = 0, back_cells = cells_between(tty.pos, tty.len); i < back_cells && n < 2 * LINE_MAX; i++)
            ref[n++] = '\b';
        console_unlock(f);
        for (u32 i = 0; i < KEYS; i++) {
            f = console_lock();
            u64 t0 = rdtsc();
            console_write(ref, n);
            plain = MIN(plain, rdtsc() - t0);
            console_unlock(f);
        }
        kfree(ref);
    }
    tty_input("\x03", 1);   /* Ctrl+C: no foreground group yet, so it only discards the line */
    bool ok = ref && key * 100 <= plain * LIMIT_PCT;
    kprintf("tty: a key at the start of a %u-byte line: %lu cycles; writing the %u bytes it redraws: %lu%s\n",
            LONG + KEYS, key, n, plain, ok ? "" : " -- FAILED (over 135%)");
    return ok;
}
