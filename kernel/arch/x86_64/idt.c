/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
#include <opaline/arch/gdt.h>
#include <opaline/arch/idt.h>
#include <opaline/console.h>
#include <opaline/panic.h>

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

void interrupt_dispatch(struct interrupt_frame *f) {
    if (f->vector == 3) {   /* breakpoint: report and continue after the int3 */
        kprintf("Breakpoint (#BP) at RIP=%p: handled, continuing\n", (void *)f->rip);
        return;
    }
    if (f->vector < 32) panic_exception(f);

    if (handlers[f->vector]) {
        handlers[f->vector](f);
        return;
    }
    /* spurious interrupts (LAPIC 0xFF, PIC IRQ 7/15) need no acknowledgement */
    if (f->vector == VECTOR_SPURIOUS || (f->vector >= VECTOR_PIC_BASE && f->vector < VECTOR_PIC_BASE + 16)) return;
    kprintf("Warning: unexpected interrupt, vector %lu\n", f->vector);
}
