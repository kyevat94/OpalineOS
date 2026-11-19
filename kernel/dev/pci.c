/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
#include <opaline/arch/cpu.h>
#include <opaline/console.h>
#include <opaline/pci.h>
#include <opaline/serial.h>

#define CONFIG_ADDRESS 0xCF8
#define CONFIG_DATA    0xCFC
#define MAX_DEVICES    64

static struct pci_device devices[MAX_DEVICES];
static int device_count;

static u32 address(const struct pci_device *d, u8 offset) {
    return 0x80000000u | ((u32)d->bus << 16) | ((u32)d->slot << 11) | ((u32)d->func << 8) | (offset & 0xFC);
}

u32 pci_read32(const struct pci_device *d, u8 offset) {
    outl(CONFIG_ADDRESS, address(d, offset));
    return inl(CONFIG_DATA);
}

void pci_write32(const struct pci_device *d, u8 offset, u32 value) {
    outl(CONFIG_ADDRESS, address(d, offset));
    outl(CONFIG_DATA, value);
}

u16 pci_read16(const struct pci_device *d, u8 offset) {
    return (u16)(pci_read32(d, offset) >> ((offset & 2) * 8));
}

void pci_write16(const struct pci_device *d, u8 offset, u16 value) {
    u32 v = pci_read32(d, offset);
    int shift = (offset & 2) * 8;
    v = (v & ~(0xFFFFu << shift)) | ((u32)value << shift);
    pci_write32(d, offset, v);
}

static void add(u8 bus, u8 slot, u8 func) {
    struct pci_device d = {.bus = bus, .slot = slot, .func = func};
    u32 id = pci_read32(&d, 0x00);
    if ((id & 0xFFFF) == 0xFFFF) return;
    d.vendor = id & 0xFFFF;
    d.device = id >> 16;
    u32 cls = pci_read32(&d, 0x08);
    d.class_code = cls >> 24;
    d.subclass = (cls >> 16) & 0xFF;
    d.prog_if = (cls >> 8) & 0xFF;
    if (device_count < MAX_DEVICES) devices[device_count++] = d;
    char line[96];
    int n = ksnprintf(line, sizeof line, "  pci %02x:%02x.%u  %04x:%04x  class %02x.%02x.%02x\n", bus, slot, func,
                      d.vendor, d.device, d.class_code, d.subclass, d.prog_if);
    serial_write(line, (size_t)MIN(n, (int)sizeof line - 1));
}

void pci_scan(void) {
    device_count = 0;
    for (unsigned bus = 0; bus < 256; bus++)
        for (u8 slot = 0; slot < 32; slot++) {
            struct pci_device d = {.bus = (u8)bus, .slot = slot, .func = 0};
            if ((pci_read32(&d, 0x00) & 0xFFFF) == 0xFFFF) continue;
            bool multi = (pci_read32(&d, 0x0C) >> 16) & 0x80;   /* header type bit 7 */
            for (u8 func = 0; func < (multi ? 8 : 1); func++) add((u8)bus, slot, func);
        }
}

bool pci_find_class(u8 class_code, u8 subclass, u8 prog_if, int n, struct pci_device *out) {
    for (int i = 0; i < device_count; i++) {
        const struct pci_device *d = &devices[i];
        if (d->class_code != class_code || d->subclass != subclass) continue;
        if (prog_if != 0xFF && d->prog_if != prog_if) continue;
        if (n-- == 0) {
            *out = *d;
            return true;
        }
    }
    return false;
}

u64 pci_bar_address(const struct pci_device *d, int bar) {
    u32 lo = pci_read32(d, (u8)(0x10 + bar * 4));
    if (lo & 1) return 0;   /* I/O space BAR */
    u64 addr = lo & ~0xFULL;
    if (((lo >> 1) & 3) == 2) addr |= (u64)pci_read32(d, (u8)(0x14 + bar * 4)) << 32;   /* 64-bit BAR */
    return addr;
}

void pci_enable_dma(const struct pci_device *d) {
    pci_write16(d, 0x04, pci_read16(d, 0x04) | 0x0006);   /* memory space + bus master */
}
