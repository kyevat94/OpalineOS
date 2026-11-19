/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * OpalineOS boot protocol: what the bootloader hands to the kernel.
 *
 * Shared by the UEFI bootloader (boot/efi) and the kernel. Only fixed-width
 * types are used, and every pointer is stored as a uint64_t virtual address
 * in the higher-half direct map (HHDM), so the layout is identical in both
 * programs.
 *
 * Machine state when the kernel entry point runs:
 *   - 64-bit long mode, interrupts disabled, boot services exited;
 *   - RDI = address of struct boot_info (HHDM virtual address);
 *   - paging: identity map of all physical memory, the same memory again at
 *     BOOT_HHDM_BASE, and the kernel ELF at its link address (BOOT_KERNEL_BASE);
 *   - the stack is the firmware's; the kernel switches to its own at once.
 */
#ifndef OPALINE_BOOTINFO_H
#define OPALINE_BOOTINFO_H

#include <stdint.h>

#define BOOT_INFO_MAGIC    0x544F4F424C41504FULL   /* "OPALBOOT" */
#define BOOT_INFO_VERSION  1

#define BOOT_HHDM_BASE     0xFFFF800000000000ULL
#define BOOT_KERNEL_BASE   0xFFFFFFFF80000000ULL

/* Memory region types. */
enum {
    BOOT_MEM_USABLE = 1,           /* free RAM */
    BOOT_MEM_RESERVED,             /* do not touch */
    BOOT_MEM_ACPI_RECLAIMABLE,     /* free once the ACPI tables were read */
    BOOT_MEM_ACPI_NVS,
    BOOT_MEM_MMIO,
    BOOT_MEM_BAD,
    BOOT_MEM_BOOTLOADER,           /* bootloader code/data: free once boot_info was consumed */
    BOOT_MEM_KERNEL,               /* the loaded kernel image */
    BOOT_MEM_BOOT_DATA,            /* boot_info, memory map, page tables, font */
    BOOT_MEM_FIRMWARE_RUNTIME,     /* UEFI runtime services */
};

struct boot_mem_region {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

struct boot_framebuffer {
    uint64_t base;            /* HHDM virtual address, 0 if there is no framebuffer */
    uint64_t base_phys;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;           /* bytes per row */
    uint32_t bpp;             /* always 32 */
    uint8_t  red_shift;
    uint8_t  green_shift;
    uint8_t  blue_shift;
    uint8_t  reserved[5];
};

struct boot_info {
    uint64_t magic;
    uint32_t version;
    uint32_t reserved0;

    uint64_t hhdm_base;
    uint64_t kernel_phys;           /* physical address of the kernel image */
    uint64_t kernel_size;           /* bytes, page aligned */

    struct boot_framebuffer fb;

    uint64_t mem_regions;           /* struct boot_mem_region[] (HHDM virtual) */
    uint64_t mem_region_count;

    uint64_t rsdp_phys;             /* ACPI root pointer, 0 if not found */
    uint64_t font;                  /* console font file (HHDM virtual), 0 if missing */
    uint64_t font_size;

    uint32_t firmware_revision;
    char     firmware_vendor[64];   /* UTF-8 */
    char     cmdline[256];          /* contents of \OPALINE\CMDLINE.TXT */
};

#endif
