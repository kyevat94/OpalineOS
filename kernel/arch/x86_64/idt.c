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

#define GATE_INTERRUPT 0x8E   /* present, DPL 0, 64-bit interrupt gate */

static struct idt_entry idt[256] ALIGNED(16);
extern const u64 isr_stubs[32];
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
    for (int v = 0; v < 32; v++) set_gate(v, isr_stubs[v], 0);
    /* these must work even when the kernel stack is broken */
    set_gate(8, isr_stubs[8], IST_DOUBLE_FAULT);
    set_gate(2, isr_stubs[2], IST_NMI);
    set_gate(18, isr_stubs[18], IST_MACHINE_CHECK);
    struct idt_pointer idtr = {sizeof idt - 1, (u64)idt};
    idt_load(&idtr);
}

void interrupt_dispatch(struct interrupt_frame *f) {
    if (f->vector == 3) {   /* breakpoint: report and continue after the int3 */
        kprintf("Breakpoint (#BP) at RIP=%p: handled, continuing\n", (void *)f->rip);
        return;
    }
    panic_exception(f);
}
