/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * The SYSCALL entry point and the jump into user mode.
 *
 * Calling convention (the Linux x86-64 one, see project.md section 3.1):
 * RAX = number, arguments in RDI, RSI, RDX, R10, R8, R9, result in RAX.
 * RCX and R11 are overwritten by the CPU (return address and flags).
 */
#ifndef OPALINE_ARCH_SYSCALL_H
#define OPALINE_ARCH_SYSCALL_H

#include <opaline/types.h>

/* Registers saved by syscall_entry (kernel/arch/x86_64/syscall.asm), lowest address first. */
struct syscall_frame {
    u64 r15, r14, r13, r12, r9, r8, r10, rdx, rsi, rdi, rbp, rbx;
    u64 rax;          /* system call number on entry, result on exit */
    u64 rip;          /* user return address (RCX) */
    u64 rflags;       /* user flags (R11) */
    u64 rsp;          /* user stack pointer */
};

/* Programs the SYSCALL MSRs and the per-CPU data block. */
void syscall_init(void);
/* Kernel stack used by the next SYSCALL (the current thread's stack top). */
void cpu_set_syscall_stack(u64 top);
/* Leaves the kernel: starts executing user code at `rip` with stack `rsp`. */
NORETURN void jump_to_user(u64 rip, u64 rsp);

/* Implemented in kernel/proc/syscalls.c. */
void syscall_dispatch(struct syscall_frame *f);

#endif
