/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Kernel entry (called from kernel/arch/x86_64/entry.asm).
 *
 * Boot order: console, GDT/TSS/IDT, physical page allocator, the kernel's own
 * page tables, heap, memory self-test.
 *
 * Command line options (\OPALINE\CMDLINE.TXT) used by the automated tests:
 *   test=pagefault   read an unmapped kernel address
 *   test=null        read address 0 (the identity map is gone after vmm_init)
 *   test=wp          write to read-only kernel data (.rodata)
 *   test=nx          execute code from the heap (no-execute)
 *   test=doublefree  free the same heap block twice
 *   test=stackoverflow  recurse in a thread until it hits its guard page (double fault)
 *   test=threadfault  NULL access inside a thread (the panic names the thread)
 *   test=ud          execute an invalid instruction
 *   test=divide      divide by zero
 * Each one must end in a panic screen.
 */
#include <boot/bootinfo.h>
#include <opaline/arch/apic.h>
#include <opaline/arch/cpu.h>
#include <opaline/arch/gdt.h>
#include <opaline/arch/idt.h>
#include <opaline/arch/syscall.h>
#include <opaline/console.h>
#include <opaline/fb.h>
#include <opaline/mm.h>
#include <opaline/panic.h>
#include <opaline/proc.h>
#include <opaline/sched.h>
#include <opaline/serial.h>
#include <opaline/string.h>

#define OPALINE_VERSION "0.4.0-e4"

static const char *const region_names[] = {
    "?", "usable", "reserved", "ACPI reclaimable", "ACPI NVS", "MMIO", "bad",
    "bootloader", "kernel", "boot data", "firmware runtime",
};

/* The detailed memory map goes to the serial log only. */
static void log_memory_map(const struct boot_info *bi) {
    const struct boot_mem_region *r = (const struct boot_mem_region *)bi->mem_regions;
    for (u64 i = 0; i < bi->mem_region_count; i++) {
        char line[96];
        int n = ksnprintf(line, sizeof line, "  %016lx-%016lx  %s\n", r[i].base, r[i].base + r[i].length,
                          r[i].type < ARRAY_SIZE(region_names) ? region_names[r[i].type] : "?");
        serial_write(line, (size_t)MIN(n, (int)sizeof line - 1));
    }
}

static const u64 read_only_value = 0x0BADC0DE;

static void timer_interrupt(struct interrupt_frame *f) {
    (void)f;
    lapic_eoi();      /* before sched_tick: it may switch threads and not return for a while */
    sched_tick();
}

/* Deep recursion: each call keeps 256 bytes on the stack until it runs into the guard page.
   (The limit is far beyond any kernel stack; it only stops the compiler from treating this as
   infinite recursion, which C does not allow.) */
static volatile u64 recursion_limit = 1000000;

static u64 recurse(u64 depth) {
    volatile u8 buffer[256];
    buffer[0] = (u8)depth;
    if (depth >= recursion_limit) return 0;
    return recurse(depth + 1) + buffer[0];
}

static void overflow_thread(void *arg) { (void)arg; (void)recurse(0); }

static void faulty_thread(void *arg) {
    (void)arg;
    __asm__ volatile("mov (%0), %%rax" : : "r"(0ULL) : "rax", "memory");
}

static void run_self_test(const char *cmdline) {
    if (has_word(cmdline, "test=stackoverflow")) {
        kprintf("Self-test: overflowing a thread's stack...\n");
        thread_join(thread_create("overflow", overflow_thread, NULL));
    } else if (has_word(cmdline, "test=threadfault")) {
        kprintf("Self-test: NULL access inside a thread...\n");
        thread_join(thread_create("faulty", faulty_thread, NULL));
    } else if (has_word(cmdline, "test=pagefault")) {
        kprintf("Self-test: reading an unmapped address...\n");
        (void)*(volatile u64 *)0xFFFFC00000000000ULL;
    } else if (has_word(cmdline, "test=null")) {
        kprintf("Self-test: reading address 0...\n");
        /* in assembly: a NULL dereference in C is undefined and may be compiled into something else */
        __asm__ volatile("mov (%0), %%rax" : : "r"(0ULL) : "rax", "memory");
    } else if (has_word(cmdline, "test=wp")) {
        kprintf("Self-test: writing to read-only data...\n");
        *(volatile u64 *)&read_only_value = 1;
    } else if (has_word(cmdline, "test=nx")) {
        kprintf("Self-test: executing code from the heap...\n");
        u8 *code = kmalloc(16);
        code[0] = 0xC3;   /* ret */
        ((void (*)(void))code)();
    } else if (has_word(cmdline, "test=doublefree")) {
        kprintf("Self-test: freeing a heap block twice...\n");
        void *p = kmalloc(48);
        kfree(p);
        kfree(p);
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
    kprintf("ACPI RSDP: %p\n", (void *)bi->rsdp_phys);
    kprintf("Command line: \"%s\"\n", bi->cmdline);
    log_memory_map(bi);

    gdt_init();
    idt_init();
    __asm__ volatile("int3");   /* the breakpoint handler must return here */

    pmm_init(bi);
    vmm_init(bi);
    struct pmm_stats ps;
    pmm_get_stats(&ps);
    kprintf("Memory: %lu MiB managed, %lu MiB free, no-execute %s\n\n", ps.total_pages * PAGE_SIZE / MiB,
            ps.free_pages * PAGE_SIZE / MiB, vmm_nx_enabled() ? "on" : "off");

    console_set_color(COLOR_TEXT);
    kprintf("GDT, TSS and IDT loaded.\n");
    kprintf("Kernel page tables active: text read-only, data no-execute, NULL unmapped.\n");
    if (!mm_selftest()) panic("memory self-test failed");
    static const char mm_marker[] = "TEST_OK E2 memory\n";
    serial_write(mm_marker, sizeof mm_marker - 1);

    /* interrupts and scheduling */
    pic_disable();
    lapic_init();
    irq_set_handler(VECTOR_TIMER, timer_interrupt);
    sched_init();
    u64 timer_hz = lapic_timer_start(VECTOR_TIMER, TIMER_HZ);
    cpu_sti();
    kprintf("Timer: local APIC at %lu MHz, %d interrupts per second, %d ms time slice\n",
            timer_hz / 1000000, TIMER_HZ, TIME_SLICE_MS);
    if (!sched_selftest()) panic("scheduler self-test failed");
    static const char sched_marker[] = "TEST_OK E3 scheduler\n";
    serial_write(sched_marker, sizeof sched_marker - 1);

    /* user mode */
    syscall_init();
    kprintf("System calls ready (SYSCALL/SYSRET, Linux x86-64 numbering).\n");
    if (!user_selftest()) panic("user-space self-test failed");
    static const char user_marker[] = "TEST_OK E4 userspace\n";
    serial_write(user_marker, sizeof user_marker - 1);

    run_self_test(bi->cmdline);

    console_set_color(COLOR_SUCCESS);
    kprintf("Stage E4 complete. The system is idle.\n");
    static const char marker[] = "TEST_OK boot complete\n";
    serial_write(marker, sizeof marker - 1);
    for (;;) thread_sleep_ms(1000);   /* kmain stays alive; the idle thread halts the CPU */
}
