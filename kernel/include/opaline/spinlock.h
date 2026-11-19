/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Spinlocks that also disable interrupts while held.
 *
 * With one CPU, disabling interrupts is what actually protects the data (the
 * timer cannot preempt the holder). The atomic flag makes the same code correct
 * once more CPUs run (stage E13). Never sleep or yield while holding one.
 */
#ifndef OPALINE_SPINLOCK_H
#define OPALINE_SPINLOCK_H

#include <opaline/types.h>

typedef struct {
    volatile u32 locked;
} spinlock_t;

#define SPINLOCK_INIT {0}

/* Disables interrupts and returns the previous RFLAGS. */
static inline u64 irq_save(void) {
    u64 flags;
    __asm__ volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(u64 flags) {
    if (flags & (1 << 9)) __asm__ volatile("sti" ::: "memory");
}

static inline u64 spin_lock(spinlock_t *l) {
    u64 flags = irq_save();
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return flags;
}

static inline void spin_unlock(spinlock_t *l, u64 flags) {
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    irq_restore(flags);
}

#endif
