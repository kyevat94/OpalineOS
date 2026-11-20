/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Random bytes for programs (AT_RANDOM, getrandom). RDRAND when the CPU has
 * it, mixed with the time stamp counter; otherwise a xorshift generator
 * reseeded from the TSC. Good enough for stack canaries and hash seeds, not
 * for cryptography (a proper entropy pool comes later).
 */
#include <opaline/arch/cpu.h>
#include <opaline/proc.h>
#include <opaline/spinlock.h>

static u64 state[2];
static bool have_rdrand, ready;
static spinlock_t lock = SPINLOCK_INIT;

static inline u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static bool rdrand(u64 *out) {
    for (int i = 0; i < 10; i++) {
        u8 ok;
        __asm__ volatile("rdrand %0\n\tsetc %1" : "=r"(*out), "=qm"(ok));
        if (ok) return true;
    }
    return false;
}

static u64 next(void) {
    u64 a = state[0], b = state[1];
    state[0] = b;
    a ^= a << 23;
    state[1] = a ^ b ^ (a >> 17) ^ (b >> 26);
    return state[1] + b;
}

void random_bytes(void *buf, u64 len) {
    u64 f = spin_lock(&lock);
    if (!ready) {
        u32 a, b, c, d;
        cpuid(1, 0, &a, &b, &c, &d);
        have_rdrand = c & (1u << 30);
        state[0] = rdtsc() | 1;
        state[1] = (rdtsc() * 0x9E3779B97F4A7C15ULL) ^ 0xD1B54A32D192ED03ULL;
        ready = true;
    }
    state[0] ^= rdtsc();
    u8 *p = buf;
    while (len) {
        u64 v = next(), r;
        if (have_rdrand && rdrand(&r)) v ^= r;
        for (int i = 0; i < 8 && len; i++, len--) {
            *p++ = (u8)v;
            v >>= 8;
        }
    }
    spin_unlock(&lock, f);
}
