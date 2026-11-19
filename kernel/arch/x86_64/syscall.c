/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
#include <opaline/arch/cpu.h>
#include <opaline/arch/gdt.h>
#include <opaline/arch/syscall.h>

/* Per-CPU data reached through GS during syscall_entry (one CPU for now). */
struct cpu_local {
    u64 kernel_stack;   /* [gs:0] */
    u64 user_stack;     /* [gs:8] scratch */
};

static struct cpu_local cpu0;

void syscall_entry(void);

void syscall_init(void) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    /* SYSCALL loads CS=0x08/SS=0x10; SYSRET loads CS=0x10+16 (user code), SS=0x10+8 (user data) */
    wrmsr(MSR_STAR, ((u64)(GDT_KERNEL_DATA | 0) << 48) | ((u64)GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (u64)syscall_entry);
    /* cleared on entry: TF (0x100), IF (0x200), DF (0x400), AC (0x40000) */
    wrmsr(MSR_FMASK, 0x40700);
    wrmsr(MSR_GS_BASE, 0);
    wrmsr(MSR_KERNEL_GS_BASE, (u64)&cpu0);
}

void cpu_set_syscall_stack(u64 top) { cpu0.kernel_stack = top; }
