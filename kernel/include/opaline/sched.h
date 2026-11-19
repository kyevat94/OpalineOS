/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Processes, threads and the scheduler (kernel/sched/sched.c).
 *
 * A process owns an address space; threads run inside a process. Until stage
 * E4 there is only the kernel process (pid 0) and its kernel threads.
 *
 * Scheduling is preemptive round-robin: the LAPIC timer interrupts 1000 times
 * per second and a thread that used up its time slice (10 ms) goes to the back
 * of the ready queue. When nothing is ready, the idle thread halts the CPU.
 */
#ifndef OPALINE_SCHED_H
#define OPALINE_SCHED_H

#include <opaline/types.h>

#define TIMER_HZ        1000
#define TIME_SLICE_MS   10
#define THREAD_STACK_PAGES 4      /* 16 KiB kernel stack per thread */

enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_SLEEPING,
    THREAD_BLOCKED,
    THREAD_DEAD,
};

struct process {
    u64 pid;
    char name[32];
    u64 page_table;           /* physical address of the PML4 */
    u64 thread_count;
};

struct thread {
    u64 id;
    char name[32];
    enum thread_state state;
    struct process *process;
    u64 rsp;                  /* saved stack pointer while not running */
    u64 stack_top;            /* 0 for the boot thread (static stack) */
    u64 wake_tick;            /* when a sleeping thread becomes ready */
    int exit_code;
    bool joinable;
    struct thread *joiner;    /* thread waiting in thread_join */
    struct thread *next;      /* run queue / sleep list / dead list */
    struct thread *all_next;  /* list of every thread */
};

typedef void (*thread_fn)(void *arg);

/* Turns the boot context into the first thread ("kmain") and creates the idle thread. */
void sched_init(void);
/* Called by the timer interrupt (interrupts disabled). */
void sched_tick(void);

struct thread *thread_create(const char *name, thread_fn fn, void *arg);   /* NULL if out of memory */
struct thread *thread_current(void);
void thread_yield(void);
void thread_sleep_ms(u64 ms);
NORETURN void thread_exit(int code);
int thread_join(struct thread *t);    /* waits for t to finish, frees it, returns its exit code */
void thread_detach(struct thread *t); /* nobody will join: freed automatically when it exits */

u64 sched_ticks(void);                /* timer ticks since the timer started */
static inline u64 time_ms(void) { return sched_ticks() * 1000 / TIMER_HZ; }
u64 sched_context_switches(void);

bool sched_selftest(void);            /* kernel/sched/selftest.c */

#endif
