/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
#include <opaline/arch/cpu.h>
#include <opaline/arch/idt.h>
#include <opaline/arch/syscall.h>
#include <opaline/console.h>
#include <opaline/mm.h>
#include <opaline/panic.h>
#include <opaline/proc.h>
#include <opaline/spinlock.h>
#include <opaline/string.h>

static u64 next_pid = 1;

/* First code of a user process's main thread: leave the kernel. */
static void user_thread_start(void *arg) {
    struct process *p = arg;
    jump_to_user(p->user_entry, p->user_stack);
}

static bool map_user_stack(u64 page_table) {
    for (u64 i = 1; i <= USER_STACK_PAGES; i++) {
        u64 phys = pmm_alloc_page();
        if (!phys) return false;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        if (!vmm_map_in(page_table, USER_STACK_TOP - i * PAGE_SIZE, phys, VMM_USER | VMM_WRITE)) return false;
    }
    return true;   /* the page below the stack stays unmapped: overflowing it faults */
}

struct process *process_spawn(const char *name, const void *elf, size_t size) {
    struct process *p = kcalloc(1, sizeof *p);
    if (!p) return NULL;
    for (size_t i = 0; i < sizeof p->name - 1 && name[i]; i++) p->name[i] = name[i];
    p->user = true;
    p->page_table = vmm_create_address_space();
    const char *error = "out of memory";
    if (!p->page_table || !elf_load(p->page_table, elf, size, &p->user_entry, &error) ||
        !map_user_stack(p->page_table)) {
        kprintf("Cannot start \"%s\": %s\n", name, error);
        if (p->page_table) vmm_destroy_address_space(p->page_table);
        kfree(p);
        return NULL;
    }
    /* initial stack, as the System V ABI expects at _start: argc = 0, argv = {NULL},
       envp = {NULL}, auxv = {AT_NULL}. The pages are zeroed, so only RSP needs setting. */
    p->user_stack = USER_STACK_TOP - 64;

    u64 f = irq_save();
    p->pid = next_pid++;
    irq_restore(f);
    p->main_thread = thread_create_in(p, p->name, user_thread_start, p);
    if (!p->main_thread) {
        vmm_destroy_address_space(p->page_table);
        kfree(p);
        return NULL;
    }
    return p;
}

int process_wait(struct process *p) {
    int code = thread_join(p->main_thread);
    kfree(p);
    return code;
}

void process_exit(int code) {
    struct thread *t = thread_current();
    struct process *p = t->process;
    if (!p || !p->user) panic("process_exit called by a kernel thread");
    /* leave the address space before freeing it; from now on this thread is kernel-only */
    u64 old = p->page_table;
    u64 f = irq_save();
    p->page_table = vmm_kernel_page_table();
    write_cr3(p->page_table);
    irq_restore(f);
    vmm_destroy_address_space(old);
    thread_exit(code);
}

void process_fault(const struct interrupt_frame *f) {
    struct process *p = thread_current()->process;
    int code = f->vector == 6 ? EXIT_SIGILL : f->vector == 0 || f->vector == 16 || f->vector == 19 ? EXIT_SIGFPE
                                                                                                     : EXIT_SIGSEGV;
    console_set_color(COLOR_WARNING);
    kprintf("Process \"%s\" (pid %lu) crashed: %s at RIP=%p", p->name, p->pid, exception_name(f->vector),
            (void *)f->rip);
    if (f->vector == 14) kprintf(", address %p", (void *)read_cr2());
    kprintf("\n  The process was stopped (exit code %d). The system continues.\n", code);
    console_set_color(COLOR_TEXT);
    process_exit(code);
}
