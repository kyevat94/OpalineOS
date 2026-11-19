/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/* Checks the system calls from user mode. Exit code 0 = everything passed. */
#include <opaline/sys.h>

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("usertest: FAILED: %s\n", what);
        failures++;
    }
}

static volatile double sse_value = 1.5;   /* uses SSE registers: their state must survive switches */

int main(void) {
    const char msg[] = "usertest: write() from ring 3 works\n";
    check(write(1, msg, sizeof msg - 1) == (long)(sizeof msg - 1), "write returns the byte count");

    check(syscall0(9999) == -ENOSYS, "an unknown system call returns -ENOSYS");
    check(write(1, (const void *)0xFFFF800000000000UL, 16) == -EFAULT, "write from a kernel address is refused");
    check(write(1, (const void *)0x1000, 16) == -EFAULT, "write from an unmapped address is refused");
    check(write(7, msg, 4) == -EBADF, "write to an unknown file descriptor fails");
    check(getpid() > 0 && gettid() > 0, "getpid and gettid");
    check(sched_yield() == 0, "sched_yield");

    long t0 = time_ms();
    sleep_ms(30);
    long slept = time_ms() - t0;
    check(slept >= 30 && slept < 1000, "nanosleep(30 ms) is never early");

    double x = sse_value;
    for (int i = 0; i < 1000; i++) {
        x = x * 1.0001 + 0.5;
        if (i % 100 == 0) sched_yield();   /* other threads run in between */
    }
    check(x > 500.0 && x < 1000.0, "floating point (SSE) state is preserved");

    struct timespec bad = {0, 2000000000};
    check(nanosleep(&bad, NULL) == -EINVAL, "nanosleep rejects an invalid time");

    if (failures == 0) printf("usertest: all checks passed (pid %ld, slept %ld ms)\n", getpid(), slept);
    return failures;
}
