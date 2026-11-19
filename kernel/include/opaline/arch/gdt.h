/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Global descriptor table and task state segment. */
#ifndef OPALINE_ARCH_GDT_H
#define OPALINE_ARCH_GDT_H

#include <opaline/types.h>

/*
 * Selectors. The order (kernel code, kernel data, user data, user code) is the
 * one SYSCALL/SYSRET expect, used from stage E4 on.
 */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

/* Interrupt stack table slots (separate stacks for faults that must always work). */
#define IST_DOUBLE_FAULT 1
#define IST_NMI          2
#define IST_MACHINE_CHECK 3

void gdt_init(void);

#endif
