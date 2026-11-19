/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Interrupt descriptor table and the register frame saved on every interrupt. */
#ifndef OPALINE_ARCH_IDT_H
#define OPALINE_ARCH_IDT_H

#include <opaline/types.h>

/* Layout pushed by kernel/arch/x86_64/isr.asm (lowest address first). */
struct interrupt_frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, error;
    u64 rip, cs, rflags, rsp, ss;    /* pushed by the CPU */
};

void idt_init(void);
void interrupt_dispatch(struct interrupt_frame *f);

#endif
