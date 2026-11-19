/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * PS/2 keyboard (i8042 controller, scan code set 1), US layout.
 *
 * Polled by a kernel thread every 10 ms for now; interrupt delivery through
 * the I/O APIC and more layouts (US-International, ABNT2) come in stage E8,
 * USB keyboards in E13. Key presses become UTF-8 text for the terminal.
 */
#include <opaline/arch/cpu.h>
#include <opaline/sched.h>
#include <opaline/vfs.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define STATUS_OUTPUT_FULL 0x01
#define STATUS_FROM_MOUSE  0x20

/* scan code -> character, without and with Shift (0 = no character) */
static const char normal[128] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0x08, '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};
static const char shifted[128] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0x08, '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CTRL   0x1D
#define SC_CAPS   0x3A

static bool shift, ctrl, caps, extended;

static void handle(u8 sc) {
    if (sc == 0xE0) {
        extended = true;
        return;
    }
    bool release = sc & 0x80;
    u8 code = sc & 0x7F;
    bool was_extended = extended;
    extended = false;
    if (code == SC_LSHIFT || code == SC_RSHIFT) {
        shift = !release;
        return;
    }
    if (code == SC_CTRL) {
        ctrl = !release;
        return;
    }
    if (release) return;
    if (code == SC_CAPS) {
        caps = !caps;
        return;
    }
    if (was_extended) {   /* the gray keys: sent as the ANSI escape sequences terminals use */
        switch (code) {
        case 0x1C: tty_input("\n", 1); break;          /* keypad Enter */
        case 0x48: tty_input("\x1b[A", 3); break;      /* Up */
        case 0x50: tty_input("\x1b[B", 3); break;      /* Down */
        case 0x4D: tty_input("\x1b[C", 3); break;      /* Right */
        case 0x4B: tty_input("\x1b[D", 3); break;      /* Left */
        case 0x47: tty_input("\x1b[H", 3); break;      /* Home */
        case 0x4F: tty_input("\x1b[F", 3); break;      /* End */
        case 0x53: tty_input("\x1b[3~", 4); break;     /* Delete */
        default: break;
        }
        return;
    }
    char c = (shift ? shifted : normal)[code];
    if (!c) return;
    if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c ^= 0x20;
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c = (char)((c & 0x1F));   /* Ctrl+letter */
    tty_input(&c, 1);
}

static void keyboard_thread(void *arg) {
    (void)arg;
    for (;;) {
        for (int i = 0; i < 64; i++) {
            u8 status = inb(PS2_STATUS);
            if (!(status & STATUS_OUTPUT_FULL)) break;
            u8 data = inb(PS2_DATA);
            if (!(status & STATUS_FROM_MOUSE)) handle(data);
        }
        thread_sleep_ms(10);
    }
}

void keyboard_init(void) {
    while (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) (void)inb(PS2_DATA);   /* drop stale bytes */
    struct thread *t = thread_create("keyboard", keyboard_thread, NULL);
    if (t) thread_detach(t);
}
