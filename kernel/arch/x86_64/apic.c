/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
#include <opaline/arch/apic.h>
#include <opaline/arch/cpu.h>
#include <opaline/arch/idt.h>
#include <opaline/mm.h>
#include <opaline/panic.h>

#define MSR_APIC_BASE      0x1B
#define APIC_BASE_ENABLE   (1ULL << 11)

/* LAPIC registers (byte offsets) */
#define LAPIC_ID           0x020
#define LAPIC_TPR          0x080
#define LAPIC_EOI          0x0B0
#define LAPIC_SVR          0x0F0
#define LAPIC_LVT_TIMER    0x320
#define LAPIC_TIMER_INIT   0x380
#define LAPIC_TIMER_COUNT  0x390
#define LAPIC_TIMER_DIV    0x3E0

#define LVT_MASKED         (1u << 16)
#define LVT_PERIODIC       (1u << 17)
#define TIMER_DIVIDE_16    0x3

/* 8254 PIT */
#define PIT_HZ             1193182
#define PIT_CH2_DATA       0x42
#define PIT_COMMAND        0x43
#define PIT_GATE_PORT      0x61    /* bit 0: channel 2 gate, bit 1: speaker, bit 5: channel 2 output */

static volatile u32 *lapic;

static inline u32 lapic_read(u32 reg) { return lapic[reg / 4]; }
static inline void lapic_write(u32 reg, u32 value) { lapic[reg / 4] = value; }

void pic_disable(void) {
    /* initialize both PICs (ICW1-4) so they use vectors 0x30-0x3F instead of 0x08-0x0F */
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, VECTOR_PIC_BASE);
    outb(0xA1, VECTOR_PIC_BASE + 8);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    /* mask every line: interrupts come through the APIC */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void lapic_init(void) {
    u32 a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1u << 9))) panic("this CPU has no local APIC");
    u64 base = rdmsr(MSR_APIC_BASE);
    wrmsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE);
    lapic = vmm_map_mmio(base & 0xFFFFFFFFFF000ULL, PAGE_SIZE);
    lapic_write(LAPIC_TPR, 0);                          /* accept every priority */
    lapic_write(LAPIC_SVR, 0x100 | VECTOR_SPURIOUS);    /* software enable + spurious vector */
}

void lapic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

u32 lapic_id(void) { return lapic_read(LAPIC_ID) >> 24; }

#define CALIBRATION_MS   25
#define CALIBRATION_RUNS 5

/* Counts LAPIC timer ticks during CALIBRATION_MS measured by PIT channel 2 (interrupts must be off). */
static u64 measure_ticks(void) {
    const u32 pit_count = PIT_HZ / (1000 / CALIBRATION_MS);
    u8 gate = inb(PIT_GATE_PORT);
    outb(PIT_GATE_PORT, (u8)((gate & ~0x02) | 0x01));   /* speaker off, gate on */
    outb(PIT_COMMAND, 0xB0);                             /* channel 2, lo/hi byte, mode 0 */
    outb(PIT_CH2_DATA, pit_count & 0xFF);
    outb(PIT_CH2_DATA, (u8)(pit_count >> 8));
    gate = inb(PIT_GATE_PORT) & ~0x01;
    outb(PIT_GATE_PORT, gate);                           /* restart the count: gate low, then high */
    outb(PIT_GATE_PORT, gate | 0x01);

    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
    for (u64 spin = 0; !(inb(PIT_GATE_PORT) & 0x20); spin++)
        if (spin > 100000000ULL) panic("timer calibration: the PIT does not count");
    return 0xFFFFFFFFu - lapic_read(LAPIC_TIMER_COUNT);
}

u64 lapic_timer_start(u8 vector, u32 hz) {
    lapic_write(LAPIC_TIMER_DIV, TIMER_DIVIDE_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | vector);

    /* median of five 25 ms measurements: in a virtual machine a single one can be far off
       when the host pauses the guest in the middle of it */
    u64 m[CALIBRATION_RUNS];
    for (int i = 0; i < CALIBRATION_RUNS; i++) {
        u64 t = measure_ticks();
        int j = i;
        for (; j > 0 && m[j - 1] > t; j--) m[j] = m[j - 1];   /* insertion sort */
        m[j] = t;
    }
    lapic_write(LAPIC_TIMER_INIT, 0);
    u64 median = m[CALIBRATION_RUNS / 2];
    u64 ticks_per_second = median * (1000 / CALIBRATION_MS);
    if (ticks_per_second < hz) panic("timer calibration failed (%lu ticks per %d ms)", median, CALIBRATION_MS);

    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | vector);
    lapic_write(LAPIC_TIMER_INIT, (u32)(ticks_per_second / hz));
    return ticks_per_second * 16;   /* undo the divide-by-16 */
}
