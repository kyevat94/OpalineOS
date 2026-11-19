/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * OpalineOS bootloader - stage E0.
 *
 * E0 only proves the toolchain and the disk image: the firmware finds this
 * program at \EFI\BOOT\BOOTX64.EFI on the GPT/FAT32 image and runs it.
 * It prints a banner on the UEFI console and on the first serial port
 * (COM1, used by the automated tests), then waits for a key and shuts down.
 *
 * Later stages replace this with the real bootloader (load the kernel,
 * get the memory map, set up the framebuffer and exit boot services).
 */
#include "efi.h"

#define OPALINE_VERSION "0.0.0-e0"

/* ---- COM1 serial port (polled, 115200 8N1) ------------------------- */

#define COM1 0x3F8

static inline void outb(UINT16 port, UINT8 value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline UINT8 inb(UINT16 port) {
    UINT8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* no interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8 bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xC7);   /* FIFO on, cleared */
    outb(COM1 + 4, 0x03);   /* DTR + RTS */
}

static void serial_putc(char c) {
    for (int spin = 0; spin < 100000; spin++) {
        if (inb(COM1 + 5) & 0x20) break;   /* transmit holding register empty */
    }
    outb(COM1, (UINT8)c);
}

static void serial_write(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s);
    }
}

/* ---- UEFI console --------------------------------------------------- */

static EFI_SYSTEM_TABLE *st;

/* Writes an ASCII string to the UEFI console (converted to UCS-2). */
static void con_write(const char *s) {
    CHAR16 buf[128];
    UINTN n = 0;
    for (; *s; s++) {
        if (*s == '\n') buf[n++] = u'\r';
        buf[n++] = (CHAR16)(UINT8)*s;
        if (n >= 126) {
            buf[n] = 0;
            st->ConOut->OutputString(st->ConOut, buf);
            n = 0;
        }
    }
    buf[n] = 0;
    st->ConOut->OutputString(st->ConOut, buf);
}

/*
 * Writes to both the screen and the serial log, one whole line at a time.
 * (Some firmware, like QEMU's OVMF, also mirrors the console to the serial
 * port; flushing whole lines keeps the two copies from interleaving.)
 */
static char line[256];
static UINTN line_len;

static void say(const char *s) {
    for (; *s; s++) {
        if (line_len < sizeof line - 2) line[line_len++] = *s;
        if (*s == '\n') {
            line[line_len] = 0;
            con_write(line);
            serial_write(line);
            line_len = 0;
        }
    }
}

/* Converts a UCS-2 string (firmware vendor) to ASCII, replacing non-ASCII characters with '?'. */
static void ucs2_to_ascii(const CHAR16 *in, char *out, UINTN size) {
    UINTN i = 0;
    for (; in && in[i] && i + 1 < size; i++) out[i] = in[i] < 128 ? (char)in[i] : '?';
    out[i] = 0;
}

static void u32_to_hex(UINT32 v, char *out) {
    static const char digits[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 8; i++) out[2 + i] = digits[(v >> (28 - 4 * i)) & 0xF];
    out[10] = 0;
}

/* ---- Entry point ---------------------------------------------------- */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table) {
    (void)image;
    st = system_table;

    /* The firmware reboots the machine after 5 minutes unless the watchdog is disabled. */
    st->BootServices->SetWatchdogTimer(0, 0, 0, 0);

    serial_init();
    st->ConOut->ClearScreen(st->ConOut);

    st->ConOut->SetAttribute(st->ConOut, EFI_LIGHTMAGENTA | EFI_BACKGROUND_BLACK);
    say("\nOpalineOS bootloader " OPALINE_VERSION "\n");
    st->ConOut->SetAttribute(st->ConOut, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);

    char vendor[64], revision[16];
    ucs2_to_ascii(st->FirmwareVendor, vendor, sizeof vendor);
    u32_to_hex(st->FirmwareRevision, revision);
    say("Firmware: ");
    say(vendor);
    say(" (revision ");
    say(revision);
    say(")\n");
    say("Stage E0: toolchain and disk image OK.\n");

    /* Marker read by tools/test.py. */
    serial_write("TEST_OK E0 bootloader reached\n");

    st->ConOut->SetAttribute(st->ConOut, EFI_LIGHTCYAN | EFI_BACKGROUND_BLACK);
    con_write("\nPress any key to shut down.\n");
    st->ConOut->SetAttribute(st->ConOut, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);

    EFI_INPUT_KEY key;
    st->ConIn->Reset(st->ConIn, 0);
    while (st->ConIn->ReadKeyStroke(st->ConIn, &key) == EFI_NOT_READY) {
        st->BootServices->Stall(10000);   /* 10 ms */
    }

    serial_write("Shutting down.\n");
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, 0);
    return EFI_SUCCESS;
}
