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
#include <opaline/block.h>
#include <opaline/console.h>
#include <opaline/fb.h>
#include <opaline/mm.h>
#include <opaline/pci.h>
#include <opaline/power.h>
#include <opaline/panic.h>
#include <opaline/proc.h>
#include <opaline/sched.h>
#include <opaline/time.h>
#include <opaline/serial.h>
#include <opaline/string.h>
#include <opaline/vfs.h>

#define OPALINE_VERSION "0.6.3-se3"

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

/* GPT partition type of the OpalineOS root partition: 4f50414c-494e-4552-4f4f-542d45465333 (tools/efs3.py) */
static const u8 root_partition_type[16] = {0x4C, 0x41, 0x50, 0x4F, 0x4E, 0x49, 0x52, 0x45,
                                           0x4F, 0x4F, 0x54, 0x2D, 0x45, 0x46, 0x53, 0x33};

static void mount_root_file_system(void) {
    pci_scan();
    if (ahci_init() == 0) panic("no SATA disk found (AHCI)");
    struct block_device *disk = block_first_disk();
    struct block_device *part = gpt_find_partition(disk, root_partition_type);
    if (!part) panic("no OpalineOS root partition on %s", disk->name);
    struct vnode *root;
    int r = efs3_mount(part, &root);
    if (r < 0) panic("cannot mount the root file system on %s (error %d)", part->name, r);
    vfs_init();
    vfs_mount_root(root);
    vnode_put(root);
    r = vfs_mount("/dev", devfs_init());
    if (r < 0) panic("cannot mount /dev (error %d)", r);
    u64 total, free_blocks;
    char label[33];
    efs3_info(root, &total, &free_blocks, label, sizeof label);
    kprintf("Root file system: EFS3 \"%s\" on %s, %lu MiB, %lu MiB free; /dev mounted\n", label, part->name,
            total * 4 / 1024, free_blocks * 4 / 1024);
}

/* Starts a program with no arguments and waits for it. Returns its exit code (or -errno). */
static int run_program(const char *path) {
    const char *argv[] = {path, NULL};
    int err;
    struct process *p = process_spawn_path(path, argv, NULL, 0, &err);
    if (!p) return err;
    return process_wait(p);
}

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

/* The number after "test=powercut:" in the command line, or -1. */
static i64 powercut_arg(const char *cmdline) {
    static const char key[] = "test=powercut:";
    for (const char *p = cmdline; *p; p++) {
        if (strncmp(p, key, sizeof key - 1) || (p != cmdline && p[-1] != ' ')) continue;
        i64 n = 0;
        for (p += sizeof key - 1; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
        return n;
    }
    return -1;
}

static void run_self_test(const char *cmdline) {
    i64 cut = powercut_arg(cmdline);
    if (cut >= 0) powercut_test((u64)cut);   /* does not return */
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
    acpi_init(bi->rsdp_phys);
    irq_set_handler(VECTOR_TIMER, timer_interrupt);
    sched_init();
    u64 timer_hz = lapic_timer_start(VECTOR_TIMER, TIMER_HZ);
    cpu_sti();
    kprintf("Timer: local APIC at %lu MHz, %d interrupts per second, %d ms time slice\n",
            timer_hz / 1000000, TIMER_HZ, TIME_SLICE_MS);
    console_start_cursor_blink();
    if (!sched_selftest()) panic("scheduler self-test failed");
    time_init();   /* the wall clock: the RTC plus the timer from now on */
    static const char sched_marker[] = "TEST_OK E3 scheduler\n";
    serial_write(sched_marker, sizeof sched_marker - 1);

    /* storage, files and the terminal */
    mount_root_file_system();
    tty_init();
    keyboard_init();

    /* user mode */
    syscall_init();
    kprintf("System calls ready (SYSCALL/SYSRET, Linux x86-64 numbering).\n");
    if (!user_selftest()) panic("user-space self-test failed");
    static const char user_marker[] = "TEST_OK E4 userspace\n";
    serial_write(user_marker, sizeof user_marker - 1);

    int fs_result = run_program("/apps/fstest");
    if (fs_result != 0) panic("file system self-test (/apps/fstest) failed (result %d)", fs_result);
    static const char fs_marker[] = "TEST_OK E5 files\n";
    serial_write(fs_marker, sizeof fs_marker - 1);

    /* C programs on musl: through the dynamic linker, then statically linked */
    if (!posix_selftest()) panic("POSIX self-test failed");
    static const char posix_marker[] = "TEST_OK E6 posix\n";
    serial_write(posix_marker, sizeof posix_marker - 1);

    run_self_test(bi->cmdline);

    console_set_color(COLOR_SUCCESS);
    kprintf("Stage E6 complete.\n\n");
    console_set_color(COLOR_TEXT);
    static const char marker[] = "TEST_OK boot complete\n";
    serial_write(marker, sizeof marker - 1);

    /* the shell runs on the terminal; leaving it ("exit", Ctrl+D) turns the computer off */
    int r;
    while ((r = run_program("/system/bin/sh")) < 0) {
        kprintf("Cannot start /system/bin/sh (error %d), trying again\n", r);
        thread_sleep_ms(1000);
    }
    system_poweroff();
}
