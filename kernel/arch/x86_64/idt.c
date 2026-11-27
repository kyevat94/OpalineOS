/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
#include <opaline/arch/gdt.h>
#include <opaline/arch/idt.h>
#include <opaline/console.h>
#include <opaline/panic.h>
#include <opaline/arch/cpu.h>
#include <opaline/mm.h>
#include <opaline/proc.h>
#include <opaline/string.h>
#include <opaline/uvm.h>

struct idt_entry {
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 zero;
} PACKED;

struct idt_pointer {
    u16 limit;
    u64 base;
} PACKED;

#define GATE_INTERRUPT 0x8E   /* present, DPL 0, 64-bit interrupt gate (IF cleared on entry) */

static struct idt_entry idt[256] ALIGNED(16);
static irq_handler handlers[256];
extern const u64 isr_stubs[256];
void idt_load(const struct idt_pointer *idtr);

static void set_gate(int vector, u64 handler, u8 ist) {
    idt[vector] = (struct idt_entry){
        .offset_low = handler & 0xFFFF,
        .selector = GDT_KERNEL_CODE,
        .ist = ist,
        .type_attr = GATE_INTERRUPT,
        .offset_mid = (handler >> 16) & 0xFFFF,
        .offset_high = handler >> 32,
        .zero = 0,
    };
}

void idt_init(void) {
    for (int v = 0; v < 256; v++) set_gate(v, isr_stubs[v], 0);
    /* these must work even when the current stack is broken (e.g. a kernel stack overflow) */
    set_gate(8, isr_stubs[8], IST_DOUBLE_FAULT);
    set_gate(2, isr_stubs[2], IST_NMI);
    set_gate(18, isr_stubs[18], IST_MACHINE_CHECK);
    struct idt_pointer idtr = {sizeof idt - 1, (u64)idt};
    idt_load(&idtr);
}

void irq_set_handler(u8 vector, irq_handler handler) { handlers[vector] = handler; }

/* Access type of a page fault, from its error code. */
static u32 fault_access(u64 error) { return error & 2 ? PROT_WRITE : error & 0x10 ? PROT_EXEC : PROT_READ; }

/*
 * The kernel touched a user address that is not there. System calls bring user pages in
 * before using them, so this only happens when another thread of the process unmapped
 * the memory meanwhile: put a kernel-only zero page there so the access completes, and
 * end the process (SIGSEGV). A kernel bug still panics.
 */
static bool kernel_user_fault(struct interrupt_frame *f) {
    u64 addr = read_cr2();
    struct process *p = thread_current() ? thread_current()->process : NULL;
    if (addr >= USER_TOP || !p || !p->user || !p->mm || p->mm->lock.owner == thread_current()) return false;
    if (f->rflags & 0x200) __asm__ volatile("sti");
    bool ok = mm_fault(p->mm, addr, fault_access(f->error));
    u64 page = ok ? 0 : pmm_alloc_page();
    if (page) {
        /* the address may still hold a page (one made read-only meanwhile, for a write): it goes
           as on munmap, so its reference is not lost; the lock keeps other threads' changes whole */
        memset(phys_to_virt(page), 0, PAGE_SIZE);
        u64 va = ALIGN_DOWN(addr, PAGE_SIZE);
        mutex_lock(&p->mm->lock);
        u64 old = vmm_unmap_in(p->mm->page_table, va);
        if (old != VMM_NOT_MAPPED) pmm_page_put(old);
        if (!vmm_map_in(p->mm->page_table, va, page, VMM_WRITE)) {
            pmm_free_page(page);
            page = 0;
        }
        mutex_unlock(&p->mm->lock);
    }
    __asm__ volatile("cli");
    if (ok) return true;
    if (!page) return false;
    process_kill(p, SIGSEGV);
    return true;
}

void interrupt_dispatch(struct interrupt_frame *f) {
    if (f->vector < 32 && (f->cs & 3) == 3) {
        /* user memory not there yet: bring it in (demand paging) */
        if (f->vector == 14) {
            struct thread *t = thread_current();
            struct process *p = t->process;
            /* before STI: a switch to another thread that faults would change CR2 */
            t->fault_addr = read_cr2();
            __asm__ volatile("sti");
            bool ok = p->mm && mm_fault(p->mm, t->fault_addr, fault_access(f->error));
            __asm__ volatile("cli");
            if (!ok) process_fault(f);
        } else {
            /* any other CPU exception in user mode becomes a signal: it never stops the system */
            process_fault(f);
        }
        user_return(f);
        return;
    }
    if (f->vector == 14 && kernel_user_fault(f)) return;
    if (f->vector == 3) {   /* breakpoint: report and continue after the int3 */
        kprintf("Breakpoint (#BP) at RIP=%p: handled, continuing\n", (void *)f->rip);
        return;
    }
    if (f->vector < 32) panic_exception(f);

    if (handlers[f->vector]) {
        handlers[f->vector](f);
        if ((f->cs & 3) == 3) user_return(f);   /* signals, or the process is ending */
        return;
    }
    /* spurious interrupts (LAPIC 0xFF, PIC IRQ 7/15) need no acknowledgement */
    if (f->vector == VECTOR_SPURIOUS || (f->vector >= VECTOR_PIC_BASE && f->vector < VECTOR_PIC_BASE + 16)) return;
    kprintf("Warning: unexpected interrupt, vector %lu\n", f->vector);
}
