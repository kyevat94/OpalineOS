/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* x86-64 instructions as inline functions. */
#ifndef OPALINE_ARCH_CPU_H
#define OPALINE_ARCH_CPU_H

#include <opaline/types.h>

static inline void outb(u16 port, u8 value) { __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port)); }

static inline u8 inb(u16 port) {
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void cpu_cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void cpu_sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void cpu_hlt(void) { __asm__ volatile("hlt" ::: "memory"); }

static inline NORETURN void cpu_halt_forever(void) {
    for (;;) __asm__ volatile("cli; hlt");
}

#define READ_CR(n)                                                  \
    static inline u64 read_cr##n(void) {                            \
        u64 v;                                                      \
        __asm__ volatile("mov %%cr" #n ", %0" : "=r"(v));           \
        return v;                                                   \
    }
READ_CR(0)
READ_CR(2)
READ_CR(3)
READ_CR(4)
#undef READ_CR

static inline u64 rdmsr(u32 msr) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

#endif
