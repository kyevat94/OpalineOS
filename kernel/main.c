/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Kernel entry (called from kernel/arch/x86_64/entry.asm).
 *
 * Stage E1: console on the framebuffer and COM1, GDT/TSS/IDT and exception
 * diagnostics. Command line options (\OPALINE\CMDLINE.TXT) used by the tests:
 *   test=pagefault   read an unmapped address (expect a page-fault panic)
 *   test=ud          execute an invalid instruction
 *   test=divide      divide by zero
 */
#include <boot/bootinfo.h>
#include <opaline/arch/cpu.h>
#include <opaline/arch/gdt.h>
#include <opaline/arch/idt.h>
#include <opaline/console.h>
#include <opaline/fb.h>
#include <opaline/panic.h>
#include <opaline/serial.h>
#include <opaline/string.h>

#define OPALINE_VERSION "0.1.0-e1"

static const char *const region_names[] = {
    "?", "usable", "reserved", "ACPI reclaimable", "ACPI NVS", "MMIO", "bad",
    "bootloader", "kernel", "boot data", "firmware runtime",
};

static void print_memory(const struct boot_info *bi) {
    const struct boot_mem_region *r = (const struct boot_mem_region *)bi->mem_regions;
    u64 usable = 0;
    for (u64 i = 0; i < bi->mem_region_count; i++)
        if (r[i].type == BOOT_MEM_USABLE) usable += r[i].length;
    kprintf("Memory: %lu MiB usable in %lu regions\n", usable / MiB, bi->mem_region_count);
    /* the detailed map goes to the serial log only */
    for (u64 i = 0; i < bi->mem_region_count; i++) {
        char line[96];
        int n = ksnprintf(line, sizeof line, "  %016lx-%016lx  %s\n", r[i].base, r[i].base + r[i].length,
                          r[i].type < ARRAY_SIZE(region_names) ? region_names[r[i].type] : "?");
        serial_write(line, (size_t)MIN(n, (int)sizeof line - 1));
    }
}

static void run_self_test(const char *cmdline) {
    if (has_word(cmdline, "test=pagefault")) {
        kprintf("Self-test: reading an unmapped address...\n");
        volatile u64 *bad = (volatile u64 *)0xFFFFC00000000000ULL;
        (void)*bad;
    } else if (has_word(cmdline, "test=ud")) {
        kprintf("Self-test: executing an invalid instruction...\n");
        __asm__ volatile("ud2");
    } else if (has_word(cmdline, "test=divide")) {
        kprintf("Self-test: dividing by zero...\n");
        /* in assembly: the compiler is allowed to remove a C division by zero */
        __asm__ volatile("xor %%ecx, %%ecx\n\tmov $1, %%eax\n\txor %%edx, %%edx\n\tdiv %%ecx" ::: "eax", "ecx", "edx");
    }
}

void kmain(struct boot_info *bi) {
    serial_init();
    if (bi->magic != BOOT_INFO_MAGIC || bi->version != BOOT_INFO_VERSION) {
        static const char msg[] = "PANIC: invalid boot_info from the bootloader\n";
        serial_write(msg, sizeof msg - 1);
        cpu_halt_forever();
    }

    fb_init(&bi->fb);
    console_init((const void *)bi->font, bi->font_size);

    console_set_color(COLOR_ACCENT);
    kprintf("OpalineOS %s\n", OPALINE_VERSION);
    console_set_color(COLOR_SECONDARY);
    kprintf("x86-64 kernel · %s firmware\n\n", bi->firmware_vendor);

    console_set_color(COLOR_TEXT);
    kprintf("Hello, OpalineOS!\n");
    kprintf("Olá, mundo! Ação, coração, über, ñandú, Ελληνικά, Русский — € (UTF-8)\n\n");

    console_set_color(COLOR_SECONDARY);
    if (bi->fb.base)
        kprintf("Screen: %ux%u, %u bytes per row\n", bi->fb.width, bi->fb.height, bi->fb.pitch);
    else
        kprintf("Screen: none (serial console only)\n");
    kprintf("Kernel: %lu KiB at physical %p\n", bi->kernel_size / KiB, (void *)bi->kernel_phys);
    print_memory(bi);
    kprintf("ACPI RSDP: %p\n", (void *)bi->rsdp_phys);
    kprintf("Command line: \"%s\"\n\n", bi->cmdline);

    gdt_init();
    idt_init();
    console_set_color(COLOR_TEXT);
    kprintf("GDT, TSS and IDT loaded.\n");
    __asm__ volatile("int3");   /* the breakpoint handler must return here */

    run_self_test(bi->cmdline);

    console_set_color(COLOR_SUCCESS);
    kprintf("Stage E1 complete. The system is idle.\n");
    static const char marker[] = "TEST_OK E1 kernel\n";
    serial_write(marker, sizeof marker - 1);
    cpu_halt_forever();
}
