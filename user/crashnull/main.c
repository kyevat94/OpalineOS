/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Reads address 0. The kernel must stop this process and keep running. */
#include <opaline/sys.h>

int main(void) {
    printf("crashnull: reading a NULL pointer...\n");
    __asm__ volatile("mov (%0), %%rax" : : "r"(0UL) : "rax", "memory");
    printf("crashnull: still alive (this must not happen)\n");
    return 0;
}
