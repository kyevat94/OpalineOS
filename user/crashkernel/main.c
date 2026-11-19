/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Reads kernel memory (the kernel image is mapped, but not for user mode). */
#include <opaline/sys.h>

int main(void) {
    printf("crashkernel: reading kernel memory...\n");
    __asm__ volatile("mov (%0), %%rax" : : "r"(0xFFFFFFFF80000000UL) : "rax", "memory");
    printf("crashkernel: still alive (this must not happen)\n");
    return 0;
}
