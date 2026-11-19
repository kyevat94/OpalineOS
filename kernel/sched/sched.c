/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Scheduler: preemptive round-robin over a FIFO of ready threads.
 *
 * All scheduler state is changed with interrupts disabled (one CPU), either
 * inside the timer interrupt or between irq_save()/irq_restore(). schedule()
 * must be called with interrupts disabled; it may switch to another thread
 * and return much later, when this thread is picked again.
 *
 * Dead threads cannot free their own stack (they are running on it), so they
 * go to a dead list: thread_join() frees a joinable thread, and the idle
 * thread frees detached ones.
 */
#include <opaline/arch/cpu.h>
#include <opaline/arch/gdt.h>
#include <opaline/arch/syscall.h>
#include <opaline/console.h>
#include <opaline/mm.h>
#include <opaline/panic.h>
#include <opaline/sched.h>
#include <opaline/spinlock.h>
#include <opaline/string.h>

void switch_context(u64 *old_rsp, u64 new_rsp);
void thread_trampoline(void);

static struct process kernel_process = {.pid = 0, .name = "kernel"};

static struct thread boot_thread;        /* kmain, on the static boot stack */
static struct thread *current;
static struct thread *idle_thread;
static struct thread *ready_head, *ready_tail;
static struct thread *sleeping;          /* unsorted list */
static struct thread *dead;
static struct thread *all_threads;

static volatile u64 ticks;
static u64 slice_left;
static u64 next_id = 1;
static u64 switches;

/* ---- queues (interrupts disabled) ---------------------------------------- */

static void ready_push(struct thread *t) {
    t->state = THREAD_READY;
    t->next = NULL;
    if (ready_tail) ready_tail->next = t;
    else ready_head = t;
    ready_tail = t;
}

static struct thread *ready_pop(void) {
    struct thread *t = ready_head;
    if (t) {
        ready_head = t->next;
        if (!ready_head) ready_tail = NULL;
        t->next = NULL;
    }
    return t;
}

static void all_remove(struct thread *t) {
    for (struct thread **p = &all_threads; *p; p = &(*p)->all_next)
        if (*p == t) {
            *p = t->all_next;
            return;
        }
}

static void dead_remove(struct thread *t) {
    for (struct thread **p = &dead; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            return;
        }
}

/* ---- core ---------------------------------------------------------------- */

/* Picks the next thread and switches to it. Interrupts must be disabled. */
static void schedule(void) {
    struct thread *old = current;
    if (old->state == THREAD_RUNNING && old != idle_thread) ready_push(old);
    struct thread *next = ready_pop();
    if (!next) next = old->state == THREAD_RUNNING ? old : idle_thread;
    slice_left = TIME_SLICE_MS * TIMER_HZ / 1000;
    if (next == old) {
        old->state = THREAD_RUNNING;
        return;
    }
    next->state = THREAD_RUNNING;
    current = next;
    switches++;
    if (next->stack_top) {
        tss_set_kernel_stack(next->stack_top);    /* interrupts from user mode land here */
        cpu_set_syscall_stack(next->stack_top);   /* and so do system calls */
    }
    u64 page_table = next->process->page_table;
    if (page_table && read_cr3() != page_table) write_cr3(page_table);
    /* the kernel itself never touches FPU/SSE registers, so they only change here */
    fxsave(old->fpu);
    fxrstor(next->fpu);
    switch_context(&old->rsp, next->rsp);
}

void sched_tick(void) {
    ticks++;
    for (struct thread **p = &sleeping; *p;) {   /* wake every sleeper whose time came */
        struct thread *t = *p;
        if (t->wake_tick <= ticks) {
            *p = t->next;
            ready_push(t);
        } else {
            p = &t->next;
        }
    }
    if (!current) return;   /* scheduler not started yet */
    if (current == idle_thread ? ready_head != NULL : --slice_left == 0) schedule();
}

/* ---- threads ------------------------------------------------------------- */

static void free_thread(struct thread *t) {
    all_remove(t);
    if (t->process) t->process->thread_count--;
    if (t->stack_top) vmm_free_stack(t->stack_top, THREAD_STACK_PAGES);
    kfree(t);
}

static void idle_main(void *arg) {
    (void)arg;
    for (;;) {
        u64 f = irq_save();
        for (struct thread **p = &dead; *p;) {   /* free detached threads that finished */
            struct thread *t = *p;
            if (!t->joinable) {
                *p = t->next;
                free_thread(t);
            } else {
                p = &t->next;
            }
        }
        irq_restore(f);
        __asm__ volatile("sti\n\thlt" ::: "memory");
    }
}

/* FPU state of a new thread: x87 and SSE reset to their defaults (taken once in sched_init). */
static u8 fpu_initial[512] ALIGNED(16);

struct thread *thread_create(const char *name, thread_fn fn, void *arg) {
    return thread_create_in(&kernel_process, name, fn, arg);
}

struct process *sched_kernel_process(void) { return &kernel_process; }

struct thread *thread_create_in(struct process *p, const char *name, thread_fn fn, void *arg) {
    struct thread *t = kcalloc(1, sizeof *t);
    if (!t) return NULL;
    t->stack_top = vmm_alloc_stack(THREAD_STACK_PAGES);
    if (!t->stack_top) {
        kfree(t);
        return NULL;
    }
    for (size_t i = 0; i < sizeof t->name - 1 && name[i]; i++) t->name[i] = name[i];
    t->process = p;
    t->joinable = true;
    memcpy(t->fpu, fpu_initial, sizeof t->fpu);

    /* initial frame, popped by switch_context: r15 r14 r13 r12 rbx rbp, then the return address */
    u64 *sp = (u64 *)t->stack_top;
    *--sp = (u64)thread_trampoline;
    *--sp = 0;            /* rbp: ends stack traces */
    *--sp = 0;            /* rbx */
    *--sp = (u64)fn;      /* r12 */
    *--sp = (u64)arg;     /* r13 */
    *--sp = 0;            /* r14 */
    *--sp = 0;            /* r15 */
    t->rsp = (u64)sp;

    u64 f = irq_save();
    t->id = next_id++;
    t->all_next = all_threads;
    all_threads = t;
    p->thread_count++;
    ready_push(t);
    irq_restore(f);
    return t;
}

struct thread *thread_current(void) { return current; }

void thread_yield(void) {
    u64 f = irq_save();
    schedule();
    irq_restore(f);
}

void thread_sleep_ms(u64 ms) {
    if (!ms) {
        thread_yield();
        return;
    }
    u64 f = irq_save();
    current->state = THREAD_SLEEPING;
    current->wake_tick = ticks + (ms * TIMER_HZ + 999) / 1000;
    current->next = sleeping;
    sleeping = current;
    schedule();
    irq_restore(f);
}

void thread_exit(int code) {
    irq_save();
    if (current == &boot_thread || current == idle_thread) panic("thread_exit: \"%s\" may not exit", current->name);
    current->exit_code = code;
    current->state = THREAD_DEAD;
    current->next = dead;
    dead = current;
    if (current->joiner) ready_push(current->joiner);
    schedule();
    panic("thread_exit: a dead thread was scheduled again");
}

int thread_join(struct thread *t) {
    if (t == current) panic("thread_join: a thread cannot join itself");
    u64 f = irq_save();
    if (!t->joinable) panic("thread_join: thread %lu is detached", t->id);
    while (t->state != THREAD_DEAD) {
        t->joiner = current;
        current->state = THREAD_BLOCKED;
        schedule();
    }
    int code = t->exit_code;
    dead_remove(t);
    free_thread(t);
    irq_restore(f);
    return code;
}

void thread_detach(struct thread *t) {
    u64 f = irq_save();
    t->joinable = false;   /* the idle thread frees it once it is dead */
    irq_restore(f);
}

u64 sched_ticks(void) { return ticks; }
u64 sched_context_switches(void) { return switches; }

void sched_init(void) {
    /* a clean FPU/SSE state for new threads: x87 reset, SSE exceptions masked (MXCSR 0x1F80) */
    u32 mxcsr = 0x1F80;
    __asm__ volatile("fninit\n\tldmxcsr %0" : : "m"(mxcsr));
    fxsave(fpu_initial);

    kernel_process.page_table = read_cr3();
    kernel_process.main_thread = &boot_thread;
    memcpy(boot_thread.name, "kmain", 6);
    boot_thread.id = 0;
    boot_thread.state = THREAD_RUNNING;
    boot_thread.process = &kernel_process;
    boot_thread.all_next = NULL;
    all_threads = &boot_thread;
    kernel_process.thread_count = 1;

    idle_thread = thread_create("idle", idle_main, NULL);
    if (!idle_thread) panic("sched_init: cannot create the idle thread");
    u64 f = irq_save();
    ready_head = ready_tail = NULL;      /* idle is never in the ready queue */
    idle_thread->state = THREAD_READY;
    idle_thread->joinable = false;
    slice_left = TIME_SLICE_MS * TIMER_HZ / 1000;
    current = &boot_thread;
    irq_restore(f);
}
