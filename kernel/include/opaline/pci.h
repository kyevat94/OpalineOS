/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* PCI configuration space (legacy I/O ports 0xCF8/0xCFC) and device discovery. */
#ifndef OPALINE_PCI_H
#define OPALINE_PCI_H

#include <opaline/types.h>

struct pci_device {
    u8 bus, slot, func;
    u16 vendor, device;
    u8 class_code, subclass, prog_if;
};

u32 pci_read32(const struct pci_device *d, u8 offset);
void pci_write32(const struct pci_device *d, u8 offset, u32 value);
u16 pci_read16(const struct pci_device *d, u8 offset);
void pci_write16(const struct pci_device *d, u8 offset, u16 value);

/* Scans every bus once and logs the devices to the serial port. */
void pci_scan(void);
/* Finds the n-th device of a class/subclass/programming interface (0xFF = any). */
bool pci_find_class(u8 class_code, u8 subclass, u8 prog_if, int n, struct pci_device *out);
/* Physical address of a memory BAR (handles 64-bit BARs). */
u64 pci_bar_address(const struct pci_device *d, int bar);
/* Turns on memory decoding and bus mastering (needed for DMA). */
void pci_enable_dma(const struct pci_device *d);

#endif
