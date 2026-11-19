/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* The first OpalineOS user program. Two copies run at the same time during the self-test. */
#include <opaline/sys.h>

int main(void) {
    long pid = getpid();
    for (int i = 1; i <= 3; i++) {
        printf("[hello pid %ld] Olá do espaço de usuário! Hello from user space (%d/3)\n", pid, i);
        sleep_ms(20);
    }
    return 0;
}
