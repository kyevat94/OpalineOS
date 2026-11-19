/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Scheduler self-test, run on every boot:
 *   1. preemption: two threads that never yield must still alternate;
 *   2. sleeping: thread_sleep_ms(50) takes about 50 ms;
 *   3. many threads: 32 threads add to a shared counter under a lock;
 *   4. cleanup: all their stacks and structures are freed afterwards.
 */
#include <opaline/console.h>
#include <opaline/mm.h>
#include <opaline/sched.h>
#include <opaline/spinlock.h>

#define FAIL(...)                          \
    do {                                   \
        console_set_color(COLOR_ERROR);    \
        kprintf("  FAILED: " __VA_ARGS__); \
        kprintf("\n");                     \
        return false;                      \
    } while (0)

/* ---- 1. preemption ------------------------------------------------------ */

#define TIMELINE_MAX 48
static char timeline[TIMELINE_MAX + 1];
static volatile u64 alternations;
static volatile int last_runner = -1;
static volatile u64 work[2];

static void busy_worker(void *arg) {
    int me = (int)(u64)arg;
    u64 end = time_ms() + 300;
    while (time_ms() < end) {           /* never yields: only the timer can switch away */
        work[me]++;
        if (last_runner != me) {
            u64 f = irq_save();
            if (alternations < TIMELINE_MAX) timeline[alternations] = (char)('A' + me);
            alternations++;
            last_runner = me;
            irq_restore(f);
        }
    }
}

/* ---- 3. many threads ---------------------------------------------------- */

static spinlock_t counter_lock = SPINLOCK_INIT;
static u64 counter;

static void counter_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        u64 f = spin_lock(&counter_lock);
        counter++;
        spin_unlock(&counter_lock, f);
        if (i % 100 == 99) thread_yield();
    }
}

static void exit_worker(void *arg) { thread_exit((int)(u64)arg); }

bool sched_selftest(void) {
    struct pmm_stats before, after;
    pmm_get_stats(&before);
    u64 switches_before = sched_context_switches();

    /* 1. preemption */
    struct thread *a = thread_create("worker-A", busy_worker, (void *)0);
    struct thread *b = thread_create("worker-B", busy_worker, (void *)1);
    if (!a || !b) FAIL("thread_create failed");
    thread_join(a);
    thread_join(b);
    timeline[MIN(alternations, (u64)TIMELINE_MAX)] = 0;
    if (alternations < 6 || !work[0] || !work[1])
        FAIL("threads did not alternate (%lu switches, timeline \"%s\")", alternations, timeline);

    /* 2. sleeping */
    u64 t0 = time_ms();
    thread_sleep_ms(50);
    u64 slept = time_ms() - t0;
    if (slept < 50 || slept > 80) FAIL("thread_sleep_ms(50) took %lu ms", slept);

    /* 3. many threads */
    enum { N = 32 };
    struct thread *t[N];
    for (int i = 0; i < N; i++) {
        t[i] = thread_create("counter", counter_worker, NULL);
        if (!t[i]) FAIL("thread_create %d failed", i);
    }
    for (int i = 0; i < N; i++) thread_join(t[i]);
    if (counter != N * 1000) FAIL("shared counter is %lu, expected %d", counter, N * 1000);

    /* exit codes travel to thread_join */
    struct thread *e = thread_create("exit-code", exit_worker, (void *)42);
    if (!e || thread_join(e) != 42) FAIL("exit code was not passed to thread_join");

    /* 4. cleanup */
    pmm_get_stats(&after);
    struct heap_stats hs;
    heap_get_stats(&hs);
    if (before.free_pages - after.free_pages > hs.slab_pages)
        FAIL("pages lost: %lu free before, %lu after", before.free_pages, after.free_pages);

    kprintf("Threads A and B alternating without yielding: %s...\n", timeline);
    kprintf("Scheduler self-test passed: %lu alternations in 300 ms, sleep %lu ms, %d threads, "
            "%lu context switches\n", alternations, slept, N + 3, sched_context_switches() - switches_before);
    return true;
}
