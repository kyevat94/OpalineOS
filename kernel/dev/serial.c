/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
#include <opaline/arch/cpu.h>
#include <opaline/serial.h>

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* no interrupts (polled) */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8 bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xC7);   /* FIFO on, cleared */
    outb(COM1 + 4, 0x03);   /* DTR + RTS */
}

static void serial_putc(u8 c) {
    for (int spin = 0; spin < 100000 && !(inb(COM1 + 5) & 0x20); spin++) {}
    outb(COM1, c);
}

void serial_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') serial_putc('\r');
        serial_putc((u8)s[i]);
    }
}
