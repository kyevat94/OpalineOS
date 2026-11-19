/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Local APIC (interrupt controller and timer of each CPU) and the legacy 8259 PIC. */
#ifndef OPALINE_ARCH_APIC_H
#define OPALINE_ARCH_APIC_H

#include <opaline/types.h>

/* Remaps the legacy PIC away from the exception vectors and masks all its lines. */
void pic_disable(void);

/* Enables the local APIC of this CPU. */
void lapic_init(void);
void lapic_eoi(void);
u32 lapic_id(void);

/*
 * Measures the LAPIC timer against the PIT and starts it in periodic mode,
 * firing `vector` `hz` times per second. Returns the timer frequency in Hz.
 */
u64 lapic_timer_start(u8 vector, u32 hz);

#endif
