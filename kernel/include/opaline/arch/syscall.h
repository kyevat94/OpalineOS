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

struct interrupt_frame;   /* syscall_entry saves the same frame as an interrupt (opaline/arch/idt.h) */
#define SYSCALL_VECTOR 0x100     /* frame.vector of a system call */

/* Programs the SYSCALL MSRs and the per-CPU data block. */
void syscall_init(void);
/* Kernel stack used by the next SYSCALL (the current thread's stack top). */
void cpu_set_syscall_stack(u64 top);
/* Leaves the kernel: starts executing user code at `rip` with stack `rsp`. */
NORETURN void jump_to_user(u64 rip, u64 rsp);
/* Leaves the kernel with every register taken from `f`, which must be on the current kernel stack. */
NORETURN void return_to_user(struct interrupt_frame *f);

/* Implemented in kernel/proc/syscalls.c. Returns non-zero when the frame must be restored with IRETQ. */
int syscall_dispatch(struct interrupt_frame *f);

#endif
