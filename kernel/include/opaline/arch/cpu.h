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

static inline void wrmsr(u32 msr, u64 value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((u32)value), "d"((u32)(value >> 32)) : "memory");
}

static inline void cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static inline void write_cr3(u64 v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline void write_cr4(u64 v) { __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }
static inline void invlpg(u64 addr) { __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory"); }
static inline void wbinvd(void) { __asm__ volatile("wbinvd" ::: "memory"); }

/* FPU/SSE state (512-byte, 16-byte aligned area) */
static inline void fxsave(void *area) { __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory"); }
static inline void fxrstor(const void *area) { __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory"); }

#define MSR_EFER           0xC0000080
#define MSR_STAR           0xC0000081
#define MSR_LSTAR          0xC0000082
#define MSR_FMASK          0xC0000084
#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_PAT            0x00000277
#define EFER_SCE (1ULL << 0)
#define EFER_NXE (1ULL << 11)
#define CR4_PGE  (1ULL << 7)

#endif
