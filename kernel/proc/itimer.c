/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Interval timers: setitimer(), getitimer() and alarm(), with Linux semantics.
 *
 * Each process has three: ITIMER_REAL counts real time and sends SIGALRM,
 * ITIMER_VIRTUAL counts the time its threads run in user mode (SIGVTALRM) and
 * ITIMER_PROF the time they run in user or kernel mode (SIGPROF). Time is
 * counted in timer ticks (1 ms): every tick charges the thread that was running.
 * A timer with an interval starts again when it expires. The timers are kept
 * across execve, a fork child starts with none, and they end with the process.
 *
 * The state is only changed with interrupts disabled: the timer interrupt
 * (itimer_tick) counts the timers down.
 */
#include <opaline/errno.h>
#include <opaline/proc.h>
#include <opaline/spinlock.h>
#include <opaline/uvm.h>

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

#define US_PER_TICK (1000000 / TIMER_HZ)
#define MAX_SECONDS (1LL << 40)   /* longer times are clamped (about 35000 years) */

struct timeval_k {
    i64 tv_sec, tv_usec;
};

struct itimerval_k {   /* struct itimerval of Linux x86-64 */
    struct timeval_k interval, value;
};

static const int timer_signal[3] = {SIGALRM, SIGVTALRM, SIGPROF};

static struct process *real_list;   /* processes whose ITIMER_REAL runs, through itimer_next */

static struct process *self(void) { return thread_current()->process; }

/* A time as ticks, rounded up (a time that is not 0 never becomes 0). */
static int to_ticks(const struct timeval_k *tv, u64 *ticks) {
    if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000) return -EINVAL;
    i64 sec = tv->tv_sec < MAX_SECONDS ? tv->tv_sec : MAX_SECONDS;
    *ticks = (u64)sec * TIMER_HZ + ((u64)tv->tv_usec + US_PER_TICK - 1) / US_PER_TICK;
    return 0;
}

static struct timeval_k from_ticks(u64 ticks) {
    return (struct timeval_k){(i64)(ticks / TIMER_HZ), (i64)(ticks % TIMER_HZ * US_PER_TICK)};
}

static void real_unlink(struct process *p) {
    for (struct process **q = &real_list; *q; q = &(*q)->itimer_next)
        if (*q == p) {
            *q = p->itimer_next;
            break;
        }
    p->itimer_next = NULL;
}

/* Interrupts disabled. The time left (never 0 while the timer runs) and the interval. */
static struct itimerval_k get_timer(struct process *p, int which, u64 now) {
    const struct itimer *it = &p->itimers[which];
    u64 left = it->value;
    if (which == ITIMER_REAL && left) left = left > now ? left - now : 1;
    return (struct itimerval_k){from_ticks(it->interval), from_ticks(left)};
}

/* Interrupts disabled. value 0 stops the timer. */
static void set_timer(struct process *p, int which, u64 value, u64 interval, u64 now) {
    struct itimer *it = &p->itimers[which];
    if (which == ITIMER_REAL) {
        if (it->value) real_unlink(p);
        if (value) {
            p->itimer_next = real_list;
            real_list = p;
        }
        it->value = value ? now + value : 0;
    } else {
        it->value = value;
    }
    it->interval = interval;
}

static bool user_ok(u64 addr, u64 len, bool write) { return mm_user_access(self()->mm, addr, len, write); }

i64 sys_setitimer(u64 which, u64 new_value, u64 old_value) {
    if (which > ITIMER_PROF) return -EINVAL;
    struct itimerval_k nv = {0};   /* NULL stops the timer, as on Linux */
    if (new_value) {
        if (!user_ok(new_value, sizeof nv, false)) return -EFAULT;
        nv = *(const struct itimerval_k *)new_value;
    }
    if (old_value && !user_ok(old_value, sizeof nv, true)) return -EFAULT;
    u64 value, interval;
    int err = to_ticks(&nv.value, &value);
    if (!err) err = to_ticks(&nv.interval, &interval);
    if (err) return err;
    struct process *p = self();
    u64 f = irq_save();
    u64 now = sched_ticks();
    struct itimerval_k old = get_timer(p, (int)which, now);
    set_timer(p, (int)which, value, interval, now);
    irq_restore(f);
    if (old_value) *(struct itimerval_k *)old_value = old;
    return 0;
}

i64 sys_getitimer(u64 which, u64 value) {
    if (which > ITIMER_PROF) return -EINVAL;
    if (!user_ok(value, sizeof(struct itimerval_k), true)) return -EFAULT;
    u64 f = irq_save();
    struct itimerval_k cur = get_timer(self(), (int)which, sched_ticks());
    irq_restore(f);
    *(struct itimerval_k *)value = cur;
    return 0;
}

/* alarm(): ITIMER_REAL in whole seconds; returns the seconds the previous one had left,
   rounded to the nearest (but at least 1 if it was running), as Linux does. */
i64 sys_alarm(u64 seconds) {
    struct process *p = self();
    u64 f = irq_save();
    u64 now = sched_ticks();
    struct itimerval_k old = get_timer(p, ITIMER_REAL, now);
    set_timer(p, ITIMER_REAL, (u64)(u32)seconds * TIMER_HZ, 0, now);
    irq_restore(f);
    i64 left = old.value.tv_sec;
    if ((!left && old.value.tv_usec) || old.value.tv_usec >= 500000) left++;
    return left;
}

/* ITIMER_VIRTUAL or ITIMER_PROF: one more tick of p's time. */
static void count_tick(struct process *p, int which) {
    struct itimer *it = &p->itimers[which];
    if (!it->value || --it->value) return;
    it->value = it->interval;
    signal_send_process(p, timer_signal[which]);
}

void itimer_tick(u64 now, struct process *p, bool user_mode) {
    for (struct process **q = &real_list; *q;) {
        struct process *r = *q;
        struct itimer *it = &r->itimers[ITIMER_REAL];
        if (it->value > now) {
            q = &r->itimer_next;
            continue;
        }
        if (it->interval) {   /* periodic: the next expiry keeps the period (a missed one is dropped) */
            it->value += it->interval;
            if (it->value <= now) it->value = now + it->interval;
            q = &r->itimer_next;
        } else {
            it->value = 0;
            *q = r->itimer_next;
            r->itimer_next = NULL;
        }
        signal_send_process(r, SIGALRM);
    }
    if (!p || !p->user || p->state != PROCESS_ALIVE) return;
    count_tick(p, ITIMER_PROF);
    if (user_mode) count_tick(p, ITIMER_VIRTUAL);
}

void itimer_cancel_all(struct process *p) {
    u64 f = irq_save();
    if (p->itimers[ITIMER_REAL].value) real_unlink(p);
    for (int i = 0; i < 3; i++) p->itimers[i] = (struct itimer){0, 0};
    irq_restore(f);
}
