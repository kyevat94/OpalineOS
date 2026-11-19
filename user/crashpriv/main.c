/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Executes HLT, an instruction only the kernel may use. */
#include <opaline/sys.h>

int main(void) {
    printf("crashpriv: executing a privileged instruction (hlt)...\n");
    __asm__ volatile("hlt");
    printf("crashpriv: still alive (this must not happen)\n");
    return 0;
}
