/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Turning the computer off and restarting it, through ACPI (kernel/dev/acpi.c). */
#ifndef OPALINE_POWER_H
#define OPALINE_POWER_H

#include <opaline/types.h>

/* Reads the ACPI tables (FADT and the \_S5 sleep state in the DSDT). */
void acpi_init(u64 rsdp_phys);

/* Flushes the disks, then switches the machine off / restarts it. */
NORETURN void system_poweroff(void);
NORETURN void system_reboot(void);

#endif
